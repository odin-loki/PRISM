// Export (LLVM fragment, PIR) pairs for the Lean correspondence checker
// (roadmap 8.2 "LLVM IR to PIR translation", 2.4 translation validation).
//
// When PRISM_PIR_LEAN_EXPORT is set, every function the pir stage translates
// is appended to <dir>/<unit>.pirl: the LLVM function in the fragment syntax
// of proofs/refinement/PrismRefine/Llvm.lean, and what translate() produced
// (the PIR, or the UNENCODED reason). `pir_lean_check` (proofs/refinement,
// driven by prism-qa pir-lean-check) re-translates the LLVM side with the
// Lean translator -- the one the refinement theorems are proved about -- and
// checks the C++ output is exactly that. Anything outside the fragment is
// written as `L unsupported <why>` and reported, never compared silently.
//
// PRISM_PIR_LEAN_EXPORT=1 writes to <out>/pir-lean/; any other value is the
// directory. Unset (the default): nothing is written.

#include "prism/pir.hpp"

#include "fp.hpp"
#include "translate_mem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
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
        case ir::Value::Null: [[fallthrough]];
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
          << width(in.ops[2].ty) << " " << (n.starts_with("llvm.memmove.") ? 1 : 0) << "\n";
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

// MemTr::const_string: the NUL-terminated literal a (constant GEP into a)
// constant string global holds.
std::optional<std::string> literal_string(const ir::Module& m, const ir::Value& v) {
    const ir::Value* g = &v;
    uint64_t skip = 0;
    if (v.kind == ir::Value::ConstExpr && v.ce_op == "getelementptr" && !v.elems.empty()) {
        g = &v.elems[0].v;
        for (std::size_t k = 1; k < v.elems.size(); ++k) {
            if (v.elems[k].v.kind != ir::Value::Int) return std::nullopt;
            skip += v.elems[k].v.bits;
        }
    }
    if (g->kind != ir::Value::Global) return std::nullopt;
    const auto* gl = m.find_global(g->name);
    if (!gl || !gl->is_const || gl->init.empty() || gl->init[0].v.kind != ir::Value::Str) return std::nullopt;
    const auto& bytes = gl->init[0].v.bytes;
    if (skip > bytes.size()) return std::nullopt;
    auto s = bytes.substr(static_cast<std::size_t>(skip));
    auto nul = s.find('\0');
    if (nul == std::string::npos) return std::nullopt;
    return s.substr(0, nul);
}

// The model intrinsics of Tr::model_intrinsic the extended fragment covers
// (the libc models' malloc/free/new/delete use only these; D136 adds `hhavoc`
// export ahead of the Lean fragment).
// Tr::call's noreturn failure calls (undefined in the module): the PIR check's property and class
std::optional<std::pair<std::string, std::string>> afail_kind(const ir::Module& m, const ir::Inst& in) {
    const std::string& n = in.callee;
    if (n == "llvm.trap" || n == "llvm.debugtrap" || n == "llvm.ubsantrap")
        return std::pair<std::string, std::string>{"trap", "CXX-UNREACHABLE"};
    if (m.find(n)) return std::nullopt;
    if (n == "__assert_fail" || n == "__assert_rtn" || n == "_assert" || n == "__assert" || n == "__assert2" ||
        n == "_wassert")
        return std::pair<std::string, std::string>{"assert", "FUNC-CONTRACT"};
    if (n == "__VERIFIER_error" || n == "reach_error")
        return std::pair<std::string, std::string>{"reach_error", "FUNC-CONTRACT"};
    if (n == "_ZSt21__glibcxx_assert_failPKciS0_S0_" || n == "_ZSt21__glibcxx_assert_failv") {
        // libstdc++ precondition: the class from the condition text (as Tr::call)
        std::string cond = in.ops.size() > 3 ? literal_string(m, in.ops[3].v).value_or("") : "";
        std::string cls = "FUNC-CONTRACT";
        if (cond.find("size()") != std::string::npos || cond.find("_Nm") != std::string::npos)
            cls = "MEM-OOB-READ";
        else if (cond.find("_M_is_engaged") != std::string::npos || cond.find("has_value") != std::string::npos)
            cls = "CXX-OPTIONAL-NULL";
        else if (cond.find("pointer()") != std::string::npos || cond.find("nullptr") != std::string::npos)
            cls = "PTR-NULL-DEREF";
        return std::pair<std::string, std::string>{"precondition", cls};
    }
    return std::nullopt;
}

// After a noreturn failure call at `i`: the rest defines no variable (Tr::enter_frame numbers every
// result) and ends in unreachable, ret, or an unconditional br (normal_blocks scans the whole block).
// translate_ctl.inc's std_throw_type (the typeinfo of a std::__throw_* call)
std::string std_throw_ti(const std::string& fn) {
    static const std::vector<std::pair<std::string, std::string>> k{
        {"__throw_out_of_range_fmt", "_ZTISt12out_of_range"},
        {"__throw_out_of_range", "_ZTISt12out_of_range"},
        {"__throw_length_error", "_ZTISt12length_error"},
        {"__throw_bad_alloc", "_ZTISt9bad_alloc"},
        {"__throw_bad_array_new_length", "_ZTISt20bad_array_new_length"},
        {"__throw_logic_error", "_ZTISt11logic_error"},
        {"__throw_invalid_argument", "_ZTISt16invalid_argument"},
        {"__throw_domain_error", "_ZTISt12domain_error"},
        {"__throw_runtime_error", "_ZTISt13runtime_error"},
        {"__throw_range_error", "_ZTISt11range_error"},
        {"__throw_overflow_error", "_ZTISt14overflow_error"},
        {"__throw_underflow_error", "_ZTISt15underflow_error"},
        {"__throw_bad_optional_access", "_ZTISt19bad_optional_access"},
        {"__throw_bad_function_call", "_ZTISt17bad_function_call"},
        {"__throw_bad_variant_access", "_ZTISt18bad_variant_access"},
        {"__throw_bad_cast", "_ZTISt8bad_cast"},
        {"__throw_bad_typeid", "_ZTISt10bad_typeid"},
    };
    for (auto& [pre, ti] : k) {
        auto p = fn.find(pre);
        if (p == std::string::npos) continue;
        auto e = p + pre.size();
        if (e < fn.size() && (std::islower(static_cast<unsigned char>(fn[e])) || fn[e] == '_') && fn.compare(e, 3, "PKc") != 0 &&
            fn[e] != 'v' && fn[e] != 'b' && fn[e] != 'i')
            continue;
        return ti;
    }
    return fn;
}

bool afail_tail_ok(const ir::Block& bl, size_t i) {
    for (size_t j = i + 1; j < bl.insts.size(); ++j) {
        auto& x = bl.insts[j];
        if (!x.result.empty() || x.op == "invoke" || x.op == "switch") return false;
    }
    auto& t = bl.insts.back();
    return t.op == "unreachable" || t.op == "ret" || (t.op == "br" && t.targets.size() == 1);
}

void model_line(std::ostream& b, const ir::Module& m, const ir::Inst& in) {
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
    if (n == "__prism_fresh_cstr") {
        // MemTr::fresh_cstr(kind): a drawn length below 4096, n+1 arbitrary bytes, the NUL (Lean `hfresh`)
        if (in.ops.size() != 1) throw Unsupported{"call @" + n + " arity"};
        const uint64_t k = cint(0);
        if (k > 255) throw Unsupported{"call @" + n + " kind"};
        b << "L hfresh " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " " << k << "\n";
        return;
    }
    if (n == "__prism_free" || n == "__prism_free_check") {
        // MemTr::dealloc: the free checks, then (free) the lifetime ends
        if (in.ops.size() != 2) throw Unsupported{"call @" + n + " arity"};
        const uint64_t k = cint(1);
        if (k > 255) throw Unsupported{"call @" + n + " kind"};
        b << "L hfree " << opnd(in.ops[0]) << " " << k << " " << (n == "__prism_free" ? 1 : 0) << "\n";
        return;
    }
    if (n == "__prism_assume") {
        if (in.ops.size() != 1) throw Unsupported{"call @" + n + " arity"};
        b << "L vassume " << opnd(in.ops[0]) << " " << vw(in.ops[0].ty) << "\n";
        return;
    }
    if (n == "__prism_obj_size") {
        // MemTr::obj_size_remaining: bytes from p to the end of its object
        if (in.ops.size() != 1) throw Unsupported{"call @" + n + " arity"};
        if (in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr") throw Unsupported{"call @" + n + " type"};
        if (!in.result.empty() && vw(in.ty) != 64) throw Unsupported{"call @" + n + " type"};
        b << "L hobjsz " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
          << opnd(in.ops[0]) << "\n";
        return;
    }
    if (n == "__prism_memcpy") {
        // MemTr::memcpy_(dst, src, len, overlap_ok): same L memcpy as llvm.memcpy/memmove
        if (in.ops.size() != 4) throw Unsupported{"call @" + n + " arity"};
        const uint64_t move = cint(3) != 0 ? 1u : 0u;
        b << "L memcpy " << ptr_opnd(in.ops[0]) << " " << ptr_opnd(in.ops[1]) << " " << opnd(in.ops[2]) << " "
          << vw(in.ops[2].ty) << " " << move << " 0\n";
        return;
    }
    if (n == "__prism_alloc_failed") {
        // Tr::st(oom, i8 1): a store of one byte, alignment 8, into the flag object
        b << "L store 8 #1 %" << kOom << " 8\n";
        return;
    }
    if (n == "__prism_memset") {
        // MemTr::memset_ with an i32 byte, truncated after the checks (Lean `hmemset`)
        if (in.ops.size() != 3 || in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
            throw Unsupported{"call @" + n + " arity"};
        if (vw(in.ops[1].ty) != 32 || vw(in.ops[2].ty) != 64) throw Unsupported{"call @" + n + " type"};
        b << "L hmemset " << ptr_opnd(in.ops[0]) << " " << opnd(in.ops[1]) << " " << opnd(in.ops[2]) << "\n";
        return;
    }
    if (n == "__prism_read_range") {
        // MemTr::read_range: the guarded read checks of n bytes at p (Lean `hread`)
        if (in.ops.size() != 2 || in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
            throw Unsupported{"call @" + n + " arity"};
        if (vw(in.ops[1].ty) != 64) throw Unsupported{"call @" + n + " type"};
        b << "L hread " << ptr_opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
        return;
    }
    if (n == "__prism_check") {
        // Tr::check(ok == 0, "model", cls): the class is a literal string (Lean `vcheck`)
        if (in.ops.size() != 3 || in.ops[0].ty.kind != ir::Type::Int) throw Unsupported{"call @" + n + " arity"};
        auto cls = literal_string(m, in.ops[1].v);
        if (!cls || cls->empty() || cls->find_first_of(" \t\r\n") != std::string::npos)
            throw Unsupported{"call @" + n + " class"};
        b << "L vcheck " << opnd(in.ops[0]) << " " << vw(in.ops[0].ty) << " " << *cls << "\n";
        return;
    }
    if (n == "__prism_havoc_bytes") {
        // MemTr::write_havoc (D136 stdio models): Lean `hhavoc` (XHavoc.lean, proved)
        if (in.ops.size() != 2 || in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
            throw Unsupported{"call @" + n + " arity"};
        if (vw(in.ops[1].ty) != 64) throw Unsupported{"call @" + n + " type"};
        b << "L hhavoc " << ptr_opnd(in.ops[0]) << " " << opnd(in.ops[1]) << "\n";
        return;
    }
    throw Unsupported{"call @" + n};
}

// translate.cpp's normal_blocks: the blocks reachable from the entry along
// br, switch and the normal destination of invoke. A landing pad, and the
// code only it reaches, is translated only once a throw edge reaches it
// (eh_pass); nothing in the Lean fragment throws, so those blocks stay empty.
std::set<std::string> normal_blocks(const ir::Function& f) {
    std::map<std::string, const ir::Block*> by;
    for (auto& b : f.blocks) by[b.name] = &b;
    std::set<std::string> seen;
    std::vector<std::string> work{f.blocks.front().name};
    while (!work.empty()) {
        auto n = work.back();
        work.pop_back();
        if (!seen.insert(n).second || !by.count(n)) continue;
        for (auto& in : by[n]->insts) {
            if (in.op == "br" || in.op == "switch")
                for (auto& t : in.targets) work.push_back(t);
            if (in.op == "switch")
                for (auto& [c, t] : in.cases) work.push_back(t);
            if (in.op == "invoke" && !in.targets.empty()) work.push_back(in.targets[0]);
        }
    }
    return seen;
}

// The widths of the variables Tr::enter_frame numbers for a result (in order):
// an overflow intrinsic's { iN, i1 }, a landing pad's { ptr, i32 }, a
// two-field scalar aggregate of insertvalue/call/invoke/load, else one
// variable for an integer, pointer, IEEE or compare result of at most 64 bits.
std::vector<unsigned> frame_vars(const ir::Inst& in) {
    if (in.result.empty()) return {};
    const auto& t = in.ty;
    auto pw = [](const ir::Type& e) -> unsigned {
        if (e.kind == ir::Type::Ptr && e.text == "ptr") return 64;
        if (e.kind == ir::Type::Int && e.bits > 0 && e.bits <= 64) return e.bits;
        return 0;
    };
    if (overflow_call(in)) {
        if (t.kind == ir::Type::Struct && t.elems.size() == 2 && t.elems[0].kind == ir::Type::Int &&
            t.elems[0].bits <= 64 && t.elems[0].bits > 0)
            return {t.elems[0].bits, 1};
        return {};
    }
    if (in.op == "landingpad" && t.kind == ir::Type::Struct && t.elems.size() == 2 &&
        t.elems[0].kind == ir::Type::Ptr && t.elems[1].kind == ir::Type::Int && t.elems[1].bits == 32)
        return {64, 32};
    if ((in.op == "insertvalue" || in.op == "call" || in.op == "invoke" || in.op == "load") &&
        t.kind == ir::Type::Struct && t.elems.size() == 2 && pw(t.elems[0]) && pw(t.elems[1]))
        return {pw(t.elems[0]), pw(t.elems[1])};
    if (in.op == "icmp" || in.op == "fcmp") return {1};
    if (t.kind == ir::Type::Ptr && t.text == "ptr") return {64};
    if (auto fw = fp::width_of(t)) return {*fw};
    if (t.kind == ir::Type::Int && t.bits > 0 && t.bits <= 64) return {t.bits};
    return {};
}

// A block normal_blocks leaves out, as `L dead` placeholders for the variables
// Tr::enter_frame numbers there, then `L stub` (the Lean translator refuses a
// stub block it finds reachable). Refused where Tr::enter_frame would add a
// shadow variable the Lean side does not mirror: a maybe-uninitialised phi
// (`.uninit`) or a may-undef value (`.undef`, undef_analysis).
void dead_block(std::ostream& b, const ir::Function& f, const ir::Block& bl,
                const std::set<std::string>& maybe_uninit) {
    std::map<std::string, const ir::Inst*> def;
    for (auto& x : f.blocks)
        for (auto& in : x.insts)
            if (!in.result.empty()) def[in.result] = &in;
    for (auto& in : bl.insts) {
        if (!in.parsed) throw Unsupported{in.op + " (unreachable block)"};
        if (in.op == "phi" && maybe_uninit.count(in.result))
            throw Unsupported{"maybe-uninitialised phi in an unreachable block"};
        // undef_analysis allocates a shadow only for values computed from undef:
        // none here unless an undef operand (other than the aggregate an
        // exception pair is rebuilt into) or a field read of a rebuilt pair
        for (auto& [o, _] : in.incoming)
            if (o.v.kind == ir::Value::Undef) throw Unsupported{"undef in an unreachable block"};
        for (std::size_t i = 0; i < in.ops.size(); ++i)
            if (in.ops[i].v.kind == ir::Value::Undef &&
                !(in.op == "insertvalue" && i == 0 && frame_vars(in).size() == 2))
                throw Unsupported{"undef in an unreachable block"};
        if (in.op == "extractvalue") {
            const ir::Inst* d = !in.ops.empty() && in.ops[0].v.kind == ir::Value::Local
                                    ? (def.count(in.ops[0].v.name) ? def[in.ops[0].v.name] : nullptr)
                                    : nullptr;
            if (!d || d->op != "landingpad") throw Unsupported{"extractvalue in an unreachable block"};
        }
        auto ws = frame_vars(in);
        if (ws.empty()) continue;
        b << "L dead %" << name(in.result);
        for (auto w : ws) b << " " << w;
        b << "\n";
    }
    b << "L stub\n";
}

// Tr::enter_frame's maybe_uninit: the `__prism.uninit.` results and the phis
// that carry one (fixpoint).
std::set<std::string> maybe_uninit_of(const ir::Function& f) {
    std::set<std::string> mu;
    for (auto& bl : f.blocks)
        for (auto& in : bl.insts)
            if (in.op == "call" && in.callee.starts_with("__prism.uninit.") && !in.result.empty() &&
                frame_vars(in).size() == 1)
                mu.insert(in.result);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                if (in.op != "phi" || in.result.empty() || mu.count(in.result)) continue;
                for (auto& [o, _] : in.incoming)
                    if (o.v.kind == ir::Value::Local && mu.count(o.v.name)) {
                        mu.insert(in.result);
                        changed = true;
                        break;
                    }
            }
    }
    return mu;
}

void llvm_side(std::ostream& o, const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
               const std::string& top_file, std::vector<Callee>& callees, bool top, bool foreign) {
    if (!f.parse_error.empty()) throw Unsupported{"unparsed IR"};
    std::ostringstream b;
    static const std::set<std::string> bins{"add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                            "shl", "lshr", "ashr", "and", "or", "xor"};
    const pirmem::Layout lay(m);
    // Tr::is_ptrtoint: a register the function defines by ptrtoint
    auto is_ptrtoint = [&](const ir::Operand& o) {
        if (o.v.kind != ir::Value::Local) return false;
        const auto* d = def_of(f, o.v.name);
        return d != nullptr && d->op == "ptrtoint";
    };
    // the hidden allocation-failed flag (translate.cpp: preassigned when the
    // analysed function can reach __prism_alloc_failed): an object after the
    // entry globals, passed to every inlined function that can reach it as an
    // extra last parameter (the translator refers to its variable directly)
    const bool oom = pirmem::reaches_alloc_failed(m, f);
    const bool caught = pirmem::reaches_caught_slot(m, f);
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
                if (!ty.has_value()) throw Unsupported{"parameter object type missing"};
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
    // the function's own stack objects (the Lean XFunc.escNames; a callee: outside)
    if (f.ret.kind == ir::Type::Ptr) b << "L retptr\n";
    std::vector<std::string> eg;
    if (top) eg = pirmem::entry_globals(m, lay, f, opt.globals_initial);
    g_entry = top ? &eg : nullptr;
    struct Reset {
        ~Reset() { g_entry = nullptr; }
    } reset;
    const auto live = normal_blocks(f);
    const auto muninit = maybe_uninit_of(f);
    // the blocks eh_pass may translate (Tr::region from a landing pad): those
    // reached from an invoke's unwind label along br, switch and invoke normal
    // edges, outside normal_blocks; exported in full (translated only once a
    // throw edge reaches their landing pad), a stub where the export fails
    std::set<std::string> padset;
    {
        std::map<std::string, const ir::Block*> by;
        for (auto& x : f.blocks) by[x.name] = &x;
        std::vector<std::string> work;
        auto unw = [&](const ir::Block& x) {
            for (auto& in : x.insts)
                if (in.op == "invoke" && in.targets.size() == 2) work.push_back(in.targets[1]);
        };
        for (auto& x : f.blocks)
            if (live.count(x.name)) unw(x);
        while (!work.empty()) {
            auto n = work.back();
            work.pop_back();
            if (live.count(n) || !by.count(n) || !padset.insert(n).second) continue;
            unw(*by[n]);
            for (auto& in : by[n]->insts) {
                if (in.op == "br" || in.op == "switch")
                    for (auto& t : in.targets) work.push_back(t);
                if (in.op == "switch")
                    for (auto& [c, t] : in.cases) work.push_back(t);
                if (in.op == "invoke" && !in.targets.empty()) work.push_back(in.targets[0]);
            }
        }
    }
    auto emit_block = [&](std::ostream& b, const ir::Block& bl0) {
        // an invoke: translate.cpp's Tr::translate_block translates it as the
        // call (its landing pad the handler of a throw inside), then the normal
        // edge. Nothing the Lean fragment admits throws (__cxa_throw, resume,
        // std::__throw_* are all outside), so the handler is never used and the
        // landing pads stay unreachable: the call, then `br` to the normal label.
        ir::Block bl = bl0;
        std::string invoke_pad;
        const bool invoke_end = !bl.insts.empty() && bl.insts.back().op == "invoke";
        if (invoke_end) {
            ir::Inst& inv = bl.insts.back();
            if (inv.targets.size() != 2) throw Unsupported{"invoke shape"};
            ir::Inst br;
            br.op = "br";
            br.parsed = true;
            br.targets = {inv.targets[0]};
            invoke_pad = inv.targets[1];
            inv.op = "call";
            inv.targets.clear();
            bl.insts.push_back(std::move(br));
        }
        // the call an invoke became (Tr::handlers has its landing pad while the callee runs)
        const ir::Inst* invoke_call = invoke_end ? &bl.insts[bl.insts.size() - 2] : nullptr;
        if (top && &bl0 == &f.blocks.front()) b << harness.str();
        if (top && &bl0 == &f.blocks.front())
            for (auto& gname : eg) {
                // MemTr::emit_entry_globals: the object, then its initialiser's stores
                auto e = *pirmem::entry_global(m, lay, gname, opt.globals_initial);
                b << "L glob %@" << name(gname) << " " << e.size << " " << e.align << " "
                  << static_cast<int>(e.kind) << " " << e.init << " " << e.stores.size();
                for (auto& st : e.stores) b << " " << st.off << " " << st.w << " " << st.bits;
                b << "\n";
            }
        if (top && oom && &bl0 == &f.blocks.front())
            // MemTr::alloc(-1, c64(1), Static, init 1): one zero byte
            b << "L glob %" << kOom << " 1 16 3 1 0\n";
        if (top && caught && &bl0 == &f.blocks.front())
            // Tr::caught_slot: MemTr::alloc(-1, c64(72), Static, init 1), after the oom flag
            b << "L glob %__prism.caught 72 16 3 1 0\n";
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
                if (!(op == "landingpad" && fl == "cleanup") && fl != "nsw" && fl != "nuw" && fl != "exact" && fl != "disjoint" && fl != "nneg")
                    throw Unsupported{in.op + " " + fl};
            if (op == "phi" && padset.count(bl0.name) && std::ranges::any_of(bl0.insts, [](const ir::Inst& x) {
                    return x.op == "landingpad";
                }))
                throw Unsupported{"phi in a landing pad"};
            if (op == "phi") {
                // resolve_phis drops an incoming from a block run_frame never
                // translated (normal_blocks does not reach it: a catch handler
                // joining the normal path). Kept where Tr::enter_frame's
                // shadows saw it: an undef or maybe-uninitialised input.
                std::vector<std::pair<ir::Operand, std::string>> inc;
                for (auto& [v, pred] : in.incoming) {
                    if (live.count(pred)) {
                        inc.emplace_back(v, pred);
                        continue;
                    }
                    if (v.v.kind == ir::Value::Undef ||
                        (v.v.kind == ir::Value::Local && muninit.count(v.v.name)))
                        throw Unsupported{"phi: undef or uninitialised input from an unreachable block"};
                }
                b << "L phi %" << name(in.result) << " " << vw(in.ty) << " " << inc.size();
                for (auto& [v, pred] : inc) b << " " << opnd(v) << " " << name(pred);
                b << "\n";
            } else if (op == "sub" && in.ops.size() == 2 && !in.result.empty() && is_ptrtoint(in.ops[0]) &&
                       is_ptrtoint(in.ops[1])) {
                // Tr::binop: MemTr::ptr_sub_check (same object), then the subtraction
                if (in.ty.kind != ir::Type::Int || in.ty.bits != 64) throw Unsupported{"sub of ptrtoint width"};
                std::string fl;
                for (auto f2 : {"nsw", "nuw"})
                    if (has(in, f2)) fl += (fl.empty() ? "" : ",") + std::string(f2);
                b << "L psub %" << name(in.result) << " " << word(fl) << " " << opnd(in.ops[0]) << " "
                  << opnd(in.ops[1]) << "\n";
            } else if (op == "ptrtoint") {
                // Tr::inst: a copy of the 64-bit pointer (every use is a sub of two ptrtoint values there)
                if (in.result.empty() || in.ops.size() != 1 || in.ty.kind != ir::Type::Int || in.ty.bits != 64 ||
                    in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
                    throw Unsupported{"ptrtoint shape"};
                b << "L ptrtoint %" << name(in.result) << " " << opnd(in.ops[0]) << "\n";
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
            } else if (op == "call" && in.callee.starts_with("llvm.") && !in.is_asm &&
                       !(afail_kind(m, in) && afail_tail_ok(bl, static_cast<size_t>(&in - bl.insts.data())))) {
                intrinsic_line(b, in);
            } else if (op == "landingpad") {
                // Tr::enter_frame's { exn, selector } pair; the throw edges set it
                if (frame_vars(in).size() != 2) throw Unsupported{"landingpad type"};
                b << "L lpad %" << name(in.result) << " "
                  << (std::find(in.flags.begin(), in.flags.end(), "cleanup") != in.flags.end() ? 1 : 0) << " "
                  << in.clauses.size();
                for (auto& [kind, o2] : in.clauses) {
                    if (kind != "catch") b << " f";
                    else if (o2.v.kind == ir::Value::Null) b << " null";
                    else if (o2.v.kind == ir::Value::Global && plain_name(o2.v.name)) b << " " << o2.v.name;
                    else throw Unsupported{"landingpad clause"};
                }
                b << "\n";
            } else if (op == "insertvalue") {
                // Tr::inst: a field of a two-field pair (the exception pair rebuilt before resume)
                auto ws = frame_vars(in);
                if (ws.size() != 2 || in.ops.size() != 2 || in.indices.size() != 1 || in.indices[0] > 1)
                    throw Unsupported{"insertvalue"};
                const auto& agg = in.ops[0];
                std::string a;
                if (agg.v.kind == ir::Value::Undef || agg.v.kind == ir::Value::Poison) a = "undef";
                else if (agg.v.kind == ir::Value::Local) a = "%" + name(agg.v.name);
                else throw Unsupported{"insertvalue into a constant"};
                if (in.ops[1].v.kind == ir::Value::Undef || in.ops[1].v.kind == ir::Value::Poison)
                    throw Unsupported{"insertvalue of undef"};
                b << "L insv %" << name(in.result) << " " << ws[0] << " " << ws[1] << " " << a << " "
                  << opnd(in.ops[1]) << " " << in.indices[0] << "\n";
            } else if (op == "resume") {
                if (in.ops.size() != 1 || in.ops[0].v.kind != ir::Value::Local) throw Unsupported{"resume"};
                b << "L resume %" << name(in.ops[0].v.name) << "\n";
            } else if (op == "extractvalue" && in.ops.size() == 1 && in.ops[0].v.kind == ir::Value::Local &&
                       def_of(f, in.ops[0].v.name) &&
                       (def_of(f, in.ops[0].v.name)->op == "landingpad" ||
                        def_of(f, in.ops[0].v.name)->op == "insertvalue")) {
                // a field of an exception pair
                if (in.indices.size() != 1 || in.indices[0] > 1 || in.result.empty() ||
                    frame_vars(*def_of(f, in.ops[0].v.name)).size() != 2)
                    throw Unsupported{"extractvalue"};
                b << "L xv %" << name(in.result) << " " << vw(in.ty) << " %" << name(in.ops[0].v.name) << " "
                  << in.indices[0] << "\n";
            } else if (op == "extractvalue") {
                // a field of an overflow intrinsic's { iN, i1 } result
                const auto* d = in.ops.size() == 1 && in.ops[0].v.kind == ir::Value::Local
                                    ? def_of(f, in.ops[0].v.name) : nullptr;
                if (!d || !overflow_call(*d) || in.indices.size() != 1 || in.result.empty())
                    throw Unsupported{"extractvalue"};
                b << "L xv %" << name(in.result) << " " << width(in.ty) << " %" << name(in.ops[0].v.name) << " "
                  << in.indices[0] << "\n";
            } else if (op == "call" && !in.is_asm && afail_kind(m, in) &&
                       afail_tail_ok(bl, static_cast<size_t>(&in - bl.insts.data()))) {
                // Tr::call: check(1, prop, cls), then the rest of the block is dead (noreturn);
                // an unconditional br still makes its target reachable (normal_blocks)
                auto [prop, cls] = *afail_kind(m, in);
                auto& t = bl.insts.back();
                b << "L afail " << prop << " " << cls;
                if (t.op == "br") b << " br " << name(t.targets[0]);
                b << "\n";
                break;
            } else if (op == "call" && !in.is_asm && in.callee.starts_with("_ZSt") &&
                       in.callee.find("__throw_") != std::string::npos && !m.find(in.callee) &&
                       afail_tail_ok(bl, static_cast<size_t>(&in - bl.insts.data())) &&
                       bl.insts.back().op == "unreachable") {
                // Tr::call: std::__throw_* raises a library exception (Tr::raise); the
                // semantics stops following the run there, the PIR after it is unchecked.
                // Under an invoke (`L icall` on the inlined path) a handler may catch it:
                // the Lean translator refuses that (dispatch not modelled)
                // Tr::eh_call evaluates the non-float arguments first (their checks)
                b << "L throw " << std_throw_ti(in.callee);
                for (auto& a : in.ops)
                    if (a.ty.kind != ir::Type::Float) b << " " << opnd(a) << " " << vw(a.ty);
                b << "\n";
                break;
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
                b << (&in == invoke_call ? "L ivk " + name(invoke_pad) + " " : std::string("L call ")) << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
                  << (in.ty.kind == ir::Type::Void ? 0u : vw(in.ty)) << " " << name(in.callee) << " "
                  << in.ops.size() + (coom ? 1 : 0);
                for (auto& a : in.ops) b << " " << opnd(a) << " " << vw(a.ty);
                if (coom) b << " %" << kOom << " 64";
                b << "\n";
                auto it = std::ranges::find_if(callees, [&](const Callee& c) { return c.name == in.callee; });
                if (it == callees.end()) callees.push_back({in.callee, cforeign});
                else if (it->foreign != cforeign)
                    throw Unsupported{"@" + in.callee + " inlined both as library and as user code"};
            } else if (op == "call") {
                throw Unsupported{"call @" + (in.callee.empty() ? std::string("<indirect>") : in.callee)};
            } else {
                throw Unsupported{op};
            }
        }
    };
    for (auto& bl0 : f.blocks) {
        b << "L block " << name(bl0.name) << "\n";
        if (live.count(bl0.name)) {
            emit_block(b, bl0);
            continue;
        }
        if (padset.count(bl0.name)) {
            std::ostringstream pb;
            try {
                emit_block(pb, bl0);
                b << pb.str();
                continue;
            } catch (const Unsupported& u) {
                if (std::getenv("PRISM_LEAN_PAD_DEBUG"))
                    std::cerr << "pir-lean: landing pad " << f.name << "/" << bl0.name << ": " << u.why << "\n";
            }
        }
        dead_block(b, f, bl0, muninit);
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
                case Stmt::MemCpy: [[fallthrough]];
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
            const Callee c = callees.at(i);
            l << "L fn " << name(c.name) << "\n";
            llvm_side(l, m, *m.find(c.name), opt, top_file, callees, false, c.foreign);
        }
        if (!callees.empty()) o << "L depth " << opt.inline_depth << "\n";
        {
            // Tr::thrown_types: every type the module throws (sorted)
            std::set<std::string> ts;
            for (auto& g : m.functions)
                for (auto& x : g.blocks)
                    for (auto& in : x.insts) {
                        if (in.op != "call" && in.op != "invoke") continue;
                        if (in.callee == "__cxa_throw" && in.ops.size() >= 2 && in.ops[1].v.kind == ir::Value::Global)
                            ts.insert(in.ops[1].v.name);
                        else if (in.callee.starts_with("_ZSt") && in.callee.find("__throw_") != std::string::npos)
                            ts.insert(std_throw_ti(in.callee));
                    }
            if (std::ranges::all_of(ts, [](const std::string& t) { return plain_name(t); })) {
                o << "L thrown";
                for (auto& t : ts) o << " " << t;
                o << "\n";
            }
        }
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
