// conc front end: from an LLVM module to a concurrent program over PIR.
//
// The module is copied and rewritten so that pir::translate (which has no
// memory model yet) can translate every thread body. Each visible operation
// becomes a *marker* whose result name is "__prism.conc.<k>" (k indexes
// Program::ops):
//
//   load  @g              -> %__prism.conc.k = call iN @__VERIFIER_nondet_conc()   (value = read)
//   store v, @g           -> %__prism.conc.k = call iN @llvm.expect.iN(iN v, iN 0)  (copy = written value)
//   atomicrmw op @g, v    -> read marker k, new = op(old, v), copy marker "k s" (same atomic step)
//   cmpxchg @g, c, n      -> read marker k, eq/select, copy marker "k s"
//   lock/unlock/init/create/join -> copy marker (join copies the thread handle)
//   trylock               -> read marker (the encoder defines the result)
//
// A marker is a Havoc (nondet call) or a Copy (llvm.expect) in PIR; the lazy
// encoder (lazy.cpp) recognises it by name and gives it its concurrent
// meaning. All other instructions go through translate unchanged, so the PIR
// UB checks and assertions apply in every thread.
//
// Not modelled (NEEDS-HARNESS "UNENCODED: ..."): shared data that is not a
// scalar integer global (heap, arrays, structs, pointers: waits for the PIR
// memory model, roadmap 2.5), non-seq_cst atomics, thread arguments,
// std::thread, mutexes that are not globals, nested thread creation.

#include "prism/conc.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace prism::conc {

namespace ir = pir::ir;

namespace {

struct Unenc {
    std::string reason;
};

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// Split at top-level commas (outside () [] {} <> and quotes).
std::vector<std::string> split_top(std::string_view s) {
    std::vector<std::string> out;
    int depth = 0;
    bool q = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') q = !q;
        if (q) continue;
        if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
        if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim(s.substr(start)));
    return out;
}

// Instruction body without "%r = " and without trailing ", align N" /
// ", !md !N" parts.
std::vector<std::string> inst_parts(const std::string& text) {
    std::string body = text;
    if (!body.empty() && body[0] == '%') {
        auto eq = body.find(" = ");
        if (eq != std::string::npos) body = body.substr(eq + 3);
    }
    auto parts = split_top(body);
    while (!parts.empty() && (parts.back().starts_with("align ") || parts.back().starts_with("!")))
        parts.pop_back();
    return parts;
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        int depth = 0;
        bool q = false;
        while (j < s.size()) {
            char c = s[j];
            if (c == '"') q = !q;
            if (!q) {
                if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
                if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
                if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
            }
            ++j;
        }
        out.push_back(std::string(s.substr(i, j - i)));
        i = j;
    }
    return out;
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

// "@name" -> "name"; "" if not a plain global reference.
std::string global_ref(const std::string& w) {
    if (w.size() < 2 || w[0] != '@') return {};
    return unquote(w.substr(1));
}

bool is_order(const std::string& w) {
    return w == "unordered" || w == "monotonic" || w == "acquire" || w == "release" || w == "acq_rel" ||
           w == "seq_cst";
}

std::string c_order_name(const std::string& w) {
    if (w == "monotonic" || w == "unordered") return "memory_order_relaxed";
    if (w == "acquire") return "memory_order_acquire (or consume)";
    if (w == "release") return "memory_order_release";
    if (w == "acq_rel") return "memory_order_acq_rel";
    return w;
}

void require_sc(const std::vector<std::string>& ws) {
    for (auto& w : ws) {
        if (w.starts_with("syncscope(")) throw Unenc{"UNENCODED: atomic syncscope " + w};
        if (is_order(w) && w != "seq_cst")
            throw Unenc{"UNENCODED: " + c_order_name(w) + " (SC only: relaxed/acquire/release atomics are not encoded yet)"};
    }
}

ir::Value parse_value(const std::string& w, const ir::Type& ty) {
    ir::Value v;
    v.text = w;
    if (w.empty()) return v;
    if (w[0] == '%') {
        v.kind = ir::Value::Local;
        v.name = unquote(w.substr(1));
        return v;
    }
    if (w[0] == '@') {
        v.kind = ir::Value::Global;
        v.name = unquote(w.substr(1));
        return v;
    }
    if (w == "true" || w == "false") {
        v.kind = ir::Value::Int;
        v.bits = w == "true" ? 1 : 0;
        return v;
    }
    if (w == "undef") {
        v.kind = ir::Value::Undef;
        return v;
    }
    if (w == "poison") {
        v.kind = ir::Value::Poison;
        return v;
    }
    if (w == "null") {
        v.kind = ir::Value::Null;
        return v;
    }
    if (w == "zeroinitializer") {
        v.kind = ty.kind == ir::Type::Int ? ir::Value::Int : ir::Value::Zero;
        return v;
    }
    bool neg = w[0] == '-';
    std::string_view d(w);
    if (neg) d.remove_prefix(1);
    if (!d.empty() && std::all_of(d.begin(), d.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
        d.size() <= 19) {
        uint64_t mag = std::stoull(std::string(d));
        v.kind = ir::Value::Int;
        v.negative = neg;
        v.bits = neg ? (~mag + 1) : mag;
        if (ty.kind == ir::Type::Int && ty.bits < 64) v.bits &= (uint64_t{1} << ty.bits) - 1;
        return v;
    }
    v.kind = ir::Value::Other;
    return v;
}

ir::Type int_type(unsigned w) {
    ir::Type t;
    t.kind = ir::Type::Int;
    t.bits = w;
    t.text = "i" + std::to_string(w);
    return t;
}

ir::Operand local(const std::string& name, const ir::Type& ty) {
    ir::Operand o;
    o.ty = ty;
    o.v.kind = ir::Value::Local;
    o.v.name = name;
    o.v.text = "%" + name;
    return o;
}

ir::Operand cint(uint64_t v, const ir::Type& ty) {
    ir::Operand o;
    o.ty = ty;
    o.v.kind = ir::Value::Int;
    o.v.bits = v;
    o.v.text = std::to_string(v);
    return o;
}

std::string marker(int k, bool tail = false) {
    return "__prism.conc." + std::to_string(k) + (tail ? "s" : "");
}

struct Rewriter {
    const ir::Module& src;
    ir::Module m;  // rewritten copy
    std::map<std::string, GlobalDef> globals;
    Program prog;
    std::map<std::string, int> var_index, mutex_index;
    std::map<std::string, int> handle_thread;       // "fn/%x" or "@g" -> thread index
    std::map<std::string, std::string> entry_of;    // thread entries (IR name) -> ""
    int fresh = 0;

    Rewriter(const ir::Module& s, std::string_view ir_text) : src(s), m(s) {
        for (auto& g : parse_globals(ir_text)) globals[g.name] = g;
    }

    int line_of(const ir::Inst& in) const {
        auto it = src.locs.find(in.dbg);
        return it == src.locs.end() ? 0 : it->second.line;
    }

    ir::Inst base(const ir::Inst& orig, std::string result, std::string op, ir::Type ty) {
        ir::Inst in;
        in.result = std::move(result);
        in.op = std::move(op);
        in.ty = std::move(ty);
        in.dbg = orig.dbg;
        in.text = orig.text;
        return in;
    }
    ir::Inst nondet(const ir::Inst& orig, const std::string& result, const ir::Type& ty) {
        auto in = base(orig, result, "call", ty);
        in.callee = "__VERIFIER_nondet_conc";
        return in;
    }
    ir::Inst copy(const ir::Inst& orig, const std::string& result, const ir::Operand& v) {
        auto in = base(orig, result, "call", v.ty);
        in.callee = "llvm.expect." + v.ty.text;
        in.ops = {v, cint(0, v.ty)};
        return in;
    }
    ir::Inst freeze(const ir::Inst& orig, const std::string& result, const ir::Operand& v) {
        auto in = base(orig, result, "freeze", v.ty);
        in.ops = {v};
        return in;
    }
    ir::Inst bin(const ir::Inst& orig, const std::string& result, const std::string& op, const ir::Operand& a,
                 const ir::Operand& b) {
        auto in = base(orig, result, op, a.ty);
        in.ops = {a, b};
        return in;
    }
    ir::Inst intrinsic(const ir::Inst& orig, const std::string& result, const std::string& name,
                       const ir::Operand& a, const ir::Operand& b) {
        auto in = base(orig, result, "call", a.ty);
        in.callee = name + "." + a.ty.text;
        in.ops = {a, b};
        return in;
    }
    // "%r = add iN 0, 0": defines the integer result of a replaced call
    void zero_result(const ir::Inst& orig, std::vector<ir::Inst>& out) {
        if (orig.result.empty() || orig.ty.kind != ir::Type::Int) return;
        out.push_back(bin(orig, orig.result, "add", cint(0, orig.ty), cint(0, orig.ty)));
    }

    int add_op(VisOp op) {
        prog.ops.push_back(std::move(op));
        return static_cast<int>(prog.ops.size()) - 1;
    }

    int shared_var(const std::string& g, const ir::Type& ty) {
        if (auto it = var_index.find(g); it != var_index.end()) {
            if (prog.vars[static_cast<std::size_t>(it->second)].width != ty.bits)
                throw Unenc{"UNENCODED: @" + g + " accessed at different widths (memory model)"};
            return it->second;
        }
        auto git = globals.find(g);
        if (git == globals.end()) throw Unenc{"UNENCODED: access to external global @" + g};
        const auto& def = git->second;
        auto gt = ir::parse_type(def.type);
        if (gt.kind != ir::Type::Int || gt.bits == 0 || gt.bits > 64)
            throw Unenc{"UNENCODED: shared " + def.type + " global @" + g +
                        " (only scalar integer globals until the PIR memory model lands)"};
        if (ty.kind != ir::Type::Int || ty.bits != gt.bits)
            throw Unenc{"UNENCODED: @" + g + " accessed as " + ty.text + " (memory model)"};
        Shared s;
        s.name = g;
        s.width = gt.bits;
        auto iv = parse_value(def.init, gt);
        if (def.init == "zeroinitializer" || def.init == "undef" || def.init == "poison") {
            s.init = 0;
        } else if (iv.kind == ir::Value::Int) {
            s.init = iv.bits;
        } else {
            throw Unenc{"UNENCODED: initialiser of @" + g + " (" + def.init + ")"};
        }
        prog.vars.push_back(s);
        var_index[g] = static_cast<int>(prog.vars.size()) - 1;
        return var_index[g];
    }

    int mutex(const ir::Operand& o, const std::string& fn) {
        if (o.v.kind != ir::Value::Global)
            throw Unenc{"UNENCODED: " + fn + " on a mutex that is not a global (needs the PIR memory model)"};
        if (auto it = mutex_index.find(o.v.name); it != mutex_index.end()) return it->second;
        if (var_index.count(o.v.name)) throw Unenc{"UNENCODED: @" + o.v.name + " used as data and as a mutex"};
        prog.mutexes.push_back(o.v.name);
        mutex_index[o.v.name] = static_cast<int>(prog.mutexes.size()) - 1;
        return mutex_index[o.v.name];
    }

    int atomic_mutex() {
        if (prog.atomic_mutex < 0) {
            prog.mutexes.push_back("__VERIFIER_atomic");
            prog.atomic_mutex = static_cast<int>(prog.mutexes.size()) - 1;
        }
        return prog.atomic_mutex;
    }

    std::string handle_key(const std::string& fn, const std::string& ptr) {
        if (ptr.starts_with("@")) return ptr;
        return fn + "/" + ptr;
    }

    // Pass 1: thread creation sites (they fix thread indices and handles).
    void find_creates(const std::set<std::string>& scope) {
        for (auto& f : src.functions) {
            if (!scope.count(f.name)) continue;
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    if (in.op != "call") continue;
                    bool p = in.callee == "pthread_create", t = in.callee == "thrd_create";
                    if (!p && !t) continue;
                    std::size_t fi = p ? 2 : 1;
                    if (in.ops.size() < fi + 1) throw Unenc{"UNENCODED: call @" + in.callee + " arity"};
                    const auto& h = in.ops[0].v;
                    const auto& fnv = in.ops[fi].v;
                    if (fnv.kind != ir::Value::Global || !src.find(fnv.name))
                        throw Unenc{"UNENCODED: " + in.callee + " of a function pointer that is not a defined function"};
                    std::string hk;
                    if (h.kind == ir::Value::Local) hk = handle_key(f.name, "%" + h.name);
                    else if (h.kind == ir::Value::Global) hk = "@" + h.name;
                    else throw Unenc{"UNENCODED: " + in.callee + " handle " + h.text};
                    if (handle_thread.count(hk))
                        throw Unenc{"UNENCODED: thread handle " + hk.substr(hk.find('/') + 1) +
                                    " reused for several threads (arrays/loops of threads need the PIR memory model)"};
                    Thread th;
                    th.entry = fnv.name;
                    th.name = fnv.name;
                    if (auto* ef = src.find(fnv.name)) {
                        auto it = src.subprograms.find(ef->dbg);
                        if (it != src.subprograms.end() && !it->second.name.empty()) th.name = it->second.name;
                    }
                    int idx = static_cast<int>(prog.threads.size());
                    VisOp op;
                    op.kind = VisOp::Create;
                    op.thread = idx;
                    op.line = line_of(in);
                    op.what = in.callee;
                    th.create_op = add_op(op);
                    prog.threads.push_back(std::move(th));
                    handle_thread[hk] = idx;
                    entry_of[fnv.name] = "";
                }
        }
    }

    int create_op_for(const std::string& fn, const ir::Inst& in) {
        const auto& h = in.ops[0].v;
        auto hk = h.kind == ir::Value::Local ? handle_key(fn, "%" + h.name) : "@" + h.name;
        int t = handle_thread.at(hk);
        return prog.threads[static_cast<std::size_t>(t)].create_op;
    }

    void rewrite_function(ir::Function& f, const std::string& orig_name) {
        std::map<std::string, std::pair<std::string, std::string>> cmpx;  // result -> (old, ok)
        for (auto& bl : f.blocks) {
            std::vector<ir::Inst> out;
            bool cut = false;
            for (auto& in : bl.insts) {
                if (cut) break;
                rewrite_inst(orig_name, f, in, out, cmpx, cut);
            }
            bl.insts = std::move(out);
        }
        // extractvalue of a rewritten cmpxchg
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                if (in.op != "extractvalue" || in.ops.empty() || in.ops[0].v.kind != ir::Value::Local) continue;
                auto it = cmpx.find(in.ops[0].v.name);
                if (it == cmpx.end()) continue;
                if (in.indices.size() != 1 || in.indices[0] > 1) throw Unenc{"UNENCODED: extractvalue of cmpxchg"};
                auto ty = in.indices[0] == 0 ? in.ty : int_type(1);
                auto name = in.indices[0] == 0 ? it->second.first : it->second.second;
                auto r = in.result;
                in = freeze(in, r, local(name, ty));
            }
    }

    void rewrite_inst(const std::string& fn, ir::Function& f, const ir::Inst& in, std::vector<ir::Inst>& out,
                      std::map<std::string, std::pair<std::string, std::string>>& cmpx, bool& cut) {
        const auto& op = in.op;
        int line = line_of(in);
        if (op == "alloca") {
            // thread handles live in allocas: drop them (their loads become constants)
            if (handle_thread.count(handle_key(fn, "%" + in.result))) return;
            out.push_back(in);
            return;
        }
        if (op == "fence") {
            require_sc(words(inst_parts(in.text)[0]));
            return;  // a seq_cst fence orders nothing more under SC
        }
        if (op == "load") {
            auto parts = inst_parts(in.text);
            if (parts.size() < 2) throw Unenc{"UNENCODED: load form " + in.text};
            auto w0 = words(parts[0]);  // load [atomic] [volatile] T
            auto w1 = words(parts[1]);  // ptr P [syncscope] [order]
            bool atomic = std::find(w0.begin(), w0.end(), "atomic") != w0.end();
            std::string tys = w0.back();
            auto ty = ir::parse_type(tys);
            if (w1.size() < 2) throw Unenc{"UNENCODED: load form " + in.text};
            auto g = global_ref(w1[1]);
            if (!g.empty() && handle_thread.count("@" + g)) {
                out.push_back(bin(in, in.result, "add", cint(0, ty),
                                  cint(static_cast<uint64_t>(handle_thread["@" + g]), ty)));
                return;
            }
            if (w1[1].starts_with("%") && handle_thread.count(handle_key(fn, w1[1]))) {
                out.push_back(bin(in, in.result, "add", cint(0, ty),
                                  cint(static_cast<uint64_t>(handle_thread[handle_key(fn, w1[1])]), ty)));
                return;
            }
            if (g.empty()) {
                if (w1[1].starts_with("%")) {
                    out.push_back(in);  // local memory: translate reports it (memory model)
                    return;
                }
                throw Unenc{"UNENCODED: load from " + w1[1].substr(0, 40) +
                            " (shared aggregates need the PIR memory model)"};
            }
            if (atomic) require_sc(w1);
            int v = shared_var(g, ty);
            VisOp vo;
            vo.kind = VisOp::Load;
            vo.var = v;
            vo.atomic = atomic;
            vo.width = ty.bits;
            vo.line = line;
            vo.what = atomic ? "atomic load" : "load";
            int k = add_op(vo);
            out.push_back(nondet(in, marker(k), ty));
            if (!in.result.empty()) out.push_back(freeze(in, in.result, local(marker(k), ty)));
            return;
        }
        if (op == "store") {
            auto parts = inst_parts(in.text);
            if (parts.size() < 2) throw Unenc{"UNENCODED: store form " + in.text};
            auto w0 = words(parts[0]);  // store [atomic] [volatile] T V
            auto w1 = words(parts[1]);
            bool atomic = std::find(w0.begin(), w0.end(), "atomic") != w0.end();
            if (w0.size() < 3 || w1.size() < 2) throw Unenc{"UNENCODED: store form " + in.text};
            auto ty = ir::parse_type(w0[w0.size() - 2]);
            auto g = global_ref(w1[1]);
            if (w1[1].starts_with("%") && handle_thread.count(handle_key(fn, w1[1])) &&
                w0.back().starts_with("%__prism_uninit."))
                return;  // the pir front end's uninitialised-local marker; pthread_create writes the handle
            if (g.empty() && w1[1].starts_with("%") && !handle_thread.count(handle_key(fn, w1[1]))) {
                out.push_back(in);  // local memory: translate reports it
                return;
            }
            if (g.empty() || handle_thread.count("@" + g) || handle_thread.count(handle_key(fn, w1[1])))
                throw Unenc{"UNENCODED: store to " + w1[1].substr(0, 40) +
                            " (shared aggregates / thread handles need the PIR memory model)"};
            if (ty.kind != ir::Type::Int)
                throw Unenc{"UNENCODED: store of " + ty.text + " to @" + g + " (memory model)"};
            if (atomic) require_sc(w1);
            int v = shared_var(g, ty);
            VisOp vo;
            vo.kind = VisOp::Store;
            vo.var = v;
            vo.atomic = atomic;
            vo.width = ty.bits;
            vo.line = line;
            vo.what = atomic ? "atomic store" : "store";
            int k = add_op(vo);
            ir::Operand val;
            val.ty = ty;
            val.v = parse_value(w0.back(), ty);
            out.push_back(copy(in, marker(k), val));
            return;
        }
        if (op == "atomicrmw") {
            auto parts = inst_parts(in.text);
            if (parts.size() < 2) throw Unenc{"UNENCODED: atomicrmw form " + in.text};
            auto w0 = words(parts[0]);  // atomicrmw [volatile] OP ptr P
            auto w1 = words(parts[1]);  // T V [syncscope] ORDER
            w0.erase(std::remove(w0.begin(), w0.end(), "volatile"), w0.end());
            if (w0.size() < 4 || w1.size() < 2) throw Unenc{"UNENCODED: atomicrmw form " + in.text};
            require_sc(w1);
            auto rop = w0[1];
            auto g = global_ref(w0[3]);
            auto ty = ir::parse_type(w1[0]);
            if (g.empty()) throw Unenc{"UNENCODED: atomicrmw on " + w0[3].substr(0, 40) + " (memory model)"};
            if (ty.kind != ir::Type::Int) throw Unenc{"UNENCODED: atomicrmw " + rop + " " + ty.text};
            int v = shared_var(g, ty);
            VisOp vo;
            vo.kind = VisOp::Rmw;
            vo.var = v;
            vo.atomic = true;
            vo.width = ty.bits;
            vo.line = line;
            vo.what = "atomicrmw " + rop;
            int k = add_op(vo);
            ir::Operand val;
            val.ty = ty;
            val.v = parse_value(w1[1], ty);
            auto old = local(marker(k), ty);
            out.push_back(nondet(in, marker(k), ty));
            std::string nv = marker(k) + ".n";
            static const std::map<std::string, std::string> bins{
                {"add", "add"}, {"sub", "sub"}, {"and", "and"}, {"or", "or"}, {"xor", "xor"}};
            static const std::map<std::string, std::string> mm{
                {"max", "llvm.smax"}, {"min", "llvm.smin"}, {"umax", "llvm.umax"}, {"umin", "llvm.umin"}};
            if (rop == "xchg") {
                out.push_back(freeze(in, nv, val));
            } else if (auto b = bins.find(rop); b != bins.end()) {
                out.push_back(bin(in, nv, b->second, old, val));
            } else if (rop == "nand") {
                out.push_back(bin(in, nv + "a", "and", old, val));
                out.push_back(bin(in, nv, "xor", local(nv + "a", ty), cint(~uint64_t{0} >> (64 - ty.bits), ty)));
            } else if (auto x = mm.find(rop); x != mm.end()) {
                out.push_back(intrinsic(in, nv, x->second, old, val));
            } else {
                throw Unenc{"UNENCODED: atomicrmw " + rop};
            }
            out.push_back(copy(in, marker(k, true), local(nv, ty)));
            if (!in.result.empty()) out.push_back(freeze(in, in.result, old));
            return;
        }
        if (op == "cmpxchg") {
            auto parts = inst_parts(in.text);
            if (parts.size() < 3) throw Unenc{"UNENCODED: cmpxchg form " + in.text};
            auto w0 = words(parts[0]);  // cmpxchg [weak] [volatile] ptr P
            auto w1 = words(parts[1]);  // T C
            auto w2 = words(parts[2]);  // T N [syncscope] S F
            bool weak = std::find(w0.begin(), w0.end(), "weak") != w0.end();
            w0.erase(std::remove(w0.begin(), w0.end(), "weak"), w0.end());
            w0.erase(std::remove(w0.begin(), w0.end(), "volatile"), w0.end());
            if (w0.size() < 3 || w1.size() < 2 || w2.size() < 2) throw Unenc{"UNENCODED: cmpxchg form " + in.text};
            require_sc(w2);
            auto g = global_ref(w0[2]);
            auto ty = ir::parse_type(w1[0]);
            if (g.empty()) throw Unenc{"UNENCODED: cmpxchg on " + w0[2].substr(0, 40) + " (memory model)"};
            if (ty.kind != ir::Type::Int) throw Unenc{"UNENCODED: cmpxchg " + ty.text};
            int v = shared_var(g, ty);
            VisOp vo;
            vo.kind = VisOp::Rmw;
            vo.var = v;
            vo.atomic = true;
            vo.width = ty.bits;
            vo.line = line;
            vo.what = weak ? "cmpxchg weak" : "cmpxchg";
            int k = add_op(vo);
            ir::Operand cmp{ty, parse_value(w1[1], ty), {}}, nv{ty, parse_value(w2[1], ty), {}};
            auto old = local(marker(k), ty);
            out.push_back(nondet(in, marker(k), ty));
            std::string eq = marker(k) + ".eq", ok = marker(k) + ".ok", n = marker(k) + ".n";
            auto ic = bin(in, eq, "icmp", old, cmp);
            ic.pred = "eq";
            out.push_back(ic);
            if (weak) {
                // a weak compare-exchange may fail spuriously
                out.push_back(nondet(in, ok + "w", int_type(1)));
                out.push_back(bin(in, ok, "and", local(eq, int_type(1)), local(ok + "w", int_type(1))));
            } else {
                out.push_back(freeze(in, ok, local(eq, int_type(1))));
            }
            auto sel = base(in, n, "select", ty);
            sel.ops = {local(ok, int_type(1)), nv, old};
            out.push_back(sel);
            out.push_back(copy(in, marker(k, true), local(n, ty)));
            if (!in.result.empty()) cmpx[in.result] = {marker(k), ok};
            return;
        }
        if (op == "call") {
            rewrite_call(fn, f, in, out, cut, line);
            return;
        }
        out.push_back(in);
    }

    void rewrite_call(const std::string& fn, ir::Function& f, const ir::Inst& in, std::vector<ir::Inst>& out,
                      bool& cut, int line) {
        const auto& n = in.callee;
        auto i64 = int_type(64);
        auto mk = [&](VisOp::Kind kind, const std::string& what) {
            VisOp vo;
            vo.kind = kind;
            vo.line = line;
            vo.what = what;
            return vo;
        };
        auto arg = [&](std::size_t i) -> const ir::Operand& {
            if (i >= in.ops.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
            return in.ops[i];
        };
        if (n == "pthread_create" || n == "thrd_create") {
            int k = create_op_for(fn, in);
            out.push_back(copy(in, marker(k), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n == "pthread_join" || n == "thrd_join") {
            const auto& h = arg(0);
            const auto& res = arg(1);
            if (res.v.kind != ir::Value::Null) throw Unenc{"UNENCODED: " + n + " with a result pointer (memory model)"};
            if (h.ty.kind != ir::Type::Int) throw Unenc{"UNENCODED: " + n + " handle " + h.ty.text};
            int k = add_op(mk(VisOp::Join, n));
            out.push_back(copy(in, marker(k), h));
            zero_result(in, out);
            return;
        }
        if (n == "pthread_mutex_lock" || n == "mtx_lock" || n == "pthread_mutex_unlock" || n == "mtx_unlock") {
            bool lock = n.ends_with("_lock");
            auto vo = mk(lock ? VisOp::Lock : VisOp::Unlock, n);
            vo.mutex = mutex(arg(0), n);
            int k = add_op(vo);
            out.push_back(copy(in, marker(k), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n == "pthread_mutex_trylock" || n == "mtx_trylock") {
            auto vo = mk(VisOp::TryLock, n);
            vo.mutex = mutex(arg(0), n);
            auto rty = in.ty.kind == ir::Type::Int ? in.ty : int_type(32);
            vo.width = rty.bits;
            vo.busy = n == "mtx_trylock" ? 1 : 16;  // thrd_busy / EBUSY (glibc)
            int k = add_op(vo);
            out.push_back(nondet(in, marker(k), rty));
            if (!in.result.empty()) out.push_back(freeze(in, in.result, local(marker(k), rty)));
            return;
        }
        if (n == "pthread_mutex_init" || n == "mtx_init") {
            if (n == "pthread_mutex_init" && arg(1).v.kind != ir::Value::Null)
                throw Unenc{"UNENCODED: pthread_mutex_init with attributes"};
            auto vo = mk(VisOp::MutexInit, n);
            vo.mutex = mutex(arg(0), n);
            int k = add_op(vo);
            out.push_back(copy(in, marker(k), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n == "pthread_cond_wait") {
            // POSIX allows spurious wake-ups, so "unlock; <switch point>; lock"
            // is an exact over-approximation of every wait.
            auto u = mk(VisOp::Unlock, n + " (release)");
            u.mutex = mutex(arg(1), n);
            int k1 = add_op(u);
            out.push_back(copy(in, marker(k1), cint(0, i64)));
            auto l = mk(VisOp::Lock, n + " (re-acquire)");
            l.mutex = u.mutex;
            int k2 = add_op(l);
            out.push_back(copy(in, marker(k2), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n == "__VERIFIER_atomic_begin" || n == "__VERIFIER_atomic_end") {
            // SV-COMP atomic section: a pseudo-mutex whose holder is the only
            // runnable thread (lazy.cpp run_slot)
            auto vo = mk(n.ends_with("_begin") ? VisOp::Lock : VisOp::Unlock, n);
            vo.mutex = atomic_mutex();
            int k = add_op(vo);
            out.push_back(copy(in, marker(k), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n.starts_with("__VERIFIER_atomic_") && src.find(n)) {
            // SV-COMP: a function named __VERIFIER_atomic_* runs atomically
            auto b = mk(VisOp::Lock, n + " (atomic begin)");
            b.mutex = atomic_mutex();
            int k1 = add_op(b);
            out.push_back(copy(in, marker(k1), cint(0, i64)));
            out.push_back(in);
            auto e = mk(VisOp::Unlock, n + " (atomic end)");
            e.mutex = b.mutex;
            int k2 = add_op(e);
            out.push_back(copy(in, marker(k2), cint(0, i64)));
            return;
        }
        if (n == "pthread_mutex_destroy" || n == "mtx_destroy" || n == "pthread_cond_signal" ||
            n == "pthread_cond_broadcast" || n == "pthread_cond_destroy" || n == "sched_yield" ||
            n == "thrd_yield" || n == "cnd_signal" || n == "cnd_broadcast" || n == "cnd_destroy") {
            zero_result(in, out);  // no effect under interleaving semantics (every schedule is explored)
            return;
        }
        if (n == "pthread_cond_init") {
            if (arg(1).v.kind != ir::Value::Null) throw Unenc{"UNENCODED: pthread_cond_init with attributes"};
            zero_result(in, out);
            return;
        }
        if (n == "pthread_exit" || n == "thrd_exit") {
            if (!entry_of.count(fn)) throw Unenc{"UNENCODED: " + n + " outside a thread entry function"};
            auto r = base(in, "", "ret", ir::Type{});
            r.ty.kind = ir::Type::Void;
            r.ty.text = "void";
            out.push_back(r);
            cut = true;  // rest of the block is dead
            return;
        }
        (void)f;
        out.push_back(in);
    }

    // Thread entry: drop the (unused) argument and the return value.
    void prepare_entry(ir::Function& f) {
        for (auto& p : f.params) {
            if (p.name.empty()) continue;
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    bool used = false;
                    for (auto& o : in.ops)
                        if (o.v.kind == ir::Value::Local && o.v.name == p.name) used = true;
                    for (auto& [o, _] : in.incoming)
                        if (o.v.kind == ir::Value::Local && o.v.name == p.name) used = true;
                    auto pos = in.text.find("%" + p.name);
                    while (!used && pos != std::string::npos) {
                        auto e = pos + 1 + p.name.size();
                        if (e >= in.text.size() || !(std::isalnum(static_cast<unsigned char>(in.text[e])) ||
                                                     in.text[e] == '.' || in.text[e] == '_'))
                            used = true;
                        pos = in.text.find("%" + p.name, e);
                    }
                    if (used)
                        throw Unenc{"UNENCODED: thread argument used by " + f.name +
                                    " (pointers passed to threads need the PIR memory model)"};
                }
        }
        f.params.clear();
        f.ret = ir::Type{};
        f.ret.kind = ir::Type::Void;
        f.ret.text = "void";
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts)
                if (in.op == "ret") {
                    in.ops.clear();
                    in.ty = f.ret;
                }
    }
};

// Call graph over defined functions (direct calls only).
std::map<std::string, std::set<std::string>> callees(const ir::Module& m) {
    std::map<std::string, std::set<std::string>> cg;
    for (auto& f : m.functions) {
        auto& s = cg[f.name];
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts)
                if (in.op == "call" && !in.callee.empty()) s.insert(in.callee);
    }
    return cg;
}

std::set<std::string> thread_entries(const ir::Module& m) {
    std::set<std::string> out;
    for (auto& f : m.functions)
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                if (in.op != "call") continue;
                std::size_t fi = in.callee == "pthread_create" ? 2 : in.callee == "thrd_create" ? 1 : 99;
                if (fi < in.ops.size() && in.ops[fi].v.kind == ir::Value::Global) out.insert(in.ops[fi].v.name);
            }
    return out;
}

bool reaches(const std::map<std::string, std::set<std::string>>& cg, const std::string& from,
             const std::function<bool(const std::string&)>& pred) {
    std::set<std::string> seen;
    std::vector<std::string> st{from};
    while (!st.empty()) {
        auto f = st.back();
        st.pop_back();
        if (!seen.insert(f).second) continue;
        auto it = cg.find(f);
        if (it == cg.end()) continue;
        for (auto& c : it->second) {
            if (pred(c)) return true;
            st.push_back(c);
        }
    }
    return false;
}

bool is_create(const std::string& n) { return n == "pthread_create" || n == "thrd_create"; }

}  // namespace

std::vector<GlobalDef> parse_globals(std::string_view text) {
    std::vector<GlobalDef> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        auto line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.starts_with("@")) continue;
        auto eq = line.find(" = ");
        if (eq == std::string_view::npos) continue;
        GlobalDef g;
        g.name = unquote(std::string(line.substr(1, eq - 1)));
        auto rest = line.substr(eq + 3);
        // strip ", align N" / metadata / section etc.
        auto parts = split_top(rest);
        auto ws = words(parts[0]);
        std::size_t i = 0;
        while (i < ws.size() && ws[i] != "global" && ws[i] != "constant") ++i;
        if (i >= ws.size()) continue;  // alias / ifunc
        g.constant = ws[i] == "constant";
        ++i;
        if (i < ws.size()) g.type = ws[i++];
        // struct types written "{ i32, i32 }" are one word (braces); named types start with '%'
        std::string init;
        for (; i < ws.size(); ++i) init += (init.empty() ? "" : " ") + ws[i];
        g.init = init;
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<std::string> harnesses(const ir::Module& m) {
    auto cg = callees(m);
    auto entries = thread_entries(m);
    std::vector<std::string> creators;
    for (auto& f : m.functions) {
        bool direct = false;
        for (auto& c : cg[f.name])
            if (is_create(c)) direct = true;
        if (direct || reaches(cg, f.name, is_create)) creators.push_back(f.name);
    }
    if (std::find(creators.begin(), creators.end(), "main") != creators.end()) return {"main"};
    std::set<std::string> called;
    for (auto& [f, cs] : cg)
        for (auto& c : cs) called.insert(c);
    std::vector<std::string> out;
    for (auto& c : creators)
        if (!called.count(c) && !entries.count(c)) out.push_back(c);
    return out;
}

std::map<std::string, std::string> unsupported_threading(const ir::Module& m) {
    std::map<std::string, std::string> out;
    for (auto& f : m.functions)
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                if (in.op != "call") continue;
                const auto& n = in.callee;
                if (n.starts_with("_ZNSt6threadC") || n.starts_with("_ZNSt6thread15_M_start_thread") ||
                    n.starts_with("_ZNSt7jthreadC"))
                    out.emplace(f.name, "UNENCODED: std::thread (C++ threads lower through a virtual "
                                        "_State object; use pthread_create/thrd_create for the conc stage)");
                else if (n.starts_with("__kmpc_fork"))
                    out.emplace(f.name, "UNENCODED: OpenMP parallel region");
            }
    return out;
}

Build build_program(const ir::Module& m, std::string_view ir_text, const std::string& harness,
                    const Options& opt) {
    Build b;
    b.status = std::string(laws::NEEDS_HARNESS);
    const auto* hf = m.find(harness);
    if (!hf) {
        b.status = std::string(laws::ERROR);
        b.reason = "harness @" + harness + " not in module";
        return b;
    }
    try {
        Rewriter rw(m, ir_text);
        Thread main;
        main.entry = harness;
        main.name = harness;
        if (auto it = m.subprograms.find(hf->dbg); it != m.subprograms.end() && !it->second.name.empty())
            main.name = it->second.name;
        rw.prog.threads.push_back(std::move(main));
        rw.prog.harness = harness;
        auto cg = callees(m);
        auto closure = [&](const std::string& root) {
            std::set<std::string> seen;
            std::vector<std::string> st{root};
            while (!st.empty()) {
                auto f = st.back();
                st.pop_back();
                if (!m.find(f) || !seen.insert(f).second) continue;
                for (auto& c : cg[f]) st.push_back(c);
            }
            return seen;
        };
        auto scope = closure(harness);
        rw.find_creates(scope);
        // creation sites the harness can reach; nested creation is not modelled
        for (auto& [e, _] : rw.entry_of) {
            if (reaches(cg, e, is_create) || std::any_of(cg[e].begin(), cg[e].end(), is_create))
                throw Unenc{"UNENCODED: thread " + e + " creates threads (nested creation)"};
            for (auto& f : closure(e)) scope.insert(f);
        }
        int created = static_cast<int>(rw.prog.threads.size()) - 1;
        if (created > opt.max_threads)
            throw Unenc{"UNENCODED: " + std::to_string(created) + " thread creation sites (bound " +
                        std::to_string(opt.max_threads) + ")"};
        for (auto& f : rw.m.functions) {
            if (!scope.count(f.name) || !f.parse_error.empty()) continue;  // translate reports it if reached
            rw.rewrite_function(f, f.name);
        }
        for (auto& f : rw.m.functions)
            if (rw.entry_of.count(f.name)) rw.prepare_entry(f);
        // the harness: parameters must be unused (main(void) / main(argc, argv) unused)
        if (auto* h = const_cast<ir::Function*>(rw.m.find(harness)); h && !h->params.empty()) {
            ir::Function probe = *h;
            try {
                rw.prepare_entry(probe);
            } catch (const Unenc&) {
                throw Unenc{"UNENCODED: harness " + harness + " uses its parameters"};
            }
            h->params.clear();
        }
        // translate each distinct body once
        std::map<std::string, pir::Function> done;
        for (auto& th : rw.prog.threads) {
            auto it = done.find(th.entry);
            if (it == done.end()) {
                const auto* f = rw.m.find(th.entry);
                auto tr = pir::translate(rw.m, *f);
                if (!tr.fn) {
                    b.reason = tr.reason + " (in " + (th.entry == harness ? "harness " : "thread ") + th.name + ")";
                    if (!tr.status.empty()) b.status = tr.status;
                    return b;
                }
                tr.fn->name = th.name;
                it = done.emplace(th.entry, std::move(*tr.fn)).first;
            }
            th.fn = it->second;
        }
        b.prog = std::move(rw.prog);
        b.status.clear();
    } catch (const Unenc& u) {
        b.reason = u.reason;
    }
    return b;
}

}  // namespace prism::conc
