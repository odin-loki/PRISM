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

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
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
        case ir::Value::Local: return "%" + name(o.v.name);
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

void llvm_side(std::ostream& o, const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
               const std::string& top_file, std::vector<std::string>& callees) {
    if (!f.parse_error.empty()) throw Unsupported{"unparsed IR"};
    std::ostringstream b;
    b << "L params " << f.params.size();
    for (auto& p : f.params) b << " " << name(p.name) << " " << width(p.ty);
    b << "\nL ret " << (f.ret.kind == ir::Type::Void ? 0u : width(f.ret)) << "\n";
    static const std::set<std::string> bins{"add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                            "shl", "lshr", "ashr", "and", "or", "xor"};
    for (auto& bl : f.blocks) {
        b << "L block " << name(bl.name) << "\n";
        for (auto& in : bl.insts) {
            if (!in.parsed) throw Unsupported{in.op};
            for (auto& fl : in.flags)
                if (fl != "nsw" && fl != "nuw" && fl != "exact" && fl != "disjoint" && fl != "nneg")
                    throw Unsupported{in.op + " " + fl};
            const std::string& op = in.op;
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
            } else if (op == "freeze") {
                if (in.result.empty() || in.ops.size() != 1) throw Unsupported{"freeze shape"};
                b << "L freeze %" << name(in.result) << " " << width(in.ty) << " " << opnd(in.ops[0]) << "\n";
            } else if (op == "call" && inlined_call(m, in, top_file)) {
                b << "L call " << (in.result.empty() ? std::string("-") : "%" + name(in.result)) << " "
                  << (in.ty.kind == ir::Type::Void ? 0u : width(in.ty)) << " " << name(in.callee) << " "
                  << in.ops.size();
                for (auto& a : in.ops) b << " " << opnd(a) << " " << width(a.ty);
                b << "\n";
                if (std::find(callees.begin(), callees.end(), in.callee) == callees.end()) callees.push_back(in.callee);
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
                default:
                    // a memory statement: written so the checker can never equate
                    // it with anything (it is not in the Lean PIR syntax)
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
