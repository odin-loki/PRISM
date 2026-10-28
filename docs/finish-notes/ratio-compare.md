# ratio comparison slice

Branch `finish/cxx-ratio-compare`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`ratio_equal` and `ratio_less` are on the existing header
`src/prism/pir/models/cxx/ratio`. The header is still a file, not a
directory. The zero-denominator `static_assert(_Den != 0, "denominator
cannot be zero")` and the `INTMAX_MIN` absolute-value `static_assert` are
unchanged. A product that does not fit in `intmax_t` still instantiates
`_Prism_mul_checked<..., false>`, which has no `value` member.

The solver was not run. This header is not a proof of a program that
merely names a ratio.

## What the comparisons do

`ratio_equal` and `ratio_less` cross-multiply the reduced numerator and
denominator: `n1 * d2` against `n2 * d1`. `_Prism_mul_fits` does that
multiply in `uintmax_t` and accepts it only when the signed product fits
in `intmax_t`. `_Prism_cross_product` then forms that same magnitude in
`uintmax_t` and converts it to `intmax_t`.

`ratio_equal::value` is true only when the two cross products are equal.
For a reduced ratio with a positive denominator, that is the same ratio.
`ratio<2,4>` equals `ratio<1,2>`. `ratio<1,-2>` equals `ratio<-1,2>`.
`ratio<0,5>` equals `ratio<0,1>`. `ratio<1,2>` and `ratio<1,3>` are not
equal.

`ratio_less::value` is the same cross product under `<`. `ratio<1,3>` is
less than `ratio<1,2>`. `ratio<-1,2>` is less than `ratio<-1,3>`. Equal
ratios are not ordered by `ratio_less`. A product that still fits, such as
`(INTMAX_MAX / 2) * 2`, compares as that mathematical value.

When either cross product does not fit in `intmax_t`, the comparison calls
`__prism_check(0, "FUNC-CONTRACT", "ratio comparison cross product does not
fit in intmax_t")` and then `__builtin_trap`. That arm has no boolean.
`ratio<INTMAX_MAX, 2>` compared with itself takes the arm, because
`INTMAX_MAX * 2` does not fit. `ratio_less` of `ratio<INTMAX_MAX, 2>` and
`ratio<1, INTMAX_MAX>` takes it too. A constant expression that reads
`value` on either of those is ill-formed: `__prism_check` is not a
constant expression, so the member is not initialized to false.

`extern "C" void __prism_check(int, const char*, const char*);` is
declared and not defined.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `ratio_not_equal`, `ratio_less_equal`, `ratio_greater`, `ratio_greater_equal`
- `atto`, `femto`, `pico`, `tera`, `peta`, `exa`

The header does not `#include <chrono>` or `<compare>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/ratio`. `tests/cpp/test_cxx_ratio.cpp` was
already a test source. `tests/cpp/test_qa_conformance.cpp` already names
`ratio` in the header array. That array was not edited. `CMakeLists.txt`
was not edited.

## Tests

`tests/cpp/test_cxx_ratio.cpp` includes the model by path. The
`static_assert`s cover reduced equality, a negative sign, `0/5` reduced
to `0/1`, and a wide product that still fits. The header text of the
failing comparison arm calls `__prism_check` before `__builtin_trap` and
contains no `return`. The zero-denominator specialization still has no
`num` and no `den`. The overflowing-product specialization still has no
`value` member.

`tests/cxx_models/ratio_trace.cpp` prints the reduced values and the
fitting comparisons `eq`, `ne`, `lt`, `lt_eq`, and `lt_neg`.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic
-Werror`, including a copy of the header compiled without `#pragma GCC
system_header`. The fitting translation unit and the trace match
libstdc++. Trace stdout includes `r12 1/2`, `eq 1`, `ne 0`, `lt 1`,
`lt_eq 0`, and `lt_neg 1`. `ratio<1,0>` still fails `denominator cannot
be zero`. `ratio_multiply` of two `ratio<10^12, 1>` still fails because
the product specialization has no stored value. `ratio<INTMAX_MIN, 1>`
still fails `absolute value is not representable by intmax_t`. Reading
`value` of an overflowing comparison does not compile. cmake,
`prism_tests`, and pir were not run.
