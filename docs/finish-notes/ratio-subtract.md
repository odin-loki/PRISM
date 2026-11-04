# ratio subtract

Branch `finish/cxx-ratio-subtract`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::ratio_subtract` is on the existing header
`src/prism/pir/models/cxx/ratio`. The header is still a file, not a
directory. The previous alias was `ratio_add<_R1, ratio<-_R2::num, _R2::den>>`.
That formed an intermediate ratio and then used `ratio_add`, whose overflow
is the `static_assert` in `_Prism_mul_checked` / `_Prism_add_checked`.
This slice replaces that alias. `ratio_multiply`, `ratio_divide`,
`ratio_add`, `ratio_equal`, `ratio_less`, and the other relation checks
are unchanged. The zero-denominator `static_assert(_Den != 0, "denominator
cannot be zero")` and the `INTMAX_MIN` absolute-value `static_assert` are
unchanged. A product that does not fit in `intmax_t` still instantiates
`_Prism_mul_checked<..., false>`, which has no `value` member. A sum that
does not fit still instantiates `_Prism_add_checked<..., false>`.

The solver was not run. An overflowing difference is not `PROVED` and not
`CLEAN`. This header is not a proof of a program that merely names a ratio.

## What the difference does

`ratio_subtract<R1, R2>` is the reduced `n1/d1 - n2/d2`. It scales the
numerators the same way `ratio_add` does: cancel `gcd(d1, d2)`, then
`n1 * (d2/g) + (-n2) * (d1/g)` over `d1 * (d2/g)`. The fit tests are
`_Prism_mul_fits` and `_Prism_add_fits`. The products are formed with
`_Prism_cross_product` only after those multiplies fit, and the sum is
formed only after `_Prism_add_fits` accepts it. The ratio is
`ratio` of those two results, so a fitting difference is reduced the same
way as any other ratio.

`ratio<1,2> - ratio<1,3>` is `1/6`. `ratio<1,3> - ratio<1,2>` is `-1/6`.
`ratio<1,2> - ratio<1,2>` is `0/1`. That zero is the mathematical
difference. `ratio<1,2> - ratio<-1,3>` is `5/6`. `ratio<2,4> - ratio<1,6>`
reduces to `1/3`. `ratio<INTMAX_MAX, 1> - ratio<1, 1>` is
`INTMAX_MAX - 1`. `ratio<INTMAX_MAX, 1> - ratio<INTMAX_MAX, 1>` is `0/1`.
`ratio<INTMAX_MAX, 2> - ratio<1, 2>` is `ratio<INTMAX_MAX / 2, 1>`.
`ratio<1, INTMAX_MAX> - ratio<1, INTMAX_MAX>` is `0/1`.

A zero denominator is still a `static_assert`, including inside
`_Prism_ratio_difference` before the divisions. It is not stored as `0/1`.

When a product or the sum does not fit in `intmax_t`, the function calls
`__prism_check(0, "FUNC-CONTRACT", "ratio difference does not fit in intmax_t")`
and then `__builtin_trap`. Both failing arms have no `return` and do not
name a ratio. They do not return `ratio<0, 1>`. The ratio specialization
is named only as `ratio<_S_diff.num, _S_diff.den>` after that value has
been computed. Reading `::num` on an overflowing difference is ill-formed:
`__prism_check` is not a constant expression, so the template argument is
not a constant and a wrapped ratio is not formed.

These differences take that path:

- `ratio<INTMAX_MAX, 1> - ratio<-1, 1>` (the sum does not fit)
- `ratio<INTMAX_MAX, 2> - ratio<1, 3>` (a scaled numerator product does not fit)
- `ratio<1, INTMAX_MAX> - ratio<1, INTMAX_MAX - 1>` (the denominator product does not fit)

`extern "C" void __prism_check(int, const char*, const char*);` is
declared and not defined.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `atto`, `femto`, `pico`, `tera`, `peta`, `exa`

The header does not `#include <chrono>` or `<compare>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `src/prism/bmc.cpp` was not
edited. `PRISM_PIR_MODELS` already lists `src/prism/pir/models/cxx/ratio`.
`tests/cpp/test_cxx_ratio.cpp` was already a test source. No
`_prism_test_sources` line was added or removed. `tests/cpp/test_qa_conformance.cpp`
was not edited. `CMakeLists.txt` was not edited.

## Tests

`tests/cpp/test_cxx_ratio.cpp` includes the model by path. The
`static_assert`s cover `1/2 - 1/3`, `1/3 - 1/2`, a real `0/1`, subtraction
of a negative, reduction of `2/4 - 1/6`, and wide differences that still
fit. The header text of `_Prism_ratio_difference` has both zero-denominator
`static_assert`s. Each failing arm calls `__prism_check` before
`__builtin_trap`, with class `FUNC-CONTRACT` and message
`ratio difference does not fit in intmax_t`, and contains no `return` and
no `ratio<`. The sum is returned only in the inner else, after
`_Prism_add_fits`. `ratio_add` and `ratio_multiply` still have no
`__prism_check` and no `__builtin_trap`. `ratio_equal` and `ratio_less`
still have no trap of their own. The old
`ratio_add<_R1, ratio<-_R2::num, _R2::den>>` alias is gone. The header
says the overflowing difference is not `PROVED` and not `CLEAN`. The
zero-denominator specialization still has no `num` and no `den`. The
overflowing-product specialization still has no `value` member.

`tests/cxx_models/ratio_trace.cpp` prints the previous values and
`sub_neg`, `sub_zero`, and `sub_back`. It does not name an overflowing
difference.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic
-Werror`, including a copy of the header compiled without `#pragma GCC
system_header`. Fitting values matched libstdc++ at `-O0` and `-O2`,
including `sub 1/6`, `sub_neg -1/6`, `sub_zero 0/1`, `sub_back 5/6`,
`wide 4611686018427387903/1`, and `fit 9223372036854775806/1`. The trace,
built with `-isystem` on the model directory and again against libstdc++,
matched. `tests/cpp/test_cxx_ratio.cpp` compiled, and the doctest run
passed (3 cases, 130 assertions) with `DOCTEST_CONFIG_NO_MULTITHREADING`.
`ratio<1,0>` still fails `denominator cannot be zero`.
`ratio<INTMAX_MIN, 1>` still fails
`absolute value is not representable by intmax_t`. `ratio_add` of
`ratio<INTMAX_MAX, 1>` and `ratio<1, 1>` still fails
`sum does not fit in intmax_t`. `ratio_multiply` of
`ratio<INTMAX_MAX, 1>` and `ratio<2, 1>` still fails
`product does not fit in intmax_t`. Reading `::num` of each overflowing
`ratio_subtract` above does not compile; the note is
`__prism_check` in the failing arm. A runtime call of
`_Prism_ratio_difference<INTMAX_MAX, 1, -1, 1>`, with a `__prism_check`
that prints and returns, printed
`CHECK FUNC-CONTRACT: ratio difference does not fit in intmax_t` and then
died with SIGILL from `__builtin_trap`. It printed no numerator.
cmake, ninja, `prism_tests`, and pir were not run.
