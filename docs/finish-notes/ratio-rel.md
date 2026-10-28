# ratio relations

Branch `finish/cxx-ratio-rel`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`ratio_not_equal`, `ratio_greater`, `ratio_less_equal`, and
`ratio_greater_equal` are on the existing header
`src/prism/pir/models/cxx/ratio`. The header is still a file, not a
directory. `ratio_equal` and `ratio_less` are unchanged. The
zero-denominator `static_assert(_Den != 0, "denominator cannot be zero")`
and the `INTMAX_MIN` absolute-value `static_assert` are unchanged. A product
that does not fit in `intmax_t` still instantiates
`_Prism_mul_checked<..., false>`, which has no `value` member.

The solver was not run. This header is not a proof of a program that
merely names a ratio.

## What the relations do

All four call `_Prism_ratio_compare`. That function still cross-multiplies
the reduced numerator and denominator, `n1 * d2` against `n2 * d1`, and
only after `_Prism_mul_fits` accepts both products. When either product
does not fit in `intmax_t`, the comparison calls
`__prism_check(0, "FUNC-CONTRACT", "ratio comparison cross product does not
fit in intmax_t")` and then `__builtin_trap`. That arm has no `return`, no
`true`, and no `false`.

The opposite results are the other arm, after both products have been
formed as `intmax_t`:

- `ratio_not_equal` is `_S_not_equal`, `!(_Left == _Right)`. That is the
  opposite of equal, and only after the products fit. `ratio<1,2>` is
  unequal to `ratio<1,3>`. `ratio<2,4>` is not unequal to `ratio<1,2>`.
  `ratio<1,-2>` is not unequal to `ratio<-1,2>`.
- `ratio_greater` is `_S_less` with the two ratios swapped. `ratio<1,2>` is
  greater than `ratio<1,3>`. `ratio<-1,3>` is greater than `ratio<-1,2>`.
  Equal ratios are not greater.
- `ratio_less_equal` is not greater. Greater is less with the ratios
  swapped, so this is `_S_not_less` on that swapped pair:
  `!(_Left < _Right)`. `ratio<1,3>` is `<=` `ratio<1,2>`. Equal ratios are
  `<=`. `ratio<1,2>` is not `<=` `ratio<1,3>`.
- `ratio_greater_equal` is not less: `_S_not_less` on the original pair.
  `ratio<1,2>` is `>=` `ratio<1,3>`. Equal ratios are `>=`. `ratio<1,3>` is
  not `>=` `ratio<1,2>`.

A product that still fits, such as `(INTMAX_MAX / 2) * 2`, compares as that
mathematical value. `ratio<INTMAX_MAX, 2>` compared with itself does not
compile, and neither does `ratio<INTMAX_MAX, 1>` compared with `ratio<1, 2>`,
for every one of these relations. Reading `value` is not a constant
expression. The member is not initialized to true and it is not initialized
to false.

`extern "C" void __prism_check(int, const char*, const char*);` is
declared and not defined.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

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
`static_assert`s cover unequal ratios, the same reduced ratio, a negative
sign, `0/5`, greater with the ratios swapped, `<=` as not greater, `>=` as
not less, and a wide product that still fits. The header text of
`ratio_equal` and `ratio_less` still calls `_Prism_ratio_compare` without
a negation. `ratio_greater` calls it with the ratios swapped and `_S_less`.
`ratio_less_equal` calls it with that same swap and `_S_not_less`.
`ratio_greater_equal` calls it with `_S_not_less`. The failing comparison
arm still calls `__prism_check` before `__builtin_trap` and contains no
`return`. The negations `return !(_Left == _Right)` and
`return !(_Left < _Right)` are after both products are formed. The
zero-denominator specialization still has no `num` and no `den`. The
overflowing-product specialization still has no `value` member.

`tests/cxx_models/ratio_trace.cpp` prints the reduced values, the previous
fitting comparisons, and `neq`, `neq_same`, `gt`, `gt_eq`, `gt_neg`, `le`,
`le_eq`, `ge`, and `ge_lt`.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic
-Werror`, including a copy of the header compiled without `#pragma GCC
system_header`. The fitting translation unit compiled. The trace, built
with `-isystem` on the model directory and again against libstdc++, matched.
Trace stdout includes `r12 1/2`, `eq 1`, `ne 0`, `lt 1`, `lt_eq 0`,
`lt_neg 1`, `neq 1`, `neq_same 0`, `gt 1`, `gt_eq 0`, `gt_neg 1`, `le 1`,
`le_eq 1`, `ge 1`, and `ge_lt 0`. `tests/cpp/test_cxx_ratio.cpp` compiled,
and the doctest run passed (2 cases, 74 assertions) with
`DOCTEST_CONFIG_NO_MULTITHREADING` so the doctest implementation does not
include `<chrono>` through `<mutex>`. `ratio<1,0>` still fails
`denominator cannot be zero`. `ratio<INTMAX_MIN, 1>` still fails
`absolute value is not representable by intmax_t`. Reading `value` of an
overflowing `ratio_equal`, `ratio_less`, `ratio_not_equal`,
`ratio_greater`, `ratio_less_equal`, or `ratio_greater_equal` does not
compile. cmake, `prism_tests`, and pir were not run.
