// LLVM IR (subset) -> PIR, with PRISM's own property instrumentation.
//
// Law 8 polarity: Clang is invoked without any -fsanitize check insertion;
// every property below is inserted here, so what is checked is decided by
// PRISM, never by a compiler flag.
//
//   add/sub/mul nsw        signed overflow            INT-SIGNED-OVF
//   sdiv/srem              divisor 0, INT_MIN / -1    INT-DIV-ZERO / INT-SIGNED-OVF
//   udiv/urem              divisor 0                  INT-DIV-ZERO
//   shl/lshr/ashr          amount >= width            INT-SHIFT-UB
//   shl (C signed <<)      negative base / overflow   INT-SHIFT-UB (located via clang)
//   nuw/exact/disjoint/nneg poison flags              UB-POISON
//   llvm.abs(x, true)      abs(INT_MIN)               INT-SIGNED-OVF
//   llvm.ctlz/cttz(x,true) zero input                 INT-CLZ-ZERO
//   unreachable / trap     reached                    CXX-UNREACHABLE
//   __assert_fail          reached                    FUNC-CONTRACT
//   read of an uninitialised local (instrumented before mem2reg) UNINIT-READ
//   clang-folded UB (poison constant / UB diagnostic)  per diagnostic
//
// Everything else is thrown as "UNENCODED: <construct>" (roadmap 2.1).

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace prism::pir {
namespace {

struct Unenc {
    std::string reason;
};

bool has_flag(const ir::Inst& in, std::string_view f) {
    return std::find(in.flags.begin(), in.flags.end(), f) != in.flags.end();
}

unsigned int_width(const ir::Type& t, std::string_view what = "type") {
    if (t.kind != ir::Type::Int) throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text};
    if (t.bits == 0 || t.bits > 64)
        throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text + " (wider than 64 bits)"};
    return t.bits;
}

bool is_overflow_intrinsic(std::string_view n) {
    for (auto p : {"llvm.sadd.with.overflow.", "llvm.uadd.with.overflow.", "llvm.ssub.with.overflow.",
                   "llvm.usub.with.overflow.", "llvm.smul.with.overflow.", "llvm.umul.with.overflow."})
        if (n.starts_with(p)) return true;
    return false;
}

bool starts(std::string_view s, std::string_view p) { return s.starts_with(p); }

struct Frame {
    const ir::Function* f = nullptr;
    std::string prefix;
    std::map<std::string, Arg> env;
    std::map<std::string, std::pair<int, int>> pairs;  // with.overflow result vars
    std::map<int, Arg> shadow;                          // var -> uninit shadow (i1)
    int ret_block = -1;
    int ret_var = -1;
};

struct PPhi {
    int block = -1;
    int dst = -1;
    // (pred qualified name or "", pred block index when known, value, kind)
    // kind: 0 value, 1 undef, 2 poison
    std::vector<std::tuple<std::string, int, Arg, int>> in;
};

struct Tr {
    const ir::Module& m;
    const TranslateOptions& opt;
    Function out;
    std::map<std::string, int> head, tail;
    std::vector<PPhi> pphis;
    std::vector<std::string> stack;
    std::set<int> folded_used;
    int uniq = 0;

    Tr(const ir::Module& mm, const TranslateOptions& o) : m(mm), opt(o) {}

    ir::DILoc loc(const ir::Inst& in) const {
        if (in.dbg.empty()) return {};
        auto it = m.locs.find(in.dbg);
        return it == m.locs.end() ? ir::DILoc{} : it->second;
    }

    int newvar(const std::string& name, unsigned w) {
        out.vars.push_back(Var{name, w});
        return static_cast<int>(out.vars.size()) - 1;
    }
    int newblock(const std::string& name) {
        out.blocks.push_back(Block{name, {}, {}, {}});
        return static_cast<int>(out.blocks.size()) - 1;
    }
    std::string tmpname(const char* base) { return std::string("_") + base + std::to_string(uniq++); }

    void emit_assign(int b, int dst, Op op, std::vector<Arg> args) {
        Stmt s;
        s.kind = Stmt::Assign;
        s.dst = dst;
        s.op = op;
        s.args = std::move(args);
        out.blocks[static_cast<std::size_t>(b)].stmts.push_back(std::move(s));
    }
    Arg assign(int b, Op op, unsigned w, std::vector<Arg> args, const char* base = "t") {
        int v = newvar(tmpname(base), w);
        emit_assign(b, v, op, std::move(args));
        return Arg::v(v, w);
    }
    void check(int b, Arg viol, std::string prop, std::string cls, std::string msg, int line) {
        Stmt s;
        s.kind = Stmt::Check;
        s.args = {viol};
        s.prop = std::move(prop);
        s.cls = std::move(cls);
        s.msg = std::move(msg);
        s.line = line;
        out.blocks[static_cast<std::size_t>(b)].stmts.push_back(std::move(s));
    }
    void assume(int b, Arg cond) {
        Stmt s;
        s.kind = Stmt::Assume;
        s.args = {cond};
        out.blocks[static_cast<std::size_t>(b)].stmts.push_back(std::move(s));
    }
    Arg havoc(int b, unsigned w, bool uninit, bool nondet, int dst = -1) {
        if (dst < 0) dst = newvar(tmpname(uninit ? "uninit" : "undef"), w);
        Stmt s;
        s.kind = Stmt::Assign;
        s.dst = dst;
        s.op = Op::Havoc;
        s.uninit = uninit;
        s.nondet = nondet;
        out.blocks[static_cast<std::size_t>(b)].stmts.push_back(std::move(s));
        return Arg::v(dst, w);
    }

    int var_of(Frame& fr, const std::string& name) {
        auto it = fr.env.find(name);
        if (it == fr.env.end() || it->second.is_const) return -1;
        return it->second.var;
    }

    // A non-phi use of an operand. Reads of a maybe-uninitialised local are
    // checked here (C11 6.3.2.1p2: automatic object whose address is never
    // taken, read while indeterminate).
    Arg operand(Frame& fr, const ir::Operand& o, int b, int line, bool use = true) {
        switch (o.v.kind) {
            case ir::Value::Int: return Arg::c(int_width(o.ty, "operand type"), o.v.bits);
            case ir::Value::Local: {
                if (fr.pairs.count(o.v.name)) throw Unenc{"UNENCODED: aggregate value %" + o.v.name};
                auto it = fr.env.find(o.v.name);
                if (it == fr.env.end())
                    throw Unenc{"UNENCODED: value %" + o.v.name + " of type " + o.ty.text};
                Arg a = it->second;
                if (use && !a.is_const) {
                    if (auto sh = fr.shadow.find(a.var); sh != fr.shadow.end())
                        check(b, sh->second, "uninit", "UNINIT-READ",
                              "read of an uninitialised local variable", line);
                }
                return a;
            }
            case ir::Value::Undef:
                return havoc(b, int_width(o.ty, "operand type"), false, false);
            case ir::Value::Poison: {
                auto w = int_width(o.ty, "operand type");
                check(b, Arg::c(1, 1), "poison", "UB-POISON",
                      "poison value used (undefined behaviour folded by the front end)", line);
                return havoc(b, w, false, false);
            }
            default:
                throw Unenc{"UNENCODED: operand " + o.ty.text + " " + o.v.text};
        }
    }

    void enter_frame(Frame& fr) {
        const auto& f = *fr.f;
        for (auto& bl : f.blocks) {
            auto q = fr.prefix + bl.name;
            head[q] = newblock(q);
        }
        std::set<std::string> maybe_uninit;
        for (auto& bl : f.blocks) {
            for (auto& in : bl.insts) {
                if (in.result.empty()) continue;
                if (in.op == "call" && is_overflow_intrinsic(in.callee)) {
                    if (in.ty.kind == ir::Type::Struct && in.ty.elems.size() == 2 &&
                        in.ty.elems[0].kind == ir::Type::Int && in.ty.elems[0].bits <= 64 &&
                        in.ty.elems[0].bits > 0) {
                        int a = newvar(fr.prefix + in.result + ".0", in.ty.elems[0].bits);
                        int c = newvar(fr.prefix + in.result + ".1", 1);
                        fr.pairs[in.result] = {a, c};
                    }
                    continue;
                }
                auto ty = in.op == "icmp" ? ir::Type{ir::Type::Int, 1, "i1", {}} : in.ty;
                if (ty.kind == ir::Type::Int && ty.bits > 0 && ty.bits <= 64) {
                    int v = newvar(fr.prefix + in.result, ty.bits);
                    fr.env[in.result] = Arg::v(v, ty.bits);
                    if (in.op == "call" && starts(in.callee, "__prism.uninit.")) {
                        maybe_uninit.insert(in.result);
                        fr.shadow[v] = Arg::c(1, 1);
                    }
                }
            }
        }
        // phis carrying a maybe-uninitialised value (fixpoint)
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    if (in.op != "phi" || in.result.empty() || maybe_uninit.count(in.result)) continue;
                    for (auto& [o, _] : in.incoming)
                        if (o.v.kind == ir::Value::Local && maybe_uninit.count(o.v.name)) {
                            maybe_uninit.insert(in.result);
                            changed = true;
                            break;
                        }
                }
        }
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts)
                if (in.op == "phi" && maybe_uninit.count(in.result)) {
                    int v = var_of(fr, in.result);
                    if (v < 0) continue;
                    int sv = newvar(fr.prefix + in.result + ".uninit", 1);
                    fr.shadow[v] = Arg::v(sv, 1);
                }
    }

    void run_frame(Frame& fr) {
        const auto& f = *fr.f;
        for (auto& bl : f.blocks) {
            auto q = fr.prefix + bl.name;
            int cur = head[q];
            bool noreturn = false;
            bool terminated = false;
            for (auto& in : bl.insts) {
                if (terminated) break;
                auto l = loc(in);
                int line = l.line;
                if (in.op == "phi") {
                    int dst = var_of(fr, in.result);
                    if (dst < 0) throw Unenc{"UNENCODED: phi " + in.ty.text};
                    PPhi p;
                    p.block = cur;
                    p.dst = dst;
                    PPhi ps;
                    auto sh = fr.shadow.find(dst);
                    if (sh != fr.shadow.end()) {
                        ps.block = cur;
                        ps.dst = sh->second.var;
                    }
                    for (auto& [o, pred] : in.incoming) {
                        int kind = o.v.kind == ir::Value::Undef ? 1 : o.v.kind == ir::Value::Poison ? 2 : 0;
                        Arg a;
                        if (kind == 0) a = operand(fr, o, cur, line, /*use=*/false);
                        else a.width = int_width(o.ty);
                        p.in.emplace_back(fr.prefix + pred, -1, a, kind);
                        if (ps.dst >= 0) {
                            Arg s = Arg::c(1, 0);
                            if (kind == 0 && !a.is_const)
                                if (auto it = fr.shadow.find(a.var); it != fr.shadow.end()) s = it->second;
                            ps.in.emplace_back(fr.prefix + pred, -1, s, 0);
                        }
                    }
                    pphis.push_back(std::move(p));
                    if (ps.dst >= 0) pphis.push_back(std::move(ps));
                    continue;
                }
                if (in.op == "br" || in.op == "ret" || in.op == "unreachable" || in.op == "switch") {
                    terminator(fr, in, cur, line, noreturn);
                    tail[q] = cur;
                    terminated = true;
                    continue;
                }
                inst(fr, in, cur, l, noreturn);
                if (noreturn) {
                    // exit/abort/assert: the rest of the block is dead
                    out.blocks[static_cast<std::size_t>(cur)].term = Term{};
                    tail[q] = cur;
                    terminated = true;
                }
            }
            if (!terminated) throw Unenc{"UNENCODED: block without terminator (" + bl.name + ")"};
        }
    }

    void terminator(Frame& fr, const ir::Inst& in, int& cur, int line, bool noreturn) {
        auto& T = out.blocks[static_cast<std::size_t>(cur)].term;
        if (in.op == "switch") throw Unenc{"UNENCODED: switch (lowerswitch did not run)"};
        if (!in.parsed) throw Unenc{"UNENCODED: " + in.op};
        if (in.op == "unreachable") {
            if (!noreturn)
                check(cur, Arg::c(1, 1), "unreachable", "CXX-UNREACHABLE",
                      "unreachable code reached (undefined behaviour)", line);
            out.blocks[static_cast<std::size_t>(cur)].term = Term{};
            return;
        }
        if (in.op == "br") {
            auto tgt = [&](const std::string& n) {
                auto it = head.find(fr.prefix + n);
                if (it == head.end()) throw Unenc{"UNENCODED: branch to unknown block " + n};
                return it->second;
            };
            Term t;
            if (in.targets.size() == 1) {
                t.kind = Term::Jmp;
                t.t = tgt(in.targets[0]);
            } else {
                t.kind = Term::Br;
                if (in.ops[0].ty.kind != ir::Type::Int || in.ops[0].ty.bits != 1)
                    throw Unenc{"UNENCODED: br on " + in.ops[0].ty.text};
                t.cond = operand(fr, in.ops[0], cur, line);
                t.t = tgt(in.targets[0]);
                t.f = tgt(in.targets[1]);
            }
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            return;
        }
        // ret
        std::optional<Arg> v;
        if (!in.ops.empty()) v = operand(fr, in.ops[0], cur, line);
        if (fr.ret_block >= 0) {
            Term t;
            t.kind = Term::Jmp;
            t.t = fr.ret_block;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            if (fr.ret_var >= 0 && v) {
                // continuation phi
                for (auto& p : pphis)
                    if (p.block == fr.ret_block && p.dst == fr.ret_var) {
                        p.in.emplace_back("", cur, *v, 0);
                        return;
                    }
                PPhi p;
                p.block = fr.ret_block;
                p.dst = fr.ret_var;
                p.in.emplace_back("", cur, *v, 0);
                pphis.push_back(std::move(p));
            }
            return;
        }
        Term t;
        t.kind = Term::Ret;
        t.val = v;
        (void)T;
        out.blocks[static_cast<std::size_t>(cur)].term = t;
    }

    int result_var(Frame& fr, const ir::Inst& in) {
        if (in.result.empty()) return -1;
        int v = var_of(fr, in.result);
        if (v < 0) throw Unenc{"UNENCODED: result type " + in.ty.text + " of " + in.op};
        return v;
    }

    Arg p2(int b, Op op, Arg x, Arg y) { return assign(b, op, 1, {x, y}, "c"); }

    void binop(Frame& fr, const ir::Inst& in, int cur, ir::DILoc l) {
        int line = l.line;
        const auto& op = in.op;
        if (op[0] == 'f') throw Unenc{"UNENCODED: " + op + " (floating point)"};
        unsigned w = int_width(in.ty);
        Arg a = operand(fr, in.ops[0], cur, line);
        Arg b = operand(fr, in.ops[1], cur, line);
        a.width = b.width = w;
        bool nsw = has_flag(in, "nsw"), nuw = has_flag(in, "nuw"), exact = has_flag(in, "exact");
        Op r = Op::Add;
        auto zero = Arg::c(w, 0);
        if (op == "add") {
            r = Op::Add;
            if (nsw) check(cur, p2(cur, Op::SAddOvf, a, b), "ovf+", "INT-SIGNED-OVF", "signed addition overflows", line);
            if (nuw) check(cur, p2(cur, Op::UAddOvf, a, b), "wrap+", "UB-POISON", "add nuw wraps (poison)", line);
        } else if (op == "sub") {
            r = Op::Sub;
            if (nsw) check(cur, p2(cur, Op::SSubOvf, a, b), "ovf-", "INT-SIGNED-OVF", "signed subtraction overflows", line);
            if (nuw) check(cur, p2(cur, Op::USubOvf, a, b), "wrap-", "UB-POISON", "sub nuw wraps (poison)", line);
        } else if (op == "mul") {
            r = Op::Mul;
            if (nsw) check(cur, p2(cur, Op::SMulOvf, a, b), "ovf*", "INT-SIGNED-OVF", "signed multiplication overflows", line);
            if (nuw) check(cur, p2(cur, Op::UMulOvf, a, b), "wrap*", "UB-POISON", "mul nuw wraps (poison)", line);
        } else if (op == "udiv" || op == "urem") {
            r = op == "udiv" ? Op::UDiv : Op::URem;
            check(cur, p2(cur, Op::Eq, b, zero), op == "udiv" ? "div0" : "mod0", "INT-DIV-ZERO",
                  "division by zero", line);
            if (exact) check(cur, p2(cur, Op::InexactU, a, b), "exact", "UB-POISON", "udiv exact is inexact (poison)", line);
        } else if (op == "sdiv" || op == "srem") {
            r = op == "sdiv" ? Op::SDiv : Op::SRem;
            check(cur, p2(cur, Op::Eq, b, zero), op == "sdiv" ? "div0" : "mod0", "INT-DIV-ZERO",
                  "division by zero", line);
            check(cur, p2(cur, Op::SDivOvf, a, b), "divovf", "INT-SIGNED-OVF",
                  "INT_MIN / -1 overflows", line);
            if (exact) check(cur, p2(cur, Op::InexactS, a, b), "exact", "UB-POISON", "sdiv exact is inexact (poison)", line);
        } else if (op == "shl") {
            r = Op::Shl;
            check(cur, p2(cur, Op::ShiftOob, a, b), "shift", "INT-SHIFT-UB", "shift amount >= width", line);
            bool signed_c = std::find(opt.signed_shl.begin(), opt.signed_shl.end(),
                                      std::make_pair(l.line, l.col)) != opt.signed_shl.end();
            if (signed_c)
                check(cur, p2(cur, Op::ShlSOvf, a, b), "shift-base", "INT-SHIFT-UB",
                      "signed left shift of a negative value or into/past the sign bit", line);
            if (nsw) check(cur, p2(cur, Op::ShlNswOvf, a, b), "shl-nsw", "UB-POISON", "shl nsw overflows (poison)", line);
            if (nuw) check(cur, p2(cur, Op::ShlNuwOvf, a, b), "shl-nuw", "UB-POISON", "shl nuw overflows (poison)", line);
        } else if (op == "lshr" || op == "ashr") {
            r = op == "lshr" ? Op::LShr : Op::AShr;
            check(cur, p2(cur, Op::ShiftOob, a, b), "shift", "INT-SHIFT-UB", "shift amount >= width", line);
            if (exact)
                check(cur, p2(cur, op == "lshr" ? Op::LostBitsL : Op::LostBitsA, a, b), "exact", "UB-POISON",
                      op + " exact shifts out set bits (poison)", line);
        } else if (op == "and") {
            r = Op::And;
        } else if (op == "or") {
            r = Op::Or;
            if (has_flag(in, "disjoint")) {
                auto both = assign(cur, Op::And, w, {a, b});
                check(cur, p2(cur, Op::Ne, both, zero), "disjoint", "UB-POISON", "or disjoint with common bits (poison)", line);
            }
        } else if (op == "xor") {
            r = Op::Xor;
        } else {
            throw Unenc{"UNENCODED: " + op};
        }
        int dst = result_var(fr, in);
        if (dst >= 0) emit_assign(cur, dst, r, {a, b});
    }

    void inst(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn) {
        int line = l.line;
        const auto& op = in.op;
        if (!in.parsed) {
            std::string what = op;
            if (op == "load" || op == "store" || op == "alloca" || op == "getelementptr")
                what += " (memory model not yet in PIR)";
            throw Unenc{"UNENCODED: " + what};
        }
        if (op == "add" || op == "sub" || op == "mul" || op == "udiv" || op == "sdiv" || op == "urem" ||
            op == "srem" || op == "shl" || op == "lshr" || op == "ashr" || op == "and" || op == "or" ||
            op == "xor" || op == "fadd" || op == "fsub" || op == "fmul" || op == "fdiv" || op == "frem") {
            binop(fr, in, cur, l);
            return;
        }
        if (op == "icmp") {
            if (in.ty.kind == ir::Type::Ptr) throw Unenc{"UNENCODED: icmp on ptr"};
            unsigned w = int_width(in.ty, "icmp operand type");
            Arg a = operand(fr, in.ops[0], cur, line);
            Arg b = operand(fr, in.ops[1], cur, line);
            a.width = b.width = w;
            static const std::map<std::string, Op> preds{
                {"eq", Op::Eq},   {"ne", Op::Ne},   {"ugt", Op::Ugt}, {"uge", Op::Uge}, {"ult", Op::Ult},
                {"ule", Op::Ule}, {"sgt", Op::Sgt}, {"sge", Op::Sge}, {"slt", Op::Slt}, {"sle", Op::Sle}};
            auto it = preds.find(in.pred);
            if (it == preds.end()) throw Unenc{"UNENCODED: icmp " + in.pred};
            emit_assign(cur, result_var(fr, in), it->second, {a, b});
            return;
        }
        if (op == "select") {
            if (in.ops[0].ty.kind != ir::Type::Int || in.ops[0].ty.bits != 1)
                throw Unenc{"UNENCODED: select on " + in.ops[0].ty.text};
            unsigned w = int_width(in.ty, "select type");
            Arg c = operand(fr, in.ops[0], cur, line);
            Arg a = operand(fr, in.ops[1], cur, line);
            Arg b = operand(fr, in.ops[2], cur, line);
            a.width = b.width = w;
            emit_assign(cur, result_var(fr, in), Op::Select, {c, a, b});
            return;
        }
        if (op == "zext" || op == "sext" || op == "trunc") {
            if (in.ops[0].ty.kind != ir::Type::Int || in.ty.kind != ir::Type::Int)
                throw Unenc{"UNENCODED: " + op + " " + in.ops[0].ty.text + " to " + in.ty.text};
            unsigned fw = int_width(in.ops[0].ty), tw = int_width(in.ty);
            Arg a = operand(fr, in.ops[0], cur, line);
            a.width = fw;
            if (op == "trunc" && (has_flag(in, "nuw") || has_flag(in, "nsw")))
                throw Unenc{"UNENCODED: trunc nuw/nsw"};
            if (op == "zext" && has_flag(in, "nneg"))
                check(cur, p2(cur, Op::Slt, a, Arg::c(fw, 0)), "nneg", "UB-POISON", "zext nneg of a negative value (poison)", line);
            (void)tw;
            emit_assign(cur, result_var(fr, in), op == "zext" ? Op::ZExt : op == "sext" ? Op::SExt : Op::Trunc, {a});
            return;
        }
        if (op == "freeze") {
            unsigned w = int_width(in.ty, "freeze type");
            Arg a = operand(fr, in.ops[0], cur, line);
            a.width = w;
            emit_assign(cur, result_var(fr, in), Op::Copy, {a});
            return;
        }
        if (op == "extractvalue") {
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Local || in.indices.size() != 1)
                throw Unenc{"UNENCODED: extractvalue"};
            auto it = fr.pairs.find(in.ops[0].v.name);
            if (it == fr.pairs.end()) throw Unenc{"UNENCODED: extractvalue of " + in.ops[0].ty.text};
            int src = in.indices[0] == 0 ? it->second.first : it->second.second;
            auto w = out.vars[static_cast<std::size_t>(src)].width;
            int dst = result_var(fr, in);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {Arg::v(src, w)});
            return;
        }
        if (op == "call") {
            call(fr, in, cur, l, noreturn);
            return;
        }
        throw Unenc{"UNENCODED: " + op};
    }

    void call(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn) {
        int line = l.line;
        const auto& n = in.callee;
        if (n.empty()) throw Unenc{"UNENCODED: indirect call"};
        auto arg = [&](std::size_t i) {
            if (i >= in.ops.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
            return operand(fr, in.ops[i], cur, line);
        };
        auto flag_arg = [&](std::size_t i) {
            return i < in.ops.size() && in.ops[i].v.kind == ir::Value::Int && in.ops[i].v.bits != 0;
        };
        if (starts(n, "llvm.")) {
            if (is_overflow_intrinsic(n)) {
                auto it = fr.pairs.find(in.result);
                Arg a = arg(0), b = arg(1);
                if (in.result.empty()) return;
                if (it == fr.pairs.end()) throw Unenc{"UNENCODED: call @" + n};
                auto w = out.vars[static_cast<std::size_t>(it->second.first)].width;
                a.width = b.width = w;
                bool s = n[5] == 's';
                auto kind = n.substr(6, 3);  // add / sub / mul
                Op r = kind == "add" ? Op::Add : kind == "sub" ? Op::Sub : Op::Mul;
                Op ov = kind == "add" ? (s ? Op::SAddOvf : Op::UAddOvf)
                        : kind == "sub" ? (s ? Op::SSubOvf : Op::USubOvf)
                                        : (s ? Op::SMulOvf : Op::UMulOvf);
                emit_assign(cur, it->second.first, r, {a, b});
                emit_assign(cur, it->second.second, ov, {a, b});
                return;
            }
            for (auto [p, o] : {std::pair{"llvm.smax.", Op::SMax}, std::pair{"llvm.smin.", Op::SMin},
                                std::pair{"llvm.umax.", Op::UMax}, std::pair{"llvm.umin.", Op::UMin}}) {
                if (starts(n, p)) {
                    unsigned w = int_width(in.ty);
                    Arg a = arg(0), b = arg(1);
                    a.width = b.width = w;
                    emit_assign(cur, result_var(fr, in), o, {a, b});
                    return;
                }
            }
            if (starts(n, "llvm.abs.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                if (flag_arg(1))
                    check(cur, p2(cur, Op::Eq, a, Arg::c(w, uint64_t{1} << (w - 1))), "abs", "INT-SIGNED-OVF",
                          "abs(INT_MIN) overflows", line);
                emit_assign(cur, result_var(fr, in), Op::Abs, {a});
                return;
            }
            if (starts(n, "llvm.ctlz.") || starts(n, "llvm.cttz.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                if (flag_arg(1))
                    check(cur, p2(cur, Op::Eq, a, Arg::c(w, 0)), "clz0", "INT-CLZ-ZERO",
                          "count leading/trailing zeros of 0 is undefined", line);
                emit_assign(cur, result_var(fr, in), starts(n, "llvm.ctlz.") ? Op::Ctlz : Op::Cttz, {a});
                return;
            }
            if (starts(n, "llvm.ctpop.") || starts(n, "llvm.bswap.")) {
                unsigned w = int_width(in.ty);
                if (starts(n, "llvm.bswap.") && w % 16 != 0) throw Unenc{"UNENCODED: call @" + n};
                Arg a = arg(0);
                a.width = w;
                emit_assign(cur, result_var(fr, in), starts(n, "llvm.ctpop.") ? Op::Ctpop : Op::Bswap, {a});
                return;
            }
            if (starts(n, "llvm.expect.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                emit_assign(cur, result_var(fr, in), Op::Copy, {a});
                return;
            }
            if (n == "llvm.assume") {
                assume(cur, arg(0));
                return;
            }
            if (starts(n, "llvm.dbg.") || n == "llvm.donothing" || n == "llvm.sideeffect" ||
                starts(n, "llvm.experimental.noalias.scope.decl") || starts(n, "llvm.pseudoprobe"))
                return;
            if (n == "llvm.trap" || n == "llvm.debugtrap" || n == "llvm.ubsantrap") {
                check(cur, Arg::c(1, 1), "trap", "CXX-UNREACHABLE", "trap reached", line);
                noreturn = true;
                return;
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        if (starts(n, "__prism.uninit.")) {
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, true, false, dst);
            return;
        }
        if (n == "__prism.folded") {
            // marker placed before mem2reg at the instruction carrying a
            // clang UB-folding diagnostic (stage.cpp instrument())
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Int) throw Unenc{"UNENCODED: call @" + n};
            auto k = static_cast<std::size_t>(in.ops[0].v.bits);
            std::string cls = "UB-POISON", msg = "undefined behaviour folded by clang";
            if (k < opt.folded.size()) {
                cls = opt.folded[k].cls;
                msg = "undefined behaviour folded by clang: " + opt.folded[k].msg;
                folded_used.insert(static_cast<int>(k));
            }
            check(cur, Arg::c(1, 1), "folded", cls, msg, line);
            return;
        }
        if (starts(n, "__prism.poison.")) {
            check(cur, Arg::c(1, 1), "folded", "UB-POISON",
                  "undefined behaviour folded to poison by clang (e.g. constant division by zero, "
                  "INT_MIN / -1, over-wide constant shift)", line);
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
            return;
        }
        const bool defined = m.find(n) != nullptr;
        if (!defined) {
            if (starts(n, "__VERIFIER_nondet_") || starts(n, "nondet_")) {
                int dst = result_var(fr, in);
                if (in.ty.kind != ir::Type::Void && dst < 0) throw Unenc{"UNENCODED: call @" + n};
                if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, true, dst);
                out.nondet = true;
                return;
            }
            if (n == "__VERIFIER_assume") {
                Arg c = arg(0);
                assume(cur, p2(cur, Op::Ne, c, Arg::c(c.width, 0)));
                return;
            }
            if (n == "__VERIFIER_error" || n == "reach_error") {
                check(cur, Arg::c(1, 1), "reach_error", "FUNC-CONTRACT", n + "() is reachable", line);
                noreturn = true;
                return;
            }
            if (n == "__assert_fail" || n == "__assert_rtn" || n == "_assert" || n == "__assert" ||
                n == "__assert2" || n == "_wassert") {
                check(cur, Arg::c(1, 1), "assert", "FUNC-CONTRACT", "assertion can fail", line);
                noreturn = true;
                return;
            }
            if (n == "abort") {
                // defined behaviour, but a crash: never a clean PROVED path
                check(cur, Arg::c(1, 1), "abort", "FUNC-CONTRACT", "abort() is reachable (process crash)", line);
                noreturn = true;
                return;
            }
            if (n == "exit" || n == "_Exit" || n == "_exit" || n == "quick_exit") {
                for (std::size_t i = 0; i < in.ops.size(); ++i) arg(i);
                noreturn = true;
                return;
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        inline_call(fr, in, cur, line);
    }

    void inline_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
        const auto& n = in.callee;
        const auto* callee = m.find(n);
        if (std::find(stack.begin(), stack.end(), n) != stack.end())
            throw Unenc{"UNENCODED: recursive call @" + n};
        if (static_cast<int>(stack.size()) > opt.inline_depth)
            throw Unenc{"UNENCODED: call depth > " + std::to_string(opt.inline_depth) + " (@" + n + ")"};
        if (!callee->parse_error.empty())
            throw Unenc{"UNENCODED: call @" + n + " (unparsed IR: " + callee->parse_error + ")"};
        if (callee->ret.kind != ir::Type::Void) int_width(callee->ret, "call @" + n + " returning");
        std::vector<Arg> args;
        for (std::size_t i = 0; i < in.ops.size(); ++i) {
            if (in.ops[i].ty.kind == ir::Type::Ptr) throw Unenc{"UNENCODED: call @" + n + " with ptr argument"};
            args.push_back(operand(fr, in.ops[i], cur, line));
        }
        if (args.size() != callee->params.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
        Frame cf;
        cf.f = callee;
        cf.prefix = n + "#" + std::to_string(uniq++) + ".";
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto w = int_width(callee->params[i].ty, "call @" + n + " parameter");
            args[i].width = w;
            cf.env[callee->params[i].name] = args[i];
        }
        int cont = newblock(fr.prefix + "call." + n + "." + std::to_string(uniq++));
        cf.ret_block = cont;
        if (callee->ret.kind != ir::Type::Void) {
            int rv = in.result.empty() ? newvar(tmpname("ret"), callee->ret.bits) : result_var(fr, in);
            cf.ret_var = rv;
        }
        stack.push_back(n);
        out.inlined.push_back(n);
        enter_frame(cf);
        Term j;
        j.kind = Term::Jmp;
        j.t = head[cf.prefix + callee->blocks.front().name];
        out.blocks[static_cast<std::size_t>(cur)].term = j;
        run_frame(cf);
        stack.pop_back();
        cur = cont;
    }

    void resolve_phis() {
        for (auto& p : pphis) {
            Phi phi;
            phi.dst = p.dst;
            for (auto& [qname, idx, a, kind] : p.in) {
                int pred = idx;
                if (pred < 0) {
                    auto it = tail.find(qname);
                    if (it == tail.end()) continue;  // predecessor never translated (dead)
                    pred = it->second;
                }
                Arg v = a;
                if (kind != 0) {
                    // undef / poison incoming: an unconstrained value on that edge
                    v = havoc(pred, a.width, false, false);
                }
                phi.in.emplace_back(pred, v);
            }
            out.blocks[static_cast<std::size_t>(p.block)].phis.push_back(std::move(phi));
        }
    }
};

}  // namespace

Translation translate(const ir::Module& m, const ir::Function& f, const TranslateOptions& opt) {
    Translation t;
    t.status = std::string(laws::NEEDS_HARNESS);
    if (!f.parse_error.empty()) {
        t.reason = "UNENCODED: unparsed IR (" + f.parse_error + ")";
        return t;
    }
    for (auto& p : f.params) {
        if (p.ty.kind == ir::Type::Ptr) {
            t.reason = "pointer parameter: unguarded model checking reports missing preconditions, "
                       "not defects (Law 6)";
            return t;
        }
    }
    try {
        Tr tr(m, opt);
        tr.out.ir_name = f.name;
        tr.out.name = f.name;
        for (auto& p : f.params) {
            auto w = int_width(p.ty, "parameter type");
            int v = tr.newvar(p.name.empty() ? tr.tmpname("arg") : p.name, w);
            tr.out.params.push_back(v);
        }
        if (f.ret.kind != ir::Type::Void) tr.out.ret_width = int_width(f.ret, "return type");
        if (f.blocks.empty()) throw Unenc{"UNENCODED: empty function body"};
        Frame top;
        top.f = &f;
        for (std::size_t i = 0; i < f.params.size(); ++i)
            top.env[f.params[i].name] = Arg::v(tr.out.params[i], tr.out.vars[static_cast<std::size_t>(tr.out.params[i])].width);
        tr.stack.push_back(f.name);
        tr.enter_frame(top);
        tr.run_frame(top);
        tr.resolve_phis();
        t.fn = std::move(tr.out);
        t.folded_used.assign(tr.folded_used.begin(), tr.folded_used.end());
        t.status.clear();
    } catch (const Unenc& u) {
        t.reason = u.reason;
    }
    return t;
}

}  // namespace prism::pir
