# cmath copysign

Branch `finish/cxx-cmath-copysign`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`<cmath>` is still one extensionless file, `src/prism/pir/models/cxx/cmath`.
The include guard is `_GLIBCXX_CMATH`. `#pragma GCC system_header`. Namespace
`std`. `sqrt`, `fabs`, the floating `abs` overloads, `isfinite`, `isinf`,
`isnan`, and `signbit` are unchanged. A negative `sqrt` is still
`__prism_check` with class `FUNC-CONTRACT` and the message
`sqrt of a negative value`, before `__builtin_trap`, before `return 0.0`,
and before `__builtin_sqrt`.

This header is not a proof of numeric accuracy. Classification is not a
proof of a numeric identity. `copysign`, `fmin`, and `fmax` are not PROVED
and not CLEAN. They are a model of the sign bit and of which argument is
chosen. They are not a proof, and they are not a fuzzer `CLEAN` result.

## What this slice adds

`std::copysign`, `std::fmin`, and `std::fmax`, each for `float`, `double`,
and `long double`. They are `constexpr`. An integer argument is not an
overload: `std::fmin(1, 2)` is ambiguous among the three floating types, so
that call fails to compile and the unit falls back to libstdc++.

`copysign` copies the sign bit of the second argument onto the magnitude of
the first. The magnitude is `fabs` of the first argument, so the value bits
stay that magnitude. A set sign bit negates that magnitude. `copysign(0,
negative)` is a negative zero for `double`, `float`, and `long double`, not
a positive stand-in. `copysign` of a negative zero and a positive sign is a
positive zero. `copysign(-2.5, 1)` is `2.5`. `copysign(2.5, -1)` is `-2.5`.
`fabs` of `copysign(-2.5, -1)` is `2.5`.

`fmin` returns the smaller argument and `fmax` returns the larger. The
comparison is `<` for `fmin` and `>` for `fmax`. If one argument is NaN,
`isnan` (already declared in this file) selects the other argument. If both
are NaN, that other argument is a NaN, so the result is a NaN. The bodies
do not return `0` and do not contain a `0.0` stand-in for NaN.

`<math.h>` can define `copysign`, `fmin`, and `fmax` as macros. The header
sets `_MATH_H` and undefs those three names along with `isfinite`, `isinf`,
`isnan`, and `signbit`. It does not include `<math.h>` and it does not
include `<cmath>`.

## What stays omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `sin`, `cos`, `tan`
- `exp`, `log`, `pow`
- integer overloads of `copysign`, `fmin`, and `fmax`
- the rest of `<cmath>`

`std::sin(0.0)` fails to compile against this file (`no member named 'sin'`).
The header does not `#include <complex>` and does not `#include <math.h>`.
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
cp25=1
cpn=1
cp0=1
cp0p=1
cpmag=1
cp0f=1
cpnf=1
cp0l=1
cpnl=1
fmin13=1
fmax13=1
fminn=1
fminnan=1
fminnan2=1
fmaxnan=1
fmaxnan2=1
fminnn=1
fmaxnn=1
fminf=1
fmaxf=1
fminnf=1
fmaxnnf=1
fminl=1
fmaxl=1
fminnl=1
fmaxnnl=1
```

`cp0=1` is `copysign(0.0, -1.0)` with the sign bit set. `cp0f` and `cp0l`
are the `float` and `long double` overloads. `cp0p=1` is `copysign` of a
negative zero and a positive sign, and the sign bit is clear. `cpmag=1` is
`fabs(copysign(-2.5, -1.0)) == 2.5`. `fminnan=1` is `fmin(NaN, 3.0) == 3`.
`fminnan2=1` is `fmin(3.0, NaN) == 3`. `fminnn=1` is `isnan(fmin(NaN, NaN))`.
`fmaxnn=1` is the same for `fmax`. The `f` and `l` lines are the `float` and
`long double` overloads. `argv` `neg` aborted with
`CHECK FUNC-CONTRACT: sqrt of a negative value` and printed no `value=`.

`tests/cpp/test_cxx_cmath.cpp` reads the header. The negative `sqrt` check
and the `fabs` body are unchanged. The file comment says `copysign`, `fmin`,
and `fmax` are not PROVED and not CLEAN, and those two words appear once
each. Each `copysign` body is `fabs` then `signbit`, and it does not return
`0`. Each `fmin` and `fmax` body returns the other argument when `isnan`
says so, compares with `<` or `>`, and does not return `0`. `sin`, `cos`,
`tan`, `exp`, `log`, and `pow` are still absent.

cmake, ninja, `prism_tests`, and pir were not run.
