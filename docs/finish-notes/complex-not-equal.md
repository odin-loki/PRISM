# complex not-equal

Branch `finish/cxx-complex-ne`, base `fa14d5e3bd2277fe7038eb1bf2f390678df962ed`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/complex`. That path is a file. A directory of the
same name would make `#include <complex>` fail with EISDIR. The include
guard is still `_GLIBCXX_COMPLEX`. `#pragma GCC system_header`. Namespace
`std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two complex values for inequality is
not a fuzzer `CLEAN` result. The header says this is not PROVED and not
CLEAN.

## What not-equal does

One function template, the same `T`:

```
constexpr bool operator!=(const complex<_Tp>& __x, const complex<_Tp>& __y)
```

The body is `return !(__x == __y)`. It calls the existing `operator==`.
It does not read `real()` or `imag()`. It does not compare the parts a
second time. It does not return `complex{}`, `false`, or `0`.

`operator==` is still `real() == real() && imag() == imag()`. A NaN part
stays a NaN comparison: NaN is not equal to itself, so `operator==` is
false, and `operator!=` is true. The result is not forced to false or to
0. That holds when the real part is NaN, when the imag part is NaN, and
when both parts are NaN. `-0` still compares equal to `0`, so those two
values compare not-equal as false.

Equal values compare not-equal as false. A different real part, or a
different imag part, compares not-equal as true. The return type is
`bool`.

There is no scalar overload. `operator<=>` is still not declared.

## What stayed

`operator==`, `operator+`, `operator-`, `operator*`, `operator/`, `abs`,
`conj`, `real`, and `imag` are unchanged. Division by a zero complex is
still `__prism_check` with class `FUNC-CONTRACT` and message
`complex division by zero`, then `__builtin_trap`. That arm does not
return a value, does not divide, and does not store into the real or imag
members. A scalar zero still builds `complex(y, 0)` and hits the same arm.
A divisor with one part nonzero still returns the schoolbook quotient.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- `exp`, `log`, `log10`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`
- `arg`, `polar`, `proj`
- `operator<=>`

## Wiring

`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped.
`tests/cpp/test_cxx_complex.cpp` was already one line.
`tests/cpp/test_qa_conformance.cpp` was not edited. `complex` stays in the
header-name array. `testdata/conformance/libc-models/complex_contracts.cpp`
and its yml were not edited. `complex_div0_false` still divides by `(0, 0)`.

## Tests

`tests/cpp/test_cxx_complex.cpp` includes the header by path. The new case
reads `operator!=` as `return !(__x == __y)` and checks that body does not
name `real`, `imag`, or `complex`, does not contain `!=`, and contains one
`==`. `operator==` is still the previous return. The zero arm of
`operator/=` still traps and has no `return`. Constant checks cover an
equal `complex<int>`, a different real part, and a different imag part.
The return type of `!=` is `bool`.

Runtime checks cover an equal `complex<double>`, a different real part, a
different imag part, and `-0` compared with `0`. A NaN real part, a NaN
imag part, and both parts NaN make `operator==` false and `operator!=`
true. The integer value of that `bool` is 1, not 0. A `Part` type counts
calls to its `operator==`. Equal parts call it twice for `==` and twice
for `!=`. A real mismatch calls it once for each. An imag mismatch calls
it twice for each. `operator!=` does not add a second comparison.

`tests/cxx_models/complex_trace.cpp` includes `<complex>`. The previous
lines for construction, addition, `abs`, and `norm` stay. New lines cover
equal values, a different real part, a different imag part, a value
compared with itself, `-0` compared with `0`, and a NaN part compared with
itself.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, at `-O1` and `-O2`. The trace was compiled twice:
once with this header ahead of libstdc++ as `<complex>`, and once against
libstdc++. The dependency dump names this header. Stdout matched. libstdc++
`<complex>` in this mode has no separate `operator!=`; `!=` is the
rewritten negation of its `operator==`. Not-equal lines:

```
ne same=0 re=1 im=1 self=0 neg0=0
nenan re=1 im=1 both=1
```

`same=0` and `self=0` are equal values: not-equal is false. `re=1` is a
different real part. `im=1` is a different imag part. `neg0=0` is
`(-0, -0)` compared with `(0, 0)`: not-equal is false because `operator==`
is true. `nenan` is 1 for a NaN real part, a NaN imag part, and both parts
NaN. That 1 is not forced to 0. Both binaries exited 0.

Division of `(1, 2)` by `(0, 0)` and by `0.0` printed
`CHECK FUNC-CONTRACT: complex division by zero` with `ok=0` and exited
with SIGILL (`rc=132`). Neither printed a quotient. Division by `(0, 1)`
stayed `(2, -1)`. `abs` `ovf`, `inf`, and `nan`, and `norm` `novf`,
`ninf`, and `nnan`, still exit SIGILL and do not print `mag=` or `nrm=`.

The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and run. Six cases passed, including
the new not-equal case. `prism_tests` was not run.
