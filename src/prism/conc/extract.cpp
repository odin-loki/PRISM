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
// memory model, roadmap 2.5), syncscope atomics (weaker orders are encoded as seq_cst; stage.cpp
// withholds a clean result), thread arguments,
// libstdc++'s std::thread (the stage lowers C++ with the PRISM models under
// __PRISM_CONC_THREADS, whose std::thread / mutex / condition_variable /
// atomic inline to the pthread / __atomic calls above; a callable with
// captures or arguments then reaches pthread_create as a thread argument),
// mutexes that are not globals, a creation site run more than once or by
// several threads.

#include "prism/conc.hpp"
#include "prism/laws.hpp"

#include "../pir/translate_mem.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
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
    if (!body.empty() && body.at(0) == '%') {
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
    if (w.size() < 2 || w.at(0) != '@') return {};
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

// Weaker orders are encoded as seq_cst. Every SC run is a run the C/C++
// memory model allows, so a failure found is real; a clean result proves
// nothing about the weaker order, and the stage withholds it (Program::
// weak_order, refuse_weak_order in stage.cpp).
thread_local std::string tl_weak_order;

void require_sc(const std::vector<std::string>& ws) {
    for (auto& w : ws) {
        if (w.starts_with("syncscope(")) throw Unenc{"UNENCODED: atomic syncscope " + w};
        if (is_order(w) && w != "seq_cst" && tl_weak_order.empty()) tl_weak_order = c_order_name(w);
    }
}

ir::Value parse_value(const std::string& w, const ir::Type& ty) {
    ir::Value v;
    v.text = w;
    if (w.empty()) return v;
    if (w.at(0) == '%') {
        v.kind = ir::Value::Local;
        v.name = unquote(w.substr(1));
        return v;
    }
    if (w.at(0) == '@') {
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
    bool neg = w.at(0) == '-';
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
    pir::pirmem::Layout lay;
    std::map<std::string, int> obj_index;             // aggregate global -> Program::objects
    std::map<std::string, std::string> caddr;         // "fn/%p" -> i64 local holding its address
    std::set<std::string> arg_params;                 // "fn/%p": thread-entry parameters carried by VisOp::Arg

    Rewriter(const ir::Module& s, std::string_view ir_text) : src(s), m(s), lay(s) {
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

    // ---- bounded memory (docs/CONCURRENCY.md "Memory") ----
    static uint64_t obj_base(int k) { return (static_cast<uint64_t>(k) + 1) << 32; }

    // A shared aggregate global (array/struct of integers): -1 when g is not one.
    int mem_object(const std::string& g) {
        if (auto it = obj_index.find(g); it != obj_index.end()) return it->second;
        if (var_index.count(g) || mutex_index.count(g) || handle_thread.count("@" + g)) return -1;
        const auto* gl = src.find_global(g);
        if (!gl) return -1;
        const auto& rt = lay.resolve(gl->ty);
        if (rt.kind == ir::Type::Int || rt.kind == ir::Type::Ptr) return -1;
        if (gl->thread_local_) throw Unenc{"UNENCODED: thread_local aggregate @" + g};
        MemObject o;
        o.name = g;
        o.size = lay.alloc_size(gl->ty);
        if (o.size == 0 || o.size > (uint64_t{1} << 31)) throw Unenc{"UNENCODED: size of aggregate @" + g};
        if (!gl->external && !gl->init.empty()) {
            auto st = pir::pirmem::flat_init(lay, gl->ty, gl->init[0].v);
            if (!st) throw Unenc{"UNENCODED: initialiser of shared aggregate @" + g + " (pointers / undef)"};
            for (auto& x : *st)
                if (x.bits) o.init.push_back({x.off, x.w, x.bits});
        } else if (gl->external) {
            throw Unenc{"UNENCODED: external aggregate @" + g};
        }
        prog.objects.push_back(std::move(o));
        int k = static_cast<int>(prog.objects.size()) - 1;
        obj_index[g] = k;
        return k;
    }

    // The i64 address of pointer operand `p` in `fn`, or nullopt when it is
    // not into the bounded memory.
    std::optional<ir::Operand> mem_addr(const std::string& fn, const ir::Operand& p, int& obj) {
        auto i64 = int_type(64);
        if (p.v.kind == ir::Value::Global) {
            int k = mem_object(p.v.name);
            if (k < 0) return std::nullopt;
            obj = k;
            return cint(obj_base(k), i64);
        }
        if (p.v.kind == ir::Value::ConstExpr && (p.v.ce_op == "getelementptr" || p.v.ce_op == "bitcast") &&
            !p.v.elems.empty()) {
            // Constant GEP (e.g. `getelementptr inbounds ([4 x i32], ptr @buf, i64 0, i64 2)`):
            // every index is a literal, so fold the byte offset statically.
            auto b = mem_addr(fn, p.v.elems[0], obj);
            if (!b || b->v.kind != ir::Value::Int) return std::nullopt;
            if (p.v.ce_op == "bitcast") return b;
            auto sext = [](const ir::Operand& o) -> std::optional<uint64_t> {
                if (o.v.kind == ir::Value::Zero || o.v.kind == ir::Value::Null) return 0;
                if (o.v.kind != ir::Value::Int) return std::nullopt;
                uint64_t x = o.v.bits;
                if (o.ty.bits && o.ty.bits < 64 && ((x >> (o.ty.bits - 1)) & 1)) x |= ~wmask_bits(o.ty.bits);
                return x;
            };
            uint64_t off = 0;
            ir::Type cur = p.v.ce_ty;
            for (std::size_t k = 1; k < p.v.elems.size(); ++k) {
                auto ix = sext(p.v.elems[k]);
                if (!ix) throw Unenc{"UNENCODED: constant getelementptr with a non-literal index"};
                if (k == 1) { off += *ix * lay.alloc_size(cur); continue; }
                const auto& rt = lay.resolve(cur);
                if (rt.kind == ir::Type::Struct) {
                    auto f = static_cast<unsigned>(*ix);
                    off += lay.field_offset(rt, f);
                    cur = lay.field_type(rt, f);
                } else if (rt.kind == ir::Type::Array && !rt.elems.empty()) {
                    cur = rt.elems[0];
                    off += *ix * lay.alloc_size(cur);
                } else {
                    throw Unenc{"UNENCODED: constant getelementptr through " + rt.text + " (bounded memory)"};
                }
            }
            return cint(b->v.bits + off, i64);
        }
        if (p.v.kind == ir::Value::Local) {
            auto it = caddr.find(handle_key(fn, "%" + p.v.name));
            if (it == caddr.end()) return std::nullopt;
            obj = -1;
            return local(it->second, i64);
        }
        return std::nullopt;
    }

    // ---- symbolic pointers into the bounded memory ----
    // A pointer-typed local that only ever points into the bounded memory is
    // carried as its i64 address in "__prism.conc.p.<name>". The name is fixed
    // so that phis can refer to addresses defined later (back edges).
    static std::string paddr(const std::string& local_name) { return "__prism.conc.p." + local_name; }

    bool mem_resolvable(const std::string& fn, const ir::Operand& o) {
        if (o.v.kind == ir::Value::Global) return mem_object(o.v.name) >= 0;
        if (o.v.kind == ir::Value::ConstExpr && !o.v.elems.empty() &&
            (o.v.ce_op == "getelementptr" || o.v.ce_op == "bitcast"))
            return mem_resolvable(fn, o.v.elems[0]);
        if (o.v.kind == ir::Value::Local) return caddr.count(handle_key(fn, "%" + o.v.name)) > 0;
        return false;
    }

    // Pre-pass (fixpoint): which pointer locals of `f` point into the bounded
    // memory: GEPs off it, phis/selects whose every input does, and pointers
    // loaded from it (pointers stored in memory are stored as their address).
    void find_mem_pointers(const std::string& fn, const ir::Function& f) {
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    if (in.result.empty()) continue;
                    auto key = handle_key(fn, "%" + in.result);
                    if (caddr.count(key)) continue;
                    bool yes = false;
                    if (in.op == "getelementptr" && !in.ops.empty()) {
                        yes = mem_resolvable(fn, in.ops[0]);
                    } else if (in.op == "phi" && in.ty.kind == ir::Type::Ptr && !in.incoming.empty()) {
                        yes = true;
                        for (auto& [o, _] : in.incoming) yes = yes && mem_resolvable(fn, o);
                    } else if (in.op == "select" && in.ty.kind == ir::Type::Ptr && in.ops.size() == 3) {
                        yes = mem_resolvable(fn, in.ops[1]) && mem_resolvable(fn, in.ops[2]);
                    } else if (in.op == "load" && in.ty.kind == ir::Type::Ptr && !in.ops.empty()) {
                        yes = mem_resolvable(fn, in.ops[0]);
                    } else if (in.op == "call" && in.ty.kind == ir::Type::Ptr && is_alloc_fn(in.callee)) {
                        yes = true;
                    }
                    if (yes) {
                        caddr[key] = paddr(in.result);
                        changed = true;
                    }
                }
        }
    }

    static bool is_alloc_fn(const std::string& n) {
        return n == "malloc" || n == "calloc" || n == "_Znwm" || n == "_Znam";
    }
    static bool is_free_fn(const std::string& n) {
        return n == "free" || n == "_ZdlPv" || n == "_ZdlPvm" || n == "_ZdaPv" || n == "_ZdaPvm";
    }

    // phi / select of memory pointers -> the same over their i64 addresses.
    bool rewrite_ptr_merge(const std::string& fn, const ir::Inst& in, std::vector<ir::Inst>& out) {
        if (in.ty.kind != ir::Type::Ptr || !caddr.count(handle_key(fn, "%" + in.result))) return false;
        auto i64 = int_type(64);
        int obj = -1;
        auto addr = [&](const ir::Operand& o) {
            auto a = mem_addr(fn, o, obj);
            if (!a) throw Unenc{"UNENCODED: pointer " + in.op + " input outside the bounded memory"};
            return *a;
        };
        auto r = base(in, paddr(in.result), in.op, i64);
        if (in.op == "phi") {
            for (auto& [o, b] : in.incoming) {
                // a not-yet-rewritten local: its fixed address name
                if (o.v.kind == ir::Value::Local) r.incoming.push_back({local(paddr(o.v.name), i64), b});
                else r.incoming.push_back({addr(o), b});
            }
        } else {
            r.ops = {in.ops[0], addr(in.ops[1]), addr(in.ops[2])};
        }
        out.push_back(r);
        return true;
    }

    // getelementptr into the bounded memory: integer address arithmetic. The
    // original pointer is dropped; using it other than as a load/store/GEP
    // address leaves it undefined in PIR (reported there).
    bool rewrite_gep(const std::string& fn, const ir::Inst& in, std::vector<ir::Inst>& out) {
        if (in.ops.empty()) return false;
        int obj = -1;
        auto base_addr = mem_addr(fn, in.ops[0], obj);
        if (!base_addr) return false;
        auto i64 = int_type(64);
        ir::Operand acc = *base_addr;
        auto step = [&](const ir::Operand& idx, uint64_t scale) {
            if (idx.v.kind == ir::Value::Int) {
                auto off = idx.v.bits;
                if (idx.ty.bits < 64 && (off >> (idx.ty.bits - 1)) & 1) off |= ~wmask_bits(idx.ty.bits);
                if (off == 0) return;
                auto r = "__prism.conc.a" + std::to_string(fresh++);
                out.push_back(bin(in, r, "add", acc, cint(off * scale, i64)));
                acc = local(r, i64);
                return;
            }
            ir::Operand x = idx;
            if (idx.ty.bits < 64) {
                auto r = "__prism.conc.a" + std::to_string(fresh++);
                auto se = base(in, r, "sext", i64);
                se.ops = {idx};
                out.push_back(se);
                x = local(r, i64);
            }
            auto m1 = "__prism.conc.a" + std::to_string(fresh++);
            out.push_back(bin(in, m1, "mul", x, cint(scale, i64)));
            auto a1 = "__prism.conc.a" + std::to_string(fresh++);
            out.push_back(bin(in, a1, "add", acc, local(m1, i64)));
            acc = local(a1, i64);
        };
        ir::Type cur = in.ety;
        step(in.ops.size() > 1 ? in.ops[1] : cint(0, i64), lay.alloc_size(cur));
        for (std::size_t k = 2; k < in.ops.size(); ++k) {
            const auto& rt = lay.resolve(cur);
            if (rt.kind == ir::Type::Struct) {
                if (in.ops[k].v.kind != ir::Value::Int) throw Unenc{"UNENCODED: struct GEP with a variable index"};
                auto f = static_cast<unsigned>(in.ops[k].v.bits);
                auto off = lay.field_offset(rt, f);
                cur = lay.field_type(rt, f);
                step(cint(1, i64), off);
            } else if (rt.kind == ir::Type::Array && !rt.elems.empty()) {
                cur = rt.elems[0];
                step(in.ops[k], lay.alloc_size(cur));
            } else {
                throw Unenc{"UNENCODED: getelementptr through " + rt.text + " (bounded memory)"};
            }
        }
        auto r = paddr(in.result);
        out.push_back(bin(in, r, "add", acc, cint(0, i64)));
        caddr[handle_key(fn, "%" + in.result)] = r;
        return true;
    }
    static uint64_t wmask_bits(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

    // load/store through the bounded memory: head marker = the address,
    // tail marker = the value.
    bool rewrite_mem_access(const std::string& fn, const ir::Inst& in, std::vector<ir::Inst>& out, bool store,
                            bool atomic, int line) {
        if (in.ops.size() < (store ? 2u : 1u)) return false;
        int obj = -1;
        auto a = mem_addr(fn, in.ops[store ? 1 : 0], obj);
        if (!a) return false;
        auto i64p = int_type(64);
        bool ptrv = (store ? in.ops[0].ty : in.ty).kind == ir::Type::Ptr;
        std::optional<ir::Operand> pval;
        if (ptrv && store) {
            int o2 = -1;
            pval = mem_addr(fn, in.ops[0], o2);
            if (!pval) {
                if (in.ops[0].v.kind == ir::Value::Null || in.ops[0].v.kind == ir::Value::Zero) pval = cint(0, i64p);
                else throw Unenc{"UNENCODED: store of a pointer outside the bounded memory into it"};
            }
        }
        const auto& ty = ptrv ? i64p : (store ? in.ops[0].ty : in.ty);
        if (ty.kind != ir::Type::Int || ty.bits == 0 || ty.bits > 64)
            throw Unenc{"UNENCODED: " + std::string(store ? "store of " : "load of ") + ty.text +
                        " through the bounded memory"};
        VisOp vo;
        vo.kind = store ? VisOp::Store : VisOp::Load;
        vo.mem = true;
        vo.obj = obj;
        vo.atomic = atomic;
        vo.width = ty.bits;
        vo.line = line;
        vo.what = std::string(atomic ? "atomic " : "") + (store ? "store" : "load");
        int k = add_op(vo);
        out.push_back(copy(in, marker(k), *a));
        if (store) {
            out.push_back(copy(in, marker(k, true), pval ? *pval : in.ops[0]));
        } else {
            out.push_back(nondet(in, marker(k, true), ty));
            if (!in.result.empty())
                out.push_back(freeze(in, ptrv ? paddr(in.result) : in.result, local(marker(k, true), ty)));
        }
        return true;
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

    int cond(const ir::Operand& o, const std::string& fn) {
        if (o.v.kind != ir::Value::Global)
            throw Unenc{"UNENCODED: " + fn + " on a condition variable that is not a global"};
        for (std::size_t i = 0; i < prog.conds.size(); ++i)
            if (prog.conds[i] == o.v.name) return static_cast<int>(i);
        prog.conds.push_back(o.v.name);
        return static_cast<int>(prog.conds.size()) - 1;
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
        // thread argument: the entry's pointer parameter carries the address
        // its creating call passed (VisOp::Arg)
        std::vector<ir::Inst> pre;
        if (entry_of.count(orig_name) && !f.params.empty() && f.params[0].ty.kind == ir::Type::Ptr &&
            !f.params[0].name.empty() && !f.blocks.empty()) {
            const auto& pn = f.params[0].name;
            caddr[handle_key(orig_name, "%" + pn)] = paddr(pn);
            VisOp vo;
            vo.kind = VisOp::Arg;
            vo.what = "thread argument";
            int k = add_op(vo);
            ir::Inst anchor;
            if (!f.blocks[0].insts.empty()) anchor = f.blocks[0].insts[0];
            pre.push_back(copy(anchor, marker(k), cint(0, int_type(64))));
            pre.push_back(freeze(anchor, paddr(pn), local(marker(k), int_type(64))));
            arg_params.insert(handle_key(orig_name, "%" + pn));
        }
        find_mem_pointers(orig_name, f);
        bool first = true;
        for (auto& bl : f.blocks) {
            std::vector<ir::Inst> out;
            if (first) {
                // after the entry block's phis/allocas would be ideal; the entry block has no phis
                out = pre;
                first = false;
            }
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
        if (op == "getelementptr" && rewrite_gep(fn, in, out)) return;
        if ((op == "phi" || op == "select") && rewrite_ptr_merge(fn, in, out)) return;
        if (op == "load") {
            if (rewrite_mem_access(fn, in, out, false, in.text.find(" atomic ") != std::string::npos, line)) {
                if (in.text.find(" atomic ") != std::string::npos) require_sc(words(in.text));
                return;
            }
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
            if (rewrite_mem_access(fn, in, out, true, in.text.find(" atomic ") != std::string::npos, line)) {
                if (in.text.find(" atomic ") != std::string::npos) require_sc(words(in.text));
                return;
            }
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
            std::erase(w0, "volatile");
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
            const bool weak = std::ranges::find(w0, "weak") != w0.end();
            std::erase(w0, "weak");
            std::erase(w0, "volatile");
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
            if (!in.result.empty()) cmpx.insert_or_assign(in.result, std::pair{marker(k), ok});
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
            const auto& ta = arg(n == "pthread_create" ? 3 : 2);
            std::optional<ir::Operand> av;
            int obj = -1;
            if (ta.v.kind == ir::Value::Null || ta.v.kind == ir::Value::Zero || ta.v.kind == ir::Value::Undef ||
                ta.v.kind == ir::Value::Poison)
                av = cint(0, i64);
            else
                av = mem_addr(fn, ta, obj);
            if (!av) throw Unenc{"UNENCODED: " + n + " argument outside the bounded memory (stack or scalar global)"};
            out.push_back(copy(in, marker(k), *av));
            zero_result(in, out);
            return;
        }
        if (is_alloc_fn(n)) {
            auto lit = [&](const ir::Operand& o) -> uint64_t {
                if (o.v.kind != ir::Value::Int) throw Unenc{"UNENCODED: " + n + " with a non-constant size (bounded heap)"};
                return o.v.bits;
            };
            auto vo = mk(VisOp::Alloc, n);
            vo.size = n == "calloc" ? lit(arg(0)) * lit(arg(1)) : lit(arg(0));
            vo.zero = n == "calloc";
            if (vo.size == 0 || vo.size > (uint64_t{1} << 31)) throw Unenc{"UNENCODED: " + n + " size"};
            int k = add_op(vo);
            out.push_back(copy(in, marker(k), cint(vo.size, i64)));
            if (!in.result.empty()) out.push_back(freeze(in, paddr(in.result), local(marker(k), i64)));
            return;
        }
        if (is_free_fn(n)) {
            int obj = -1;
            const auto& pa = arg(0);
            std::optional<ir::Operand> a = pa.v.kind == ir::Value::Null ? std::optional{cint(0, i64)} : mem_addr(fn, pa, obj);
            if (!a) throw Unenc{"UNENCODED: " + n + " of a pointer outside the bounded memory"};
            int k = add_op(mk(VisOp::Free, n));
            out.push_back(copy(in, marker(k), *a));
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
        if (n == "pthread_cond_wait" || n == "cnd_wait") {  // C11 cnd_wait(cnd, mtx): same argument order
            // unlock (and join the wait set, atomically); block until a
            // signal or broadcast on the same variable after that; lock.
            // Every signal wakes every waiter (a spurious wake-up of the
            // others, which POSIX allows), so the waits that never return
            // are those with no signal at all after the release: a lost
            // wake-up, reported as CONC-DEADLOCK when every thread is stuck.
            // Spurious wake-ups that would rescue such a run are not counted.
            auto u = mk(VisOp::Unlock, n + " (release)");
            u.mutex = mutex(arg(1), n);
            u.cond = cond(arg(0), n);
            int k1 = add_op(u);
            out.push_back(copy(in, marker(k1), cint(0, i64)));
            auto w = mk(VisOp::CondWait, n + " (block)");
            w.cond = u.cond;
            int kw = add_op(w);
            out.push_back(copy(in, marker(kw), cint(0, i64)));
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
        if (n == "pthread_cond_signal" || n == "pthread_cond_broadcast" || n == "cnd_signal" || n == "cnd_broadcast") {
            auto vo = mk(VisOp::CondSignal, n);
            vo.cond = cond(arg(0), n);
            int k = add_op(vo);
            out.push_back(copy(in, marker(k), cint(0, i64)));
            zero_result(in, out);
            return;
        }
        if (n == "pthread_mutex_destroy" || n == "mtx_destroy" || n == "pthread_cond_destroy" || n == "sched_yield" ||
            n == "thrd_yield" || n == "cnd_destroy") {
            zero_result(in, out);  // no effect under interleaving semantics (every schedule is explored)
            return;
        }
        if (n == "cnd_init") {  // thrd_success (0); the variable is identified by its global
            (void)cond(arg(0), n);
            zero_result(in, out);
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
            if (p.name.empty() || arg_params.count(handle_key(f.name, "%" + p.name))) continue;
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
    tl_weak_order.clear();
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
        // creation sites the harness can reach, then (nested creation) those
        // the created threads reach, to a fixpoint. Each function is scanned
        // once; lazy.cpp checks that every creation site runs in one thread
        // at most once, so the thread set stays static.
        std::set<std::string> scope, scanned;
        std::set<std::string> todo = closure(harness);
        while (!todo.empty()) {
            std::set<std::string> fresh;
            for (auto& f : todo)
                if (scanned.insert(f).second) fresh.insert(f);
            scope.insert(fresh.begin(), fresh.end());
            rw.find_creates(fresh);
            todo.clear();
            for (auto& [e, _] : rw.entry_of)
                for (auto& f : closure(e))
                    if (!scanned.count(f)) todo.insert(f);
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
        rw.prog.weak_order = tl_weak_order;
        b.prog = std::move(rw.prog);
        b.status.clear();
    } catch (const Unenc& u) {
        b.reason = u.reason;
    }
    return b;
}

}  // namespace prism::conc
