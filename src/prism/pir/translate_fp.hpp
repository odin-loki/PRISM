#pragma once

// Translation of LLVM floating-point instructions, intrinsics and libm calls
// to PIR FP operators (docs/PIR.md "Floating point"). The core translator
// (translate.cpp) calls FpTr first for every instruction and call; FpTr
// emits PIR through pirmem::TrApi only.
//
//   fadd fsub fmul fdiv frem fneg fcmp            IEEE, round to nearest even
//   fptosi fptoui                                  out of range: FLOAT-CAST-OVF (UB, always on)
//   sitofp uitofp fpext fptrunc bitcast
//   llvm.{fabs,copysign,sqrt,fma,fmuladd,minnum,maxnum,minimum,maximum,
//         floor,ceil,trunc,round,roundeven,rint,nearbyint}
//   libm: the same functions by name; other pure libm functions (sin, exp,
//         pow ...) return an unconstrained value (Op::FLibm)
//   --fp-checks (opt-in): FLOAT-DIV-ZERO, FLOAT-INVALID, FLOAT-OVERFLOW
//
// Formats other than half/float/double, vectors and fast-math flags are
// "UNENCODED: ...".

#include "translate_mem.hpp"

#include <functional>

namespace prism::pir::pirfp {

struct Ctx {
    std::function<Arg(std::size_t)> op;  // operand i (checked use)
    std::function<int()> dst;            // result variable (-1: none); evaluated only when needed
    int line = 0;
};

class FpTr {
public:
    explicit FpTr(pirmem::TrApi& t) : t_(t) {}
    // true: `in` was an FP instruction and has been translated
    bool inst(int& cur, const ir::Inst& in, Ctx& c);
    // true: the call is an FP intrinsic / libm function and has been translated
    bool call(int& cur, const ir::Inst& in, Ctx& c);

private:
    pirmem::TrApi& t_;
    void set(int b, int dst, Op op, std::vector<Arg> args);
    Arg fop(int b, Op op, unsigned w, std::vector<Arg> args);
    unsigned width(const ir::Type& t, const std::string& what) const;
    void checks(int b, Op op, unsigned w, const std::vector<Arg>& args, Arg result, int line);
    Arg p(int b, Op op, std::vector<Arg> args) { return t_.assign(b, op, 1, std::move(args), "c"); }
};

}  // namespace prism::pir::pirfp
