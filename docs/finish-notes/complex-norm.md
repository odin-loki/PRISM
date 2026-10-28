# complex norm

Branch `finish/cxx-complex-norm`, base `8a27a9cd8d34383b27462b27c3f7ea9271bef70a`.

`std::norm(const complex<T>&)` is declared on the existing header
`src/prism/pir/models/cxx/complex`. The header is still a file, not a
directory. The include guard is `_GLIBCXX_COMPLEX`. `abs` is unchanged.
Division by a zero complex is the same check as before: `__prism_check`
with class `FUNC-CONTRACT` and message `complex division by zero`, then
`return *this`. That path still does not divide and does not substitute a
quotient.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run. Nothing here is `PROVED`.

## What norm does

`norm` of `(0, 0)`, including `-0`, is `0`. That path returns before any
check and before either part is squared.

For a nonzero value each part is squared in `T`. A product that overflowed,
or a product that is non-finite, does not fit. NaN is not equal to itself.
An infinity is not equal to the difference of itself. Either failed square
is `__prism_check` with class `FUNC-CONTRACT` and message
`complex norm square does not fit`, then `__builtin_trap`. That path has no
return. It does not return 0 and it does not return a finite number.
`(1e300, 0)` is that failure. libstdc++'s primary `norm` returns an
infinity there. This model traps. The same check covers a non-finite part.

Only after that check, the return is the sum of the two squares.
`norm(complex(3, 4))` is 25. `norm(complex(-3, 0))` is 9.
`norm(complex(0, -5))` is 25. A pair whose squares fit and whose sum
overflows still returns that sum. There is no scaled magnitude and no sqrt
builtin on this path.

The header does not `#include <cmath>` and does not include the cmath model.
`__builtin_trap` is the same stand-in `abs` already uses under
`_MSC_VER && !__clang__`. That block was not edited.

## What stays omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `exp`, `log`, `log10`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`
- `arg`, `polar`, `proj`

`abs` stays declared.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited, so the header-name
array still lists `complex` once. The conformance yml files for complex
were not edited. Division by `(0, 0)` is still the false function.

## Tests

`tests/cpp/test_cxx_complex.cpp` includes the header by path. The case that
required `norm` to be absent now requires `arg`, `polar`, `proj`, and the
hyperbolic functions to stay absent, and requires the omitted-list sentence
to name those without `norm`. `(0, 0)` and `(-0, -0)` are 0 and do not call
`__prism_check`. A fitting value calls the check with `ok != 0`, class
`FUNC-CONTRACT`, and message `complex norm square does not fit`, and the
value is the sum of the squares. The same file reads the header: the zero
return is before the check, the check is before `__builtin_trap`, and the
trap is before `return __rr + __ii`. The norm body has two returns, neither
of them `return 0` or a single square, and it does not call a sqrt builtin.
The abs body and the division check text are unchanged.

`tests/cxx_models/complex_trace.cpp` includes `<complex>`. With no arguments
the lines match libstdc++. `ovf`, `inf`, and `nan` still ask `abs` for a
square that does not fit. `novf`, `ninf`, and `nnan` ask `norm` for the
same inputs. The model prints the check and does not print `nrm=`.

Checked with clang++ 18.1.3 and libstdc++ 14 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, `-O1` and `-O2`). No warnings. The doctest translation
unit compiled with `-fsyntax-only`. `prism_tests` was not run, so those
`CHECK`s were not executed. A by-path include of the header ran the same
calls. Stdout of a run with no arguments, model and libstdc++, was:

```
ctor 1.5 -2.25
add 2 0
abs0 0
abs34 5
norm0 0
norm34 25
```

`novf`, `ninf`, and `nnan` exited with SIGILL (`rc=132`) after
`CHECK FUNC-CONTRACT: complex norm square does not fit` and did not print
`nrm=`. The same holds at `-O2`, and for `float` (`1e30`) and
`long double` (`1e3000`) squares. libstdc++ `novf` and `ninf` printed
`nrm=inf`. libstdc++ `nnan` printed `nrm=-nan`. `abs` `ovf`, `inf`, and
`nan` still exit SIGILL with `complex abs square does not fit` and do not
print `mag=`. libstdc++ `ovf` printed `mag=1.0000000000000001e+300`.

`norm(0, 0)` and `norm(-0, -0)` were 0 with no check, for `double`,
`float`, and `long double`. `norm(3, 4)` was 25 with `ok` 1. `abs(3, 4)`
was still 5. Division of `(1, 2)` by `(0, 0)` and by `0.0` stayed
`check ok=0` and the value `(1, 2)`. Division by `(0, 1)` stayed `(2, -1)`.
