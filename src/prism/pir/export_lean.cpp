// Export (LLVM fragment, PIR) pairs for the Lean correspondence checker
// (roadmap 8.2 "LLVM IR to PIR translation", 2.4 translation validation).
//
// When PRISM_PIR_LEAN_EXPORT is set, every function the pir stage translates
// is appended to <dir>/<unit>.pirl: the LLVM function in the fragment syntax
// of proofs/refinement/PrismRefine/Llvm.lean, and what translate() produced
// (the PIR, or the UNENCODED reason). `pir_lean_check` (proofs/refinement,
// driven by tools/pir_lean_check.py) re-translates the LLVM side with the
// Lean translator -- the one the refinement theorems are proved about -- and
// checks the C++ output is exactly that. Anything outside the fragment is
// written as `L unsupported <why>` and reported, never compared silently.
//
// PRISM_PIR_LEAN_EXPORT=1 writes to <out>/pir-lean/; any other value is the
// directory. Unset (the default): nothing is written.

#include "prism/pir.hpp"

#include "translate_mem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace prism::pir {
namespace {

struct Unsupported {
    std::string why;
};

bool plain_name(const std::string& s) {
    if (s.empty()) return false;
    return std::none_of(s.begin(), s.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"';
    });
}

std::string name(const std::string& s) {
    if (!plain_name(s)) throw Unsupported{"name with blanks or quotes"};
    return s;
}

unsigned width(const ir::Type& t) {
    if (t.kind != ir::Type::Int) throw Unsupported{"type " + t.text};
    return t.bits;
}

std::string opnd(const ir::Operand& o) {
    switch (o.v.kind) {
        case ir::Value::Local:
            if (o.v.name.starts_with("@")) throw Unsupported{"register named like a global"};
            return "%" + name(o.v.name);
        case ir::Value::Int: width(o.ty); return "#" + std::to_string(o.v.bits);
        case ir::Value::Poison: width(o.ty); return "poison";
        case ir::Value::Undef: width(o.ty); return "undef";  // only accepted under freeze
        default: throw Unsupported{"operand " + o.ty.text + " " + o.v.text};
    }
}

bool has(const ir::Inst& in, std::string_view f) {
    return std::find(in.flags.begin(), in.flags.end(), f) != in.flags.end();
}

std::string arg(const Arg& a) {
    if (a.is_const) return "c" + std::to_string(a.width) + ":" + std::to_string(a.bits);
    return "v" + std::to_string(a.var) + ":" + std::to_string(a.width);
}

std::string word(const std::string& s) { return s.empty() ? "-" : s; }

// The analysed function's read-only globals (pirmem::entry_globals); empty
// while a callee is written (a global there is outside the fragment).
thread_local const std::vector<std::string>* g_entry = nullptr;

// A pointer operand: a register (an alloca or getelementptr result), or an
// entry global, written as the register `%@name` its `L glob` line defines.
std::string ptr_opnd(const ir::Operand& o) {
    if (o.ty.kind == ir::Type::Ptr && o.v.kind == ir::Value::Global && g_entry &&
        std::find(g_entry->begin(), g_entry->end(), o.v.name) != g_entry->end())
        return "%@" + name(o.v.name);
    if (o.ty.kind != ir::Type::Ptr || o.ty.text != "ptr" || o.v.kind != ir::Value::Local)
        throw Unsupported{"pointer operand " + o.v.text};
    if (o.v.name.starts_with("@")) throw Unsupported{"register named like a global"};
    return "%" + name(o.v.name);
}

uint64_t count_of(const ir::Type& t) {  // "[N x T]" (translate_mem.cpp)
    std::size_t i = 1;
    while (i < t.text.size() && t.text[i] == ' ') ++i;
    uint64_t n = 0;
    while (i < t.text.size() && std::isdigit(static_cast<unsigned char>(t.text[i])))
        n = n * 10 + static_cast<uint64_t>(t.text[i++] - '0');
    return n;
}

const ir::Inst* def_of(const ir::Function& f, const std::string& n) {
    for (auto& bl : f.blocks)
        for (auto& in : bl.insts)
            if (in.result == n) return &in;
    return nullptr;
}

// Tr::mem_inst's test for an integer load of a whole aggregate (a raw byte
// copy whose uninitialised bytes follow the value): outside the fragment.
bool aggregate_load(const pirmem::Layout& lay, const ir::Function& f, const ir::Inst& in) {
    if (in.ty.kind != ir::Type::Int || in.ops[0].v.kind != ir::Value::Local) return false;
    const auto* d = def_of(f, in.ops[0].v.name);
    if (!d) return false;
    try {
        std::optional<ir::Type> pt;
        if (d->op == "alloca") {
            pt = d->ety;
        } else if (d->op == "getelementptr") {
            const ir::Type* t = &d->ety;
            bool ok = true;
            for (std::size_t k = 2; ok && k < d->ops.size(); ++k) {
                const auto& rt = lay.resolve(*t);
                if (rt.kind == ir::Type::Struct && d->ops[k].v.kind == ir::Value::Int)
                    t = &lay.field_type(rt, static_cast<unsigned>(d->ops[k].v.bits));
                else if (rt.kind == ir::Type::Array)
                    t = &rt.elems.at(0);
                else
                    ok = false;
            }
            if (ok) pt = *t;
        }
        if (!pt) return false;
        const auto& rt = lay.resolve(*pt);
        return rt.kind == ir::Type::Struct || rt.kind == ir::Type::Array;
    } catch (const pirmem::Unenc&) {
        return false;
    }
}

// `L gep %d INB BASE N` then per index `f OPND W SCALE` (the first), `s OFF`
// (a struct field), `a OPND W SCALE BOUND USE` (an array index; BOUND 0: no C
// array-bound check), computed as MemTr::gep and Tr::mem_inst do.
void gep_line(std::ostream& b, const pirmem::Layout& lay, const ir::Function& f, const ir::Inst& in) {
    if (in.ty.kind != ir::Type::Ptr || in.ops.empty() || in.ops[0].ty.text != "ptr" || in.result.empty())
        throw Unsupported{"vector getelementptr"};
    for (auto& fl : in.flags)
        if (fl != "inbounds") throw Unsupported{"getelementptr " + fl};
    int use = 0;  // 0 address, 1 loaded, 2 stored through, 3 deeper GEP
    for (auto& bl : f.blocks)
        for (auto& u : bl.insts) {
            auto is_res = [&](std::size_t i) {
                return i < u.ops.size() && u.ops[i].v.kind == ir::Value::Local && u.ops[i].v.name == in.result;
            };
            if (u.op == "load" && is_res(0)) use = std::max(use, 1);
            if (u.op == "store" && is_res(1)) use = std::max(use, 2);
            if (u.op == "getelementptr" && is_res(0)) use = std::max(use, 3);
        }
    bool base_last = false;
    if (in.ops[0].v.kind == ir::Value::Local)
        if (const auto* d = def_of(f, in.ops[0].v.name); d && d->op == "getelementptr") try {
                const ir::Type* t = &d->ety;
                for (std::size_t k = 2; k < d->ops.size(); ++k) {
                    const auto& rt = lay.resolve(*t);
                    if (rt.kind == ir::Type::Struct && d->ops[k].v.kind == ir::Value::Int) {
                        auto fi = static_cast<unsigned>(d->ops[k].v.bits);
                        base_last = fi + 1 == rt.elems.size();
                        t = &lay.field_type(rt, fi);
                    } else if (rt.kind == ir::Type::Array) {
                        base_last = false;
                        t = &rt.elems.at(0);
                    } else {
                        break;
                    }
                }
            } catch (const pirmem::Unenc&) {
                base_last = true;
            }
    b << "L gep %" << name(in.result) << " " << (std::find(in.flags.begin(), in.flags.end(), "inbounds") != in.flags.end())
      << " " << ptr_opnd(in.ops[0]) << " " << in.ops.size() - 1;
    const ir::Type* t = &in.ety;
    bool last_field = base_last;
    for (std::size_t k = 1; k < in.ops.size(); ++k) {
        const auto& o = in.ops[k];
        unsigned w = width(o.ty);
        if (k == 1) {
            b << " f " << opnd(o) << " " << w << " " << lay.alloc_size(in.ety);
            continue;
        }
        const auto& rt = lay.resolve(*t);
        if (rt.kind == ir::Type::Struct) {
            if (o.v.kind != ir::Value::Int) throw Unsupported{"getelementptr with a variable struct index"};
            auto fi = static_cast<unsigned>(o.v.bits);
            b << " s " << lay.field_offset(rt, fi);
            t = &lay.field_type(rt, fi);
            last_field = fi + 1 == rt.elems.size();
            continue;
        }
        if (rt.kind != ir::Type::Array) throw Unsupported{"getelementptr into " + rt.text};
        uint64_t n = count_of(rt);
        uint64_t bound = !last_field && n > 0 ? n : 0;
        last_field = false;
        t = &rt.elems.at(0);
        b << " a " << opnd(o) << " " << w << " " << lay.alloc_size(*t) << " " << bound << " " << use;
    }
    b << "\n";
}

// A call translate.cpp inlines (Tr::call -> Tr::inline_call): a function
// defined in the module that no earlier handler of Tr::call claims. Library
// code (a model, or a function from another file: its checks are reported at
// the call site) is left out.
bool inlined_call(const ir::Module& m, const ir::Inst& in, const std::string& top_file) {
    const auto& n = in.callee;
    if (in.is_asm || n.empty() || n.starts_with("llvm.") || n.starts_with("__prism") || n.starts_with("__cxa_") ||
        n == "__clang_call_terminate" || n == "_ZSt9terminatev")
        return false;
    const auto* callee = m.find(n);
    if (!callee) return false;
    std::string cfile;
    if (auto it = m.subprograms.find(callee->dbg); it != m.subprograms.end()) cfile = it->second.file;
    return !callee->is_model && (cfile.empty() || top_file.empty() || cfile == top_file);
}

bool overflow_call(const ir::Inst& in) {
    if (in.op != "call") return false;
    for (auto p : {"llvm.sadd.with.overflow.", "llvm.uadd.with.overflow.", "llvm.ssub.with.overflow.",
                   "llvm.usub.with.overflow.", "llvm.smul.with.overflow.", "llvm.umul.with.overflow."})
        if (in.callee.starts_with(p)) return true;
    return false;
}

// The intrinsics Tr::call translates specially and the Lean fragment models
// (XLlvm.lean `SInst.mm` ... `memset`); any other intrinsic is outside.
void intrinsic_line(std::ostream& b, const ir::Inst& in) {
    const std::string& n = in.callee;
    auto res = [&] {
        if (in.result.empty()) throw Unsupported{"call @" + n + " without a result"};
        return "%" + name(in.result);
    };
    auto nops = [&](std::size_t k) {
        if (in.ops.size() != k) throw Unsupported{"call @" + n + " arity"};
    };
    for (auto k : {"smax", "smin", "umax", "umin"})
        if (n.starts_with(std::string("llvm.") + k + ".")) {
            nops(2);
            b << "L mm " << res() << " " << k << " " << width(in.ty) << " " << opnd(in.ops[0]) << " "
              << opnd(in.ops[1]) << "\n";
            return;
        }
    for (auto k : {"abs", "ctlz", "cttz", "ctpop", "bswap"})
        if (n.starts_with(std::string("llvm.") + k + ".")) {
            bool flagged = std::string_view(k) == "abs" || std::string_view(k) == "ctlz" || std::string_view(k) == "cttz";
            nops(flagged ? 2 : 1);
            bool flag = flagged && in.ops[1].v.kind == ir::Value::Int && in.ops[1].v.bits != 0;
            if (flagged && in.ops[1].v.kind != ir::Value::Int) throw Unsupported{"call @" + n + " flag"};
            b << "L un " << res() << " " << k << " " << width(in.ty) << " " << opnd(in.ops[0]) << " " << flag
              << "\n";
            return;
        }
    if (n.starts_with("llvm.expect.")) {
        if (in.ops.empty()) throw Unsupported{"call @" + n + " arity"};
        b << "L expect " << res() << " " << width(in.ty) << " " << opnd(in.ops[0]) << "\n";
        return;
    }
    if (overflow_call(in)) {
        nops(2);
        if (in.ty.kind != ir::Type::Struct || in.ty.elems.size() != 2) throw Unsupported{"call @" + n + " type"};
        b << "L ovf " << res() << " " << n.substr(5, 4) << " " << width(in.ty.elems[0]) << " " << opnd(in.ops[0])
          << " " << opnd(in.ops[1]) << "\n";
        return;
    }
    if (n.starts_with("llvm.lifetime.start") || n.starts_with("llvm.lifetime.end")) {
        nops(2);
        if (n.starts_with("llvm.lifetime.end")) {
            b << "L lend " << ptr_opnd(in.ops[1]) << "\n";
            return;
        }
        if (in.ops[0].v.kind != ir::Value::Int) throw Unsupported{"call @" + n + " size"};
        b << "L lstart " << in.ops[0].v.bits << " " << ptr_opnd(in.ops[1]) << "\n";
        return;
    }
    if (n.starts_with("llvm.memcpy.") || n.starts_with("llvm.memmove.")) {
        if (in.ops.size() < 3) throw Unsupported{"call @" + n + " arity"};
        b << "L memcpy " << ptr_opnd(in.ops[0]) << " " << ptr_opnd(in.ops[1]) << " " << opnd(in.ops[2]) << " "
          << width(in.ops[2].ty) << " " << n.starts_with("llvm.memmove.") << "\n";
        return;
    }
    if (n.starts_with("llvm.memset.")) {
        if (in.ops.size() < 3 || width(in.ops[1].ty) != 8) throw Unsupported{"call @" + n + " arity"};
        b << "L memset " << ptr_opnd(in.ops[0]) << " " << opnd(in.ops[1]) << " " << opnd(in.ops[2]) << " "
          << width(in.ops[2].ty) << "\n";
        return;
    }
    throw Unsupported{"call @" + n};
}

void llvm_side(std::ostream& o, const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
               const std::string& top_file, std::vector<std::string>& callees) {
    if (!f.parse_error.empty()) throw Unsupported{"unparsed IR"};
    std::ostringstream b;
    b << "L params " << f.params.size();
    for (auto& p : f.params) b << " " << name(p.name) << " " << width(p.ty);
    b << "\nL ret " << (f.ret.kind == ir::Type::Void ? 0u : width(f.ret)) << "\n";
    static const std::set<std::string> bins{"add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                            "shl", "lshr", "ashr", "and", "or", "xor"};
    const pirmem::Layout lay(m);
    const bool top = callees.empty();
    std::vector<std::string> eg;
    if (top) eg = pirmem::entry_globals(m, lay, f, opt.globals_initial);
    g_entry = top ? &eg : nullptr;
    struct Reset {
        ~Reset() { g_entry = nullptr; }
    } reset;
    for (auto& bl : f.blocks) {
        b << "L block " << name(bl.name) << "\n";
        if (top && &bl == &f.blocks.front())
            for (auto& gname : eg) {
                // MemTr::emit_entry_globals: a read-only object, zero-filled, then the stores
                const auto* g = m.find_global(gname);
                auto flat = pirmem::flat_init(lay, g->ty, g->init[0].v);
                b << "L glob %@" << name(gname) << " " << lay.alloc_size(g->ty) << " "
                  << std::max(g->align, lay.align(g->ty)) << " "
                  << static_cast<int>(g->is_const ? MemKind::Const : MemKind::Static) << " " << flat->size();
                for (auto& st : *flat) b << " " << st.off << " " << st.w << " " << st.bits;
                b << "\n";
            }
        for (auto& in : bl.insts) {
            if (!in.parsed) throw Unsupported{in.op};
            const std::string& op = in.op;
            if (op == "getelementptr") {  // its flags are checked by gep_line
                try {
                    gep_line(b, lay, f, in);
                } catch (const pirmem::Unenc& u) {
                    throw Unsupported{"getelementptr: " + u.reason};
                }
                continue;
            }
            for (auto& fl : in.flags)
                if (fl != "nsw" && fl != "nuw" && fl != "exact" && fl != "disjoint" && fl != "nneg")
                    throw Unsupported{in.op + " " + fl};
            if (op == "phi") {
                b << "L phi %" << name(in.result) << " " << width(in.ty) << " " << in.incoming.size();
                for (auto& [v, pred] : in.incoming) b << " " << opnd(v) << " " << name(pred);
                b << "\n";
            } else if (bins.count(op)) {
                std::string fl;
                for (auto f2 : {"nsw", "nuw", "exact", "disjoint"})
                    if (has(in, f2)) fl += (fl.empty() ? "" : ",") + std::string(f2);
                if (op == "shl") {
                    ir::DILoc l;
                    if (!in.dbg.empty())
                        if (auto it = m.locs.find(in.dbg); it != m.locs.end()) l = it->second;
                    if (std::find(opt.signed_shl.begin(), opt.signed_shl.end(), std::make_pair(l.line, l.col)) !=
                        opt.signed_shl.end())
                        fl += (fl.empty() ? "" : ",") + std::string("csigned");
                }
                if (in.result.empty() || in.ops.size() != 2) throw Unsupported{op + " shape"};
                b << "L bin %" << name(in.result) << " " << op << " " << word(fl) << " " << width(in.ty) << " "
                  << opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
            } else if (op == "icmp") {
                if (in.result.empty() || in.ops.size() != 2) throw Unsupported{"icmp shape"};
                b << "L icmp %" << name(in.result) << " " << in.pred << " " << width(in.ty) << " "
                  << opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
            } else if (op == "select") {
                if (in.result.empty() || in.ops.size() != 3 || width(in.ops[0].ty) != 1)
                    throw Unsupported{"select shape"};
                b << "L select %" << name(in.result) << " " << width(in.ty) << " " << opnd(in.ops[0]) << " "
                  << opnd(in.ops[1]) << " " << opnd(in.ops[2]) << "\n";
            } else if (op == "zext" || op == "sext" || op == "trunc") {
                if (in.result.empty() || in.ops.size() != 1) throw Unsupported{op + " shape"};
                if (op == "trunc" && (has(in, "nuw") || has(in, "nsw"))) throw Unsupported{"trunc nuw/nsw"};
                b << "L cast %" << name(in.result) << " " << op << " " << (has(in, "nneg") ? 1 : 0) << " "
                  << width(in.ops[0].ty) << " " << width(in.ty) << " " << opnd(in.ops[0]) << "\n";
            } else if (op == "br") {
                if (in.targets.size() == 1) {
                    b << "L br " << name(in.targets[0]) << "\n";
                } else if (in.targets.size() == 2 && in.ops.size() == 1 && width(in.ops[0].ty) == 1) {
                    b << "L cbr " << opnd(in.ops[0]) << " " << name(in.targets[0]) << " " << name(in.targets[1])
                      << "\n";
                } else {
                    throw Unsupported{"br shape"};
                }
            } else if (op == "ret") {
                b << "L ret " << (in.ops.empty() ? std::string("-") : opnd(in.ops[0])) << "\n";
            } else if (op == "unreachable") {
                b << "L unreachable\n";
            } else if (op == "call" && in.callee.starts_with("__prism.uninit.") && !in.result.empty()) {
                // stage.cpp's marker for a scalar local: an indeterminate value
                b << "L uninit %" << name(in.result) << " " << width(in.ty) << "\n";
            } else if (op == "alloca") {
                if (in.result.empty() || !in.ops.empty()) throw Unsupported{"alloca with a count"};
                try {
                    b << "L alloca %" << name(in.result) << " " << lay.alloc_size(in.ety) << " "
                      << (in.align ? in.align : lay.align(in.ety)) << "\n";
                } catch (const pirmem::Unenc& u) {
                    throw Unsupported{"alloca: " + u.reason};
                }
            } else if (op == "load") {
                if (in.result.empty() || in.ops.size() != 1) throw Unsupported{"load shape"};
                std::string p = ptr_opnd(in.ops[0]);
                if (aggregate_load(lay, f, in)) throw Unsupported{"integer load of an aggregate (raw byte copy)"};
                b << "L load %" << name(in.result) << " " << width(in.ty) << " " << p << " " << in.align << "\n";
            } else if (op == "store") {
                if (in.ops.size() != 2) throw Unsupported{"store shape"};
                b << "L store " << width(in.ops[0].ty) << " " << opnd(in.ops[0]) << " " << ptr_opnd(in.ops[1]) << " "
                  << in.align << "\n";
            } else if (op == "freeze") {
                if (in.result.empty() || in.ops.size() != 1) throw Unsupported{"freeze shape"};
                b << "L freeze %" << name(in.result) << " " << width(in.ty) << " " << opnd(in.ops[0]) << "\n";
            } else if (op == "call" && in.callee.starts_with("llvm.") && !in.is_asm) {
                intrinsic_line(b, in);
            } else if (op == "extractvalue") {
                // a field of an overflow intrinsic's { iN, i1 } result
                const auto* d = in.ops.size() == 1 && in.ops[0].v.kind == ir::Value::Local
                                    ? def_of(f, in.ops[0].v.name) : nullptr;
                if (!d || !overflow_call(*d) || in.indices.size() != 1 || in.result.empty())
                    throw Unsupported{"extractvalue"};
                b << "L xv %" << name(in.result) << " " << width(in.ty) << " %" << name(in.ops[0].v.name) << " "
                  << in.indices[0] << "\n";
            } else if (op == "call" && inlined_call(m, in, top_file)) {
                b << "L call " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
                  << (in.ty.kind == ir::Type::Void ? 0u : width(in.ty)) << " " << name(in.callee) << " "
                  << in.ops.size();
                for (auto& a : in.ops) b << " " << opnd(a) << " " << width(a.ty);
                b << "\n";
                if (std::find(callees.begin(), callees.end(), in.callee) == callees.end()) callees.push_back(in.callee);
            } else if (op == "call") {
                throw Unsupported{"call @" + (in.callee.empty() ? std::string("<indirect>") : in.callee)};
            } else {
                throw Unsupported{op};
            }
        }
    }
    o << b.str();
}

void pir_side(std::ostream& o, const Function& fn) {
    o << "P vars " << fn.vars.size();
    for (auto& v : fn.vars) o << " " << v.width;
    o << "\nP params " << fn.params.size();
    for (int p : fn.params) o << " " << p;
    o << "\nP retw " << fn.ret_width << "\n";
    for (auto& bl : fn.blocks) {
        o << "P block\n";
        for (auto& p : bl.phis) {
            o << "P phi " << p.dst << " " << p.in.size();
            for (auto& [pred, a] : p.in) o << " " << pred << " " << arg(a);
            o << "\n";
        }
        for (auto& s : bl.stmts) {
            switch (s.kind) {
                case Stmt::Assign:
                    if (s.op == Op::Havoc) {
                        o << "P havoc " << s.dst << "\n";
                        break;
                    }
                    o << "P assign " << s.dst << " " << op_name(s.op) << " " << s.args.size();
                    for (auto& a : s.args) o << " " << arg(a);
                    o << "\n";
                    break;
                case Stmt::Check:
                    o << "P check " << arg(s.args[0]) << " " << word(s.prop) << " " << word(s.cls) << "\n";
                    break;
                case Stmt::Assume: o << "P assume " << arg(s.args[0]) << "\n"; break;
                case Stmt::Alloc:
                    o << "P alloc " << s.dst << " " << arg(s.args[0]) << " " << static_cast<int>(s.mkind) << " "
                      << s.init << " " << s.align << "\n";
                    break;
                case Stmt::Free: o << "P free " << arg(s.args[0]) << "\n"; break;
                case Stmt::Load:
                    o << "P load " << s.dst << " " << s.dst2 << " " << s.dst3 << " " << arg(s.args[0]) << " " << s.tag
                      << "\n";
                    break;
                case Stmt::Store:
                    o << "P store " << arg(s.args[0]) << " " << arg(s.args[1]) << " " << arg(s.args[2]) << " "
                      << s.tag << "\n";
                    break;
                case Stmt::MemCpy:
                case Stmt::MemSet:
                    o << "P " << (s.kind == Stmt::MemCpy ? "memcpy " : "memset ") << arg(s.args[0]) << " "
                      << arg(s.args[1]) << " " << arg(s.args[2]) << "\n";
                    break;
                default:
                    // stack save and restore: not in the Lean PIR
                    // syntax, so the checker can never equate them with anything
                    o << "P memory-statement " << static_cast<int>(s.kind) << "\n";
                    break;
            }
        }
        auto& t = bl.term;
        switch (t.kind) {
            case Term::Jmp: o << "P jmp " << t.t << "\n"; break;
            case Term::Br: o << "P br " << arg(t.cond) << " " << t.t << " " << t.f << "\n"; break;
            case Term::Ret: o << "P ret " << (t.val ? arg(*t.val) : std::string("-")) << "\n"; break;
            case Term::Stop: o << "P stop\n"; break;
        }
    }
}

}  // namespace

void export_lean_pair(const std::filesystem::path& out_root, const std::string& unit, const ir::Module& m,
                      const ir::Function& f, const TranslateOptions& opt, const Translation& t) {
    const char* env = std::getenv("PRISM_PIR_LEAN_EXPORT");
    if (!env || !*env) return;
    std::filesystem::path dir = std::string(env) == "1" ? out_root / "pir-lean" : std::filesystem::path(env);
    std::ostringstream o;
    o << "func " << (plain_name(f.name) ? f.name : std::string("?")) << "\n";
    try {
        std::ostringstream l;
        std::string top_file;
        if (auto it = m.subprograms.find(f.dbg); it != m.subprograms.end()) top_file = it->second.file;
        std::vector<std::string> callees;
        llvm_side(l, m, f, opt, top_file, callees);
        // every function the calls reach, once, in order of first call
        for (std::size_t i = 0; i < callees.size(); ++i) {
            l << "L fn " << name(callees[i]) << "\n";
            llvm_side(l, m, *m.find(callees[i]), opt, top_file, callees);
        }
        if (!callees.empty()) o << "L depth " << opt.inline_depth << "\n";
        o << l.str();
    } catch (const Unsupported& u) {
        std::string why = u.why;
        std::replace(why.begin(), why.end(), '\n', ' ');
        o << "L unsupported " << why << "\n";
    }
    if (t.fn) {
        o << "status ok\n";
        pir_side(o, *t.fn);
    } else {
        std::string why = t.reason;
        std::replace(why.begin(), why.end(), '\n', ' ');
        o << "status unencoded " << why << "\n";
    }
    o << "end\n";
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / (unit + ".pirl"), std::ios::app) << o.str();
}

}  // namespace prism::pir
