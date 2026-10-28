# complex abs

Branch `finish/cxx-complex-abs`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::abs(const complex<T>&)` is declared on the existing header
`src/prism/pir/models/cxx/complex`. The header is still a file, not a
directory. The include guard is `_GLIBCXX_COMPLEX`. Division by a zero
complex is the same check as before: `__prism_check` with class
`FUNC-CONTRACT` and message `complex division by zero`, then `return *this`.
That path still does not divide and does not substitute a quotient.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run. Nothing here is `PROVED`.

## What abs does

`abs` of `(0, 0)`, including `-0`, is `0`. That path returns before any
check and does not call a sqrt builtin.

For a nonzero value each part is squared in `T`. A product that overflowed,
or a product that is non-finite, does not fit. NaN is not equal to itself.
An infinity is not equal to the difference of itself. Either failed square
is `__prism_check` with class `FUNC-CONTRACT` and message
`complex abs square does not fit`, then `__builtin_trap`. That path has no
return. It does not return 0 and it does not return a finite magnitude.
`(0, 1e300)` is that failure: the mathematical magnitude is `1e300`, and
libstdc++'s scaled `hypot` returns that finite value. This model traps.

Only after that check, the sum of the two squares is passed to
`__builtin_sqrt` (double), `__builtin_sqrtf` (float), or `__builtin_sqrtl`
(long double). `abs(complex(3, 4))` is 5. `abs(complex(-3, 0))` is 3.
`abs(complex(0, -5))` is 5. A pair whose squares fit and whose sum
overflows still reaches the builtin; the result is that builtin, not a
scaled finite stand-in.

`cl` provides those three sqrt intrinsics and does not provide
`__builtin_trap`. Under `_MSC_VER && !__clang__` the name `__builtin_trap`
is an inline `abort`, so the header still parses and the failure path still
does not return. clang, which the pir stage uses, keeps the builtin. The
header does not `#include <cmath>` and does not include the cmath model.

## What stays omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `exp`, `log`, `log10`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`
- `arg`, `norm`, `polar`, `proj`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited, so the header-name
array still lists `complex` once. The conformance yml files for complex
were not edited. Division by `(0, 0)` is still the false function.

## Tests

`tests/cpp/test_cxx_complex.cpp` includes the header by path. `(0, 0)` and
`(-0, -0)` are 0 and do not call `__prism_check`. A fitting magnitude calls
the check with `ok != 0` and class `FUNC-CONTRACT`. The same file reads the
header: the zero return is before the check, the check is before
`__builtin_trap`, the trap is before the sum, and the sum is before
`__builtin_sqrt` / `__builtin_sqrtf` / `__builtin_sqrtl`. The abs body has
four returns, none of them `return 0`. The division check text is unchanged.

`tests/cxx_models/complex_trace.cpp` includes `<complex>`. With no arguments
the lines match libstdc++. `ovf`, `inf`, and `nan` ask for a square that
does not fit. The model prints the check and does not print `mag=`.

Checked with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra -Wpedantic`, `-O1`
and `-O2`). No warnings. Stdout of a run with no arguments, model and
libstdc++, was:

```
ctor 1.5 -2.25
add 2 0
abs0 0
abs34 5
```

`ovf`, `inf`, and `nan` exited with SIGILL (`rc=132`) after
`CHECK FUNC-CONTRACT: complex abs square does not fit` and did not print
`mag=`. libstdc++ `ovf` printed `mag=1.0000000000000001e+300`. Division of
`(1, 2)` by `(0, 0)` and by `0.0` stayed `check ok=0` and the value
`(1, 2)`. Division by `(0, 1)` stayed `(2, -1)`.

`cl` 14.44 (Visual Studio 2026) compiled the same header. `abs(0, 0)` was
0 and `abs(3, 4)` was 5 for `double`, `float`, and `long double`. `ovf`
printed the same `CHECK FUNC-CONTRACT` line and exited `0xC0000409`
without `mag=`.
