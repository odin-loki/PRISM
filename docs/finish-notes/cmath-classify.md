# cmath classify

Branch `finish/cxx-cmath-classify`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`<cmath>` is still one extensionless file, `src/prism/pir/models/cxx/cmath`.
The include guard is `_GLIBCXX_CMATH`. `#pragma GCC system_header`. Namespace
`std`. `sqrt`, `fabs`, and the floating `abs` overloads are unchanged. A
negative `sqrt` is still `__prism_check` with class `FUNC-CONTRACT` and the
message `sqrt of a negative value`, before `__builtin_trap`, before
`return 0.0`, and before `__builtin_sqrt`. The negative path does not call
a builtin sqrt and does not return a value. `sqrt(0)` is still 0.

This header is not a proof of numeric accuracy. Classification is not a
proof of a numeric identity.

## What this slice adds

`std::isfinite`, `std::isinf`, `std::isnan`, and `std::signbit`, each for
`float`, `double`, and `long double`. They are `constexpr` and return
`bool`.

A floating zero is finite: `isfinite(0.0)`, `isfinite(0.0f)`, and
`isfinite(0.0L)` are true. The same zeros are not NaN and are not infinite.
`signbit` of a negative value is true for `double`, `float`, and
`long double` (`-2.0`, `-2.0f`, `-2.0L`). These report a class. They do not
prove that two expressions are the same number.

Clang 18's `__builtin_isfinite`, `__builtin_isinf`, and `__builtin_isnan`
are type-generic. That compiler has no `__builtin_isfinitef`,
`__builtin_isfinitel`, `__builtin_isinff`, `__builtin_isinfl`,
`__builtin_isnanf`, or `__builtin_isnanl`, so the float and long double
overloads call the same builtin as double. `signbit` uses
`__builtin_signbit`, `__builtin_signbitf`, and `__builtin_signbitl`.

An integer argument is not an overload. `std::isfinite(0)` is ambiguous
among the three floating types, so that call fails to compile and the unit
falls back to libstdc++.

`<math.h>` defines the four names as function-like macros. A later
`<cstdio>` would rewrite `std::isfinite(x)` into a qualified builtin. The
header sets `_MATH_H` and undefs the four macros. It does not include
`<math.h>`, so `sin`, `cos`, `tan`, `exp`, `log`, and `pow` stay
undeclared. If `<math.h>` was included first, the undefs still drop the
macros.

## What stays omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `sin`, `cos`, `tan`
- `exp`, `log`, `pow`
- the rest of `<cmath>`

`std::sin` fails to compile against this file. The header does not
`#include <complex>` and does not `#include <math.h>`.
`src/prism/pir/models/libc/stdlib.cpp` was not edited.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/cmath`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_cmath.cpp`. `tests/cpp/test_qa_conformance.cpp` was not
edited. No `CMakeLists.txt` line was added.

## Tests

`tests/cxx_models/cmath_trace.cpp` includes `<cmath>`. Compiled twice with
clang++ 18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`), once against
libstdc++ and once with `-isystem` on the model directory. Stdout matched:

```
sqrt0=0
sqrt4=2
fabs2=2
fabs25=1
abs25=1
fin0=1
fin0f=1
fin0l=1
nan0=0
nan0f=0
nan0l=0
inf0=0
inf0f=0
inf0l=0
sgn2=1
sgnf=1
sgnl=1
```

`fin0=1` is `isfinite(0.0)`. `nan0=0` is `isnan(0.0)`. `sgn2=1` is
`signbit(-2.0)`. The `f` and `l` lines are the `float` and `long double`
overloads. `argv` `neg` aborted with
`CHECK FUNC-CONTRACT: sqrt of a negative value` and printed no `value=`.

`static_assert(std::isfinite(0.0))`, the `float` and `long double` zeros,
`!std::isnan` of those zeros, and `!std::isinf(0.0)` compiled against the
model. `std::signbit(-1.0)` is not a constant expression on this clang, and
libstdc++'s `__builtin_signbit` is not one either. `std::sin(0.0)` does not
compile against the model.

`tests/cpp/test_cxx_cmath.cpp` reads the header: the negative `sqrt` check
is unchanged, the four classify overloads return the builtins above, and
`sin`, `cos`, `tan`, `exp`, `log`, and `pow` are still absent.

cmake, ninja, `prism_tests`, and pir were not run.
