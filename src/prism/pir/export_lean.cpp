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

// The width of a value: an integer's, or 64 for a pointer (translate.cpp
// keeps a pointer as one 64-bit value, object id in bits 63..48).
unsigned vw(const ir::Type& t) {
    if (t.kind == ir::Type::Ptr && t.text == "ptr") return 64;
    return width(t);
}

// The analysed function's entry globals (pirmem::entry_globals); null while
// a callee is written (a global there is outside the fragment).
thread_local const std::vector<std::string>* g_entry = nullptr;

std::string opnd(const ir::Operand& o) {
    switch (o.v.kind) {
        case ir::Value::Local:
            if (o.v.name.starts_with("@")) throw Unsupported{"register named like a global"};
            return "%" + name(o.v.name);
        case ir::Value::Int: width(o.ty); return "#" + std::to_string(o.v.bits);
        case ir::Value::Poison: vw(o.ty); return "poison";
        case ir::Value::Undef: vw(o.ty); return "undef";  // only accepted under freeze
        case ir::Value::Null:
        case ir::Value::Zero:
            // Tr::operand: a null pointer is the constant 0
            if (o.ty.kind == ir::Type::Ptr && o.ty.text == "ptr") return "#0";
            throw Unsupported{"operand " + o.ty.text + " " + o.v.text};
        case ir::Value::Global:
            // Tr::operand -> MemTr::global: an entry global is the register its `L glob` line defines
            if (o.ty.kind == ir::Type::Ptr && g_entry &&
                std::find(g_entry->begin(), g_entry->end(), o.v.name) != g_entry->end())
                return "%@" + name(o.v.name);
            throw Unsupported{"pointer operand " + o.v.text};
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
// defined in the module that no earlier handler of Tr::call claims (library
// models included: malloc, free, operator new, ... are C code linked into the
// module).
bool inlined_call(const ir::Module& m, const ir::Inst& in) {
    const auto& n = in.callee;
    if (in.is_asm || n.empty() || n.starts_with("llvm.") || n.starts_with("__prism") || n.starts_with("__cxa_") ||
        n == "__clang_call_terminate" || n == "_ZSt9terminatev")
        return false;
    return m.find(n) != nullptr;
}

// Tr::inline_call's `foreign`: library code (a model, or a function from
// another file). Inside it every instruction's location is the call site's
// line with column 0 (Frame::site_line), so no `shl` there is a C signed
// shift (their locations have a column).
bool foreign_callee(const ir::Module& m, const std::string& n, const std::string& top_file) {
    const auto* callee = m.find(n);
    if (!callee) return false;
    std::string cfile;
    if (auto it = m.subprograms.find(callee->dbg); it != m.subprograms.end()) cfile = it->second.file;
    return callee->is_model || (!cfile.empty() && !top_file.empty() && cfile != top_file);
}

// A function the export writes: its name and whether it is inlined as library code.
struct Callee {
    std::string name;
    bool foreign = false;
};

// The register of the hidden allocation-failed flag object (a name no LLVM
// local the exporter writes has: it contains a dot and the reserved prefix).
constexpr const char* kOom = "__prism.oom";

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

// The model intrinsics of Tr::model_intrinsic the extended fragment covers
// (the libc models' malloc/free/new/delete use only these).
void model_line(std::ostream& b, const ir::Module& m, const ir::Inst& in) {
    (void)m;
    const std::string& n = in.callee;
    auto cint = [&](std::size_t i) -> uint64_t {
        if (i >= in.ops.size() || in.ops[i].v.kind != ir::Value::Int)
            throw Unsupported{"call @" + n + " with a non-constant argument"};
        return in.ops[i].v.bits;
    };
    if (n == "__prism_alloc") {
        // MemTr::alloc(size, kind, init): the object; the size below 2^47 is assumed
        if (in.ops.size() != 3 || vw(in.ops[0].ty) != 64) throw Unsupported{"call @" + n + " arity"};
        const uint64_t k = cint(1), init = cint(2);
        if (init > 2 || k > 255) throw Unsupported{"call @" + n + " kind"};
        b << "L halloc " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
          << opnd(in.ops[0]) << " " << k << " " << init << "\n";
        return;
    }
    if (n == "__prism_free" || n == "__prism_free_check") {
        // MemTr::dealloc: the free checks, then (free) the lifetime ends
        if (in.ops.size() != 2) throw Unsupported{"call @" + n + " arity"};
        const uint64_t k = cint(1);
        if (k == static_cast<uint64_t>(MemKind::File) || k > 255) throw Unsupported{"call @" + n + " kind"};
        b << "L hfree " << opnd(in.ops[0]) << " " << k << " " << (n == "__prism_free" ? 1 : 0) << "\n";
        return;
    }
    if (n == "__prism_assume") {
        if (in.ops.size() != 1) throw Unsupported{"call @" + n + " arity"};
        b << "L vassume " << opnd(in.ops[0]) << " " << vw(in.ops[0].ty) << "\n";
        return;
    }
    if (n == "__prism_alloc_failed") {
        // Tr::st(oom, i8 1): a store of one byte, alignment 8, into the flag object
        b << "L store 8 #1 %" << kOom << " 8\n";
        return;
    }
    throw Unsupported{"call @" + n};
}

void llvm_side(std::ostream& o, const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
               const std::string& top_file, std::vector<Callee>& callees, bool top, bool foreign) {
    if (!f.parse_error.empty()) throw Unsupported{"unparsed IR"};
    std::ostringstream b;
    static const std::set<std::string> bins{"add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                            "shl", "lshr", "ashr", "and", "or", "xor"};
    const pirmem::Layout lay(m);
    // the hidden allocation-failed flag (translate.cpp: preassigned when the
    // analysed function can reach __prism_alloc_failed): an object after the
    // entry globals, passed to every inlined function that can reach it as an
    // extra last parameter (the translator refers to its variable directly)
    const bool oom = pirmem::reaches_alloc_failed(m, f);
    // Pointer parameters. Of an inlined callee: ordinary 64-bit values bound
    // to the caller's arguments (a by-value aggregate is copied first:
    // outside). Of the analysed function (translate()): a by-value aggregate
    // or return slot is a fresh stack object, a parameter with a
    // constant-size contract a fresh object of arbitrary bytes (Law 6
    // harness), written as an `L glob` line at the start of the entry block
    // (in parameter order, before the entry globals, as the prologue has
    // them) and not as a parameter; one without a contract is `L ptrparam`
    // (the Lean translator refuses it, as translate() does: NEEDS-HARNESS).
    std::ostringstream harness;
    std::vector<const ir::Param*> ints;
    for (auto& p : f.params) {
        if (p.ty.kind != ir::Type::Ptr) {
            ints.push_back(&p);
            continue;
        }
        if (p.ty.text != "ptr") throw Unsupported{"type " + p.ty.text};
        // translate(): refused unless an object the language provides (byval,
        // sret) or a contract binds it; then Tr::byval_type (byval or byref)
        // or the sret type is allocated, else the contract object
        const bool own = p.attrs.find("byval(") != std::string::npos || p.attrs.find("sret(") != std::string::npos;
        const bool bt = p.attrs.find("byval(") != std::string::npos || p.attrs.find("byref(") != std::string::npos;
        const auto sret = p.attrs.find("sret(");
        if (!top) {
            if (bt) throw Unsupported{"by-value parameter of an inlined callee (copied)"};
            ints.push_back(&p);
            continue;
        }
        const PtrContract* c = nullptr;
        for (auto& k : opt.contracts)
            if (k.param == p.name) c = &k;
        if (!own && !c) {
            harness << "L ptrparam %" << name(p.name) << "\n";
            continue;
        }
        if (bt || sret != std::string::npos) {
            std::optional<ir::Type> ty;
            for (auto* key : {"byval(", "byref(", "sret("}) {
                auto a = p.attrs.find(key);
                if (a == std::string::npos) continue;
                a += std::string_view(key).size();
                int depth = 1;
                auto e = a;
                while (e < p.attrs.size() && depth > 0) {
                    if (p.attrs[e] == '(') ++depth;
                    if (p.attrs[e] == ')') --depth;
                    if (depth > 0) ++e;
                }
                ty = ir::parse_type(p.attrs.substr(a, e - a));
                break;
            }
            uint64_t sz = 0;
            try {
                sz = lay.alloc_size(*ty);
            } catch (const pirmem::Unenc& u) {
                throw Unsupported{"parameter object: " + u.reason};
            }
            if (sz >= kMaxObjSize) throw Unsupported{"parameter object larger than 2^47 bytes"};
            harness << "L glob %" << name(p.name) << " " << sz << " 16 1 " << (bt ? 2 : 0) << " 0\n";
            continue;
        }
        if (c->count < 0) throw Unsupported{"pointer parameter with a symbolic-size contract (assumptions on the size)"};
        const uint64_t esz = pirmem::contract_elem_bytes(lay, f, p, *c);
        if (!esz || static_cast<uint64_t>(c->count) > kMaxObjSize / esz ||
            static_cast<uint64_t>(c->count) * esz >= kMaxObjSize)
            throw Unsupported{"contract object of unknown or too large size"};
        harness << "L glob %" << name(p.name) << " " << static_cast<uint64_t>(c->count) * esz << " 16 "
                << (c->read_only ? 4 : 7) << " 2 0\n";
    }
    b << "L params " << ints.size() + (oom && !top ? 1 : 0);
    for (auto* p : ints) b << " " << name(p->name) << " " << vw(p->ty);
    if (oom && !top) b << " " << kOom << " 64";
    b << "\nL ret " << (f.ret.kind == ir::Type::Void ? 0u : vw(f.ret)) << "\n";
    // translate.cpp's return of a pointer: MemTr::stack_escape_check against
    // the analysed function's own stack objects (the Lean XFunc.escNames)
    if (top && f.ret.kind == ir::Type::Ptr) b << "L retptr\n";
    std::vector<std::string> eg;
    if (top) eg = pirmem::entry_globals(m, lay, f, opt.globals_initial);
    g_entry = top ? &eg : nullptr;
    struct Reset {
        ~Reset() { g_entry = nullptr; }
    } reset;
    for (auto& bl : f.blocks) {
        b << "L block " << name(bl.name) << "\n";
        if (top && &bl == &f.blocks.front()) b << harness.str();
        if (top && &bl == &f.blocks.front())
            for (auto& gname : eg) {
                // MemTr::emit_entry_globals: the object, then its initialiser's stores
                auto e = *pirmem::entry_global(m, lay, gname, opt.globals_initial);
                b << "L glob %@" << name(gname) << " " << e.size << " " << e.align << " "
                  << static_cast<int>(e.kind) << " " << e.init << " " << e.stores.size();
                for (auto& st : e.stores) b << " " << st.off << " " << st.w << " " << st.bits;
                b << "\n";
            }
        if (top && oom && &bl == &f.blocks.front())
            // MemTr::alloc(-1, c64(1), Static, init 1): one zero byte
            b << "L glob %" << kOom << " 1 16 3 1 0\n";
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
                b << "L phi %" << name(in.result) << " " << vw(in.ty) << " " << in.incoming.size();
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
                    if (foreign) {
                        // Frame::site_line: (call-site line, column 0)
                        for (auto& [sl, sc] : opt.signed_shl)
                            if (sc == 0) throw Unsupported{"signed shift location without a column"};
                    } else if (std::find(opt.signed_shl.begin(), opt.signed_shl.end(),
                                         std::make_pair(l.line, l.col)) != opt.signed_shl.end()) {
                        fl += (fl.empty() ? "" : ",") + std::string("csigned");
                    }
                }
                if (in.result.empty() || in.ops.size() != 2) throw Unsupported{op + " shape"};
                b << "L bin %" << name(in.result) << " " << op << " " << word(fl) << " " << width(in.ty) << " "
                  << opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
            } else if (op == "icmp") {
                if (in.result.empty() || in.ops.size() != 2) throw Unsupported{"icmp shape"};
                if (in.ty.kind == ir::Type::Ptr && in.pred != "eq" && in.pred != "ne") {
                    // MemTr::icmp: a relational comparison of pointers checks they share an object
                    if (in.ty.text != "ptr") throw Unsupported{"icmp on " + in.ty.text};
                    b << "L pcmp %" << name(in.result) << " " << in.pred << " " << opnd(in.ops[0]) << " "
                      << opnd(in.ops[1]) << "\n";
                } else {
                    b << "L icmp %" << name(in.result) << " " << in.pred << " " << vw(in.ty) << " "
                      << opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
                }
            } else if (op == "select") {
                if (in.result.empty() || in.ops.size() != 3 || width(in.ops[0].ty) != 1)
                    throw Unsupported{"select shape"};
                b << "L select %" << name(in.result) << " " << vw(in.ty) << " " << opnd(in.ops[0]) << " "
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
                b << "L uninit %" << name(in.result) << " " << vw(in.ty) << "\n";
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
                b << "L load %" << name(in.result) << " " << vw(in.ty) << " " << p << " " << in.align << "\n";
            } else if (op == "store") {
                if (in.ops.size() != 2) throw Unsupported{"store shape"};
                b << "L store " << vw(in.ops[0].ty) << " " << opnd(in.ops[0]) << " " << ptr_opnd(in.ops[1]) << " "
                  << in.align << "\n";
            } else if (op == "freeze") {
                if (in.result.empty() || in.ops.size() != 1) throw Unsupported{"freeze shape"};
                b << "L freeze %" << name(in.result) << " " << vw(in.ty) << " " << opnd(in.ops[0]) << "\n";
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
            } else if (op == "call" && !in.is_asm && in.callee.starts_with("__prism_")) {
                model_line(b, m, in);
            } else if (op == "call" && !in.is_asm && !m.find(in.callee) &&
                       (in.callee.starts_with("__VERIFIER_nondet_") || in.callee.starts_with("nondet_"))) {
                // Tr::call: an arbitrary value (a havoc); an unused result is UNENCODED there
                if (in.result.empty() || in.ty.kind == ir::Type::Void) throw Unsupported{"call @" + in.callee};
                b << "L nondet %" << name(in.result) << " " << vw(in.ty) << "\n";
            } else if (op == "call" && !in.is_asm && !m.find(in.callee) && in.callee == "__VERIFIER_assume") {
                if (in.ops.size() != 1) throw Unsupported{"call @" + in.callee + " arity"};
                b << "L vassume " << opnd(in.ops[0]) << " " << vw(in.ops[0].ty) << "\n";
            } else if (op == "call" && inlined_call(m, in)) {
                const bool cforeign = foreign || foreign_callee(m, in.callee, top_file);
                const bool coom = pirmem::reaches_alloc_failed(m, *m.find(in.callee));
                b << "L call " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
                  << (in.ty.kind == ir::Type::Void ? 0u : vw(in.ty)) << " " << name(in.callee) << " "
                  << in.ops.size() + (coom ? 1 : 0);
                for (auto& a : in.ops) b << " " << opnd(a) << " " << vw(a.ty);
                if (coom) b << " %" << kOom << " 64";
                b << "\n";
                auto it = std::find_if(callees.begin(), callees.end(), [&](const Callee& c) { return c.name == in.callee; });
                if (it == callees.end()) callees.push_back({in.callee, cforeign});
                else if (it->foreign != cforeign)
                    throw Unsupported{"@" + in.callee + " inlined both as library and as user code"};
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
        std::vector<Callee> callees;
        llvm_side(l, m, f, opt, top_file, callees, true, false);
        // every function the calls reach, once, in order of first call
        for (std::size_t i = 0; i < callees.size(); ++i) {
            const Callee c = callees[i];
            l << "L fn " << name(c.name) << "\n";
            llvm_side(l, m, *m.find(c.name), opt, top_file, callees, false, c.foreign);
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
