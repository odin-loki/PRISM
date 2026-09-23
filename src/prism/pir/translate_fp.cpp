// LLVM floating point -> PIR FP operators (see translate_fp.hpp).

#include "translate_fp.hpp"

#include "fp.hpp"

#include <algorithm>
#include <map>

namespace prism::pir::pirfp {

using pirmem::Unenc;

namespace {

bool starts(std::string_view s, std::string_view p) { return s.starts_with(p); }

uint64_t sign_bit(unsigned w) { return uint64_t{1} << (w - 1); }
uint64_t wmask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

// libm name -> exact PIR operator (and the float-suffixed variant)
const std::map<std::string, Op>& libm_exact() {
    static const std::map<std::string, Op> k = [] {
        std::map<std::string, Op> t;
        for (auto [n, o] : {std::pair{"sqrt", Op::FSqrt}, std::pair{"floor", Op::FFloor}, std::pair{"ceil", Op::FCeil},
                            std::pair{"trunc", Op::FTruncI}, std::pair{"round", Op::FRoundA},
                            std::pair{"roundeven", Op::FRoundE}, std::pair{"rint", Op::FRoundE},
                            std::pair{"nearbyint", Op::FRoundE}, std::pair{"fmin", Op::FMinNum},
                            std::pair{"fmax", Op::FMaxNum}, std::pair{"fmod", Op::FRem}, std::pair{"fma", Op::FFma},
                            std::pair{"fminimum", Op::FMinimum}, std::pair{"fmaximum", Op::FMaximum}}) {
            t[n] = o;
            t[std::string(n) + "f"] = o;
        }
        return t;
    }();
    return k;
}

// llvm.<name>.fNN -> operator
const std::map<std::string, Op>& intrinsics() {
    static const std::map<std::string, Op> k{
        {"sqrt", Op::FSqrt},       {"fma", Op::FFma},         {"fmuladd", Op::FMulAdd},  {"minnum", Op::FMinNum},
        {"maxnum", Op::FMaxNum},   {"minimum", Op::FMinimum}, {"maximum", Op::FMaximum}, {"floor", Op::FFloor},
        {"ceil", Op::FCeil},       {"trunc", Op::FTruncI},    {"round", Op::FRoundA},    {"roundeven", Op::FRoundE},
        {"rint", Op::FRoundE},     {"nearbyint", Op::FRoundE},
    };
    return k;
}

}  // namespace

unsigned FpTr::width(const ir::Type& t, const std::string& what) const {
    if (auto w = fp::width_of(t)) return *w;
    throw Unenc{"UNENCODED: " + what + " on " + t.text +
                (t.kind == ir::Type::Float ? " (floating-point format not modelled)" : "")};
}

void FpTr::set(int b, int dst, Op op, std::vector<Arg> args) {
    if (dst < 0) return;
    Stmt s;
    s.kind = Stmt::Assign;
    s.dst = dst;
    s.op = op;
    s.args = std::move(args);
    t_.push(b, std::move(s));
}

Arg FpTr::fop(int b, Op op, unsigned w, std::vector<Arg> args) {
    int v = t_.newvar("_f" + std::to_string(t_.fn().vars.size()), w);
    t_.fn().vars[static_cast<std::size_t>(v)].fp = true;
    set(b, v, op, std::move(args));
    return Arg::v(v, w);
}

// --fp-checks (opt-in, docs/PIR.md "Floating point"): IEEE exceptions that
// are defined behaviour under Annex F but usually a defect.
void FpTr::checks(int b, Op op, unsigned w, const std::vector<Arg>& args, Arg r, int line) {
    if (!t_.options().fp_checks) return;
    std::optional<Arg> any_nan, all_finite;
    for (auto& a : args) {
        if (!a.is_const && !t_.fn().vars[static_cast<std::size_t>(a.var)].fp) continue;
        if (a.width != w) continue;
        Arg n = p(b, Op::FIsNaN, {a});
        Arg fin = p(b, Op::Xor, {p(b, Op::Or, {n, p(b, Op::FIsInf, {a})}), Arg::c(1, 1)});
        any_nan = any_nan ? p(b, Op::Or, {*any_nan, n}) : n;
        all_finite = all_finite ? p(b, Op::And, {*all_finite, fin}) : fin;
    }
    if (!any_nan) return;
    std::optional<Arg> divz;
    if (op == Op::FDiv) {
        divz = p(b, Op::FIsZero, {args[1]});
        t_.check(b, p(b, Op::And, {*divz, p(b, Op::Xor, {p(b, Op::FIsNaN, {args[0]}), Arg::c(1, 1)})}), "fp-div0",
                 "FLOAT-DIV-ZERO", "floating-point division by zero (result is infinite or NaN)", line);
    }
    t_.check(b, p(b, Op::And, {p(b, Op::FIsNaN, {r}), p(b, Op::Xor, {*any_nan, Arg::c(1, 1)})}), "fp-invalid",
             "FLOAT-INVALID", "floating-point invalid operation (NaN from non-NaN operands)", line);
    Arg ovf = p(b, Op::And, {p(b, Op::FIsInf, {r}), *all_finite});
    if (divz) ovf = p(b, Op::And, {ovf, p(b, Op::Xor, {*divz, Arg::c(1, 1)})});
    t_.check(b, ovf, "fp-overflow", "FLOAT-OVERFLOW", "floating-point overflow (finite operands, infinite result)",
             line);
}

bool FpTr::inst(int& cur, const ir::Inst& in, Ctx& c) {
    const auto& op = in.op;
    static const std::map<std::string, Op> bin{
        {"fadd", Op::FAdd}, {"fsub", Op::FSub}, {"fmul", Op::FMul}, {"fdiv", Op::FDiv}, {"frem", Op::FRem}};
    auto no_flags = [&] {
        // fast-math flags change the value semantics (nnan/ninf make results
        // poison, reassoc/arcp/contract/afn allow other results): not modelled
        for (auto& f : in.flags)
            if (f != "nneg") throw Unenc{"UNENCODED: " + op + " " + f + " (fast-math flags)"};
    };
    if (auto it = bin.find(op); it != bin.end()) {
        no_flags();
        unsigned w = width(in.ty, op);
        Arg a = c.op(0), b = c.op(1);
        a.width = b.width = w;
        int dst = c.dst();
        Arg r = dst >= 0 ? Arg::v(dst, w) : fop(cur, it->second, w, {a, b});
        if (dst >= 0) set(cur, dst, it->second, {a, b});
        checks(cur, it->second, w, {a, b}, r, c.line);
        return true;
    }
    if (op == "fneg") {
        no_flags();
        unsigned w = width(in.ty, op);
        Arg a = c.op(0);
        a.width = w;
        set(cur, c.dst(), Op::Xor, {a, Arg::c(w, sign_bit(w))});
        return true;
    }
    if (op == "fcmp") {
        no_flags();
        unsigned w = width(in.ty, op);
        Arg a = c.op(0), b = c.op(1);
        a.width = b.width = w;
        int dst = c.dst();
        const auto& pr = in.pred;
        auto uno = [&] { return p(cur, Op::FUno, {a, b}); };
        auto por = [&](Arg x, Arg y) { return p(cur, Op::Or, {x, y}); };
        auto pnot = [&](Arg x) { return p(cur, Op::Xor, {x, Arg::c(1, 1)}); };
        Arg r;
        if (pr == "false") r = Arg::c(1, 0);
        else if (pr == "true") r = Arg::c(1, 1);
        else if (pr == "oeq") r = p(cur, Op::FOeq, {a, b});
        else if (pr == "ogt") r = p(cur, Op::FOlt, {b, a});
        else if (pr == "oge") r = p(cur, Op::FOle, {b, a});
        else if (pr == "olt") r = p(cur, Op::FOlt, {a, b});
        else if (pr == "ole") r = p(cur, Op::FOle, {a, b});
        else if (pr == "one") r = por(p(cur, Op::FOlt, {a, b}), p(cur, Op::FOlt, {b, a}));
        else if (pr == "ord") r = pnot(uno());
        else if (pr == "ueq") r = por(uno(), p(cur, Op::FOeq, {a, b}));
        else if (pr == "ugt") r = por(uno(), p(cur, Op::FOlt, {b, a}));
        else if (pr == "uge") r = por(uno(), p(cur, Op::FOle, {b, a}));
        else if (pr == "ult") r = por(uno(), p(cur, Op::FOlt, {a, b}));
        else if (pr == "ule") r = por(uno(), p(cur, Op::FOle, {a, b}));
        else if (pr == "une") r = pnot(p(cur, Op::FOeq, {a, b}));
        else if (pr == "uno") r = uno();
        else throw Unenc{"UNENCODED: fcmp " + pr};
        set(cur, dst, Op::Copy, {r});
        return true;
    }
    if (op == "fptosi" || op == "fptoui") {
        if (in.ops.empty()) return false;
        unsigned fw = width(in.ops[0].ty, op);
        if (in.ty.kind != ir::Type::Int || in.ty.bits == 0 || in.ty.bits > 64)
            throw Unenc{"UNENCODED: " + op + " to " + in.ty.text};
        Arg a = c.op(0);
        a.width = fw;
        bool s = op == "fptosi";
        t_.check(cur, p(cur, s ? Op::FToSIOvf : Op::FToUIOvf, {a, Arg::c(8, in.ty.bits)}), "fp-cast", "FLOAT-CAST-OVF",
                 std::string("floating-point value out of the range of the ") + (s ? "signed" : "unsigned") +
                     " integer type i" + std::to_string(in.ty.bits) + " (C11 6.3.1.4: undefined behaviour)",
                 c.line);
        set(cur, c.dst(), s ? Op::FToSI : Op::FToUI, {a});
        return true;
    }
    if (op == "sitofp" || op == "uitofp") {
        if (in.ops.empty() || in.ops[0].ty.kind != ir::Type::Int || in.ops[0].ty.bits == 0 || in.ops[0].ty.bits > 64)
            throw Unenc{"UNENCODED: " + op + " from " + (in.ops.empty() ? std::string("?") : in.ops[0].ty.text)};
        (void)width(in.ty, op);  // the result format must be modelled
        Arg a = c.op(0);
        a.width = in.ops[0].ty.bits;
        if (op == "uitofp" && std::find(in.flags.begin(), in.flags.end(), "nneg") != in.flags.end())
            t_.check(cur, p(cur, Op::Slt, {a, Arg::c(a.width, 0)}), "nneg", "UB-POISON",
                     "uitofp nneg of a negative value (poison)", c.line);
        for (auto& f : in.flags)
            if (f != "nneg") throw Unenc{"UNENCODED: " + op + " " + f};
        set(cur, c.dst(), op == "sitofp" ? Op::SIToF : Op::UIToF, {a});
        return true;
    }
    if (op == "fpext" || op == "fptrunc") {
        no_flags();
        if (in.ops.empty()) return false;
        unsigned fw = width(in.ops[0].ty, op), w = width(in.ty, op);
        Arg a = c.op(0);
        a.width = fw;
        int dst = c.dst();
        Arg r = dst >= 0 ? Arg::v(dst, w) : fop(cur, Op::FConv, w, {a});
        if (dst >= 0) set(cur, dst, Op::FConv, {a});
        if (op == "fptrunc" && t_.options().fp_checks) {
            // Narrowing cannot make a NaN from a non-NaN or divide, so the only
            // IEEE exception is overflow: a finite value rounds to infinity in
            // the narrower format. (checks() compares operand widths with the
            // result's, which a narrowing never matches, so it is not used.)
            Arg fin = p(cur, Op::Xor, {p(cur, Op::Or, {p(cur, Op::FIsNaN, {a}), p(cur, Op::FIsInf, {a})}),
                                       Arg::c(1, 1)});
            t_.check(cur, p(cur, Op::And, {p(cur, Op::FIsInf, {r}), fin}), "fp-overflow", "FLOAT-OVERFLOW",
                     "floating-point overflow (finite value rounds to infinity in the narrower type)", c.line);
        }
        return true;
    }
    if (op == "bitcast" && !in.ops.empty()) {
        auto fw = fp::width_of(in.ops[0].ty), tw = fp::width_of(in.ty);
        if (!fw && !tw) return false;
        auto bits = [](const ir::Type& t, std::optional<unsigned> f) -> unsigned {
            if (f) return *f;
            return t.kind == ir::Type::Int ? t.bits : 0;
        };
        unsigned a_w = bits(in.ops[0].ty, fw), r_w = bits(in.ty, tw);
        if (a_w == 0 || a_w != r_w)
            throw Unenc{"UNENCODED: bitcast " + in.ops[0].ty.text + " to " + in.ty.text};
        Arg a = c.op(0);
        a.width = a_w;
        set(cur, c.dst(), Op::Copy, {a});
        return true;
    }
    return false;
}

bool FpTr::call(int& cur, const ir::Inst& in, Ctx& c) {
    const auto& n = in.callee;
    if (n.empty()) return false;
    Op o = Op::Copy;
    enum { None, Exact, Fabs, Copysign, Libm } kind = None;
    if (starts(n, "llvm.")) {
        auto rest = n.substr(5);
        auto dot = rest.find('.');
        if (dot == std::string::npos) return false;
        auto base = rest.substr(0, dot);
        if (base == "fabs") kind = Fabs;
        else if (base == "copysign") kind = Copysign;
        else if (auto it = intrinsics().find(base); it != intrinsics().end()) {
            kind = Exact;
            o = it->second;
        }
        if (kind == None) return false;
    } else {
        if (t_.module().find(n)) return false;  // defined in the unit (or a model): inlined as usual
        if (n == "fabs" || n == "fabsf") kind = Fabs;
        else if (n == "copysign" || n == "copysignf") kind = Copysign;
        else if (auto it = libm_exact().find(n); it != libm_exact().end()) {
            kind = Exact;
            o = it->second;
        } else if (fp::is_libm_value(n)) {
            kind = Libm;
            o = Op::FLibm;
        }
        if (kind == None) return false;
        // the libm symbol must have the C signature: all-FP arguments of the result type
        auto rw = fp::width_of(in.ty);
        if (!rw) return false;  // long double variants etc.: generic "UNENCODED: call @f"
        for (auto& a : in.ops)
            if (fp::width_of(a.ty) != rw) return false;
    }
    unsigned w = width(in.ty, "call @" + n);
    std::vector<Arg> args;
    for (std::size_t i = 0; i < in.ops.size(); ++i) {
        if (fp::width_of(in.ops[i].ty) != w) throw Unenc{"UNENCODED: call @" + n + " (argument types)"};
        Arg a = c.op(i);
        a.width = w;
        args.push_back(a);
    }
    auto need = [&](std::size_t k) {
        if (args.size() != k) throw Unenc{"UNENCODED: call @" + n + " arity"};
    };
    int dst = c.dst();
    switch (kind) {
        case Fabs:
            need(1);
            set(cur, dst, Op::And, {args[0], Arg::c(w, wmask(w) & ~sign_bit(w))});
            return true;
        case Copysign: {
            need(2);
            Arg mag = t_.assign(cur, Op::And, w, {args[0], Arg::c(w, wmask(w) & ~sign_bit(w))}, "mag");
            Arg sg = t_.assign(cur, Op::And, w, {args[1], Arg::c(w, sign_bit(w))}, "sgn");
            set(cur, dst, Op::Or, {mag, sg});
            return true;
        }
        case Exact: {
            std::size_t ar = (o == Op::FFma || o == Op::FMulAdd) ? 3
                             : (o == Op::FMinNum || o == Op::FMaxNum || o == Op::FMinimum || o == Op::FMaximum ||
                                o == Op::FRem)
                                 ? 2
                                 : 1;
            need(ar);
            if ((o == Op::FFma || o == Op::FMulAdd) && w == 16)
                throw Unenc{"UNENCODED: call @" + n + " (fused multiply-add on half)"};
            Arg r = dst >= 0 ? Arg::v(dst, w) : fop(cur, o, w, args);
            if (dst >= 0) set(cur, dst, o, args);
            if (o == Op::FSqrt || o == Op::FFma || o == Op::FMulAdd || o == Op::FRem) checks(cur, o, w, args, r, c.line);
            return true;
        }
        case Libm: {
            if (w == 16) throw Unenc{"UNENCODED: call @" + n + " on half"};
            Stmt s;
            s.kind = Stmt::Assign;
            s.dst = dst >= 0 ? dst : t_.newvar("_libm" + std::to_string(t_.fn().vars.size()), w);
            s.op = Op::FLibm;
            s.args = args;
            s.msg = n;
            t_.push(cur, std::move(s));
            auto& lm = t_.fn().libm;
            if (std::find(lm.begin(), lm.end(), n) == lm.end()) lm.push_back(n);
            return true;
        }
        default: return false;
    }
}

}  // namespace prism::pir::pirfp
