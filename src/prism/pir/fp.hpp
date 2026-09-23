#pragma once

// IEEE 754 binary16/32/64 helpers for PIR (docs/PIR.md "Floating point").
//
// PIR keeps a floating-point value as the bits of its format (half: i16,
// float: i32, double: i64); the FP operators (Op::FAdd ...) interpret them.
// The encoder uses Z3's floating-point theory (round to nearest even); the
// concrete semantics here (interpreter, translation validation) must agree
// with it bit for bit except for NaN payloads, which LLVM leaves unspecified.

#include "prism/pir.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prism::pir::fp {

// half / float / double -> 16 / 32 / 64; other formats (bfloat, x86_fp80,
// fp128, ppc_fp128) and non-float types -> nullopt.
std::optional<unsigned> width_of(const ir::Type& t);
inline bool is_fp_width(unsigned w) { return w == 16 || w == 32 || w == 64; }
inline unsigned ebits(unsigned w) { return w == 16 ? 5 : w == 32 ? 8 : 11; }
inline unsigned sbits(unsigned w) { return w == 16 ? 11 : w == 32 ? 24 : 53; }  // with the hidden bit

bool is_fp_op(Op op);
bool is_nan(uint64_t bits, unsigned w);

// Bits of the double with bits `dbits` in format w, when exactly
// representable (LLVM prints float constants as doubles); nullopt otherwise.
std::optional<uint64_t> from_double_bits(uint64_t dbits, unsigned w);

// Concrete value of an FP operator (result width w, argument widths aw).
// FLibm: `libm` names the function (host library value). FMulAdd: fused or
// not is the compiler's choice (`fused` picks one).
uint64_t eval(Op op, unsigned w, const std::vector<uint64_t>& a, const std::vector<unsigned>& aw,
              const std::string& libm = {}, bool fused = true);
// The operator's result is not determined by IEEE (llvm.minnum/maxnum of
// +0 and -0, fmuladd fused or not): the interpreter treats it as a havoc.
bool ambiguous(Op op, unsigned w, const std::vector<uint64_t>& a, const std::vector<unsigned>& aw);

// libm functions whose result PRISM leaves unconstrained (pure functions of
// their floating-point arguments; no errno read back by the program).
bool is_libm_value(const std::string& name);

// Human-readable value ("1.5", "-inf", "nan") and the LLVM literal
// (double-precision hex for float/double, 0xH.... for half).
std::string format(uint64_t bits, unsigned w);
std::string ll_const(uint64_t bits, unsigned w);

// value <-> bits
double to_double(uint64_t bits, unsigned w);
uint64_t from_double(double d, unsigned w);  // round to nearest even

}  // namespace prism::pir::fp
