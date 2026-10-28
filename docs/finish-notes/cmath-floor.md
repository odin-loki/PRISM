# cmath floor

Branch `finish/cxx-cmath-floor`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`<cmath>` is still one extensionless file, `src/prism/pir/models/cxx/cmath`.
The include guard is `_GLIBCXX_CMATH`. `#pragma GCC system_header`. Namespace
`std`. `sqrt`, `fabs`, the floating `abs` overloads, `isfinite`, `isinf`,
`isnan`, `signbit`, `copysign`, `fmin`, and `fmax` are unchanged. A negative
`sqrt` is still `__prism_check` with class `FUNC-CONTRACT` and the message
`sqrt of a negative value`, before `__builtin_trap`, before `return 0.0`,
and before `__builtin_sqrt`.

This header is not a proof of numeric accuracy. `floor`, `ceil`, and `trunc`
are not PROVED and not CLEAN. They name the rounding builtin. They are not
a proof, and they are not a fuzzer `CLEAN` result.

## What this slice adds

`std::floor`, `std::ceil`, and `std::trunc`, each for `float`, `double`,
and `long double`. They are `constexpr`. An integer argument is not an
overload: `std::floor(1)` is ambiguous among the three floating types, so
that call fails to compile and the unit falls back to libstdc++.

`floor` returns `__builtin_floor`, `__builtin_floorf`, or
`__builtin_floorl`. `ceil` returns `__builtin_ceil`, `__builtin_ceilf`, or
`__builtin_ceill`. `trunc` returns `__builtin_trunc`, `__builtin_truncf`,
or `__builtin_truncl`. `floor(1.5)` is `1` and `floor(-1.5)` is `-2`.
`ceil(1.5)` is `2` and `ceil(-1.5)` is `-1`. `trunc(1.5)` is `1` and
`trunc(-1.5)` is `-1`. `floor(2)`, `ceil(2)`, and `trunc(2)` stay `2`.
`floor(-2)`, `ceil(-2)`, and `trunc(-2)` stay `-2`.

A NaN stays a NaN. The body does not return `0` in place of a NaN. An
infinity stays that infinity, sign included. `floor` of a negative zero is
a negative zero, and `floor` of a positive zero is a positive zero. `ceil`
and `trunc` of a negative zero are a negative zero. The bodies do not
contain a `0.0` stand-in.

`<math.h>` can define `floor`, `ceil`, and `trunc` as macros. The header
sets `_MATH_H` and undefs those three names next to `isfinite`, `isinf`,
`isnan`, `signbit`, `copysign`, `fmin`, and `fmax`. It does not include
`<math.h>` and it does not include `<cmath>`.

## What stays omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `sin`, `cos`, `tan`
- `exp`, `log`, `pow`
- integer overloads of `floor`, `ceil`, and `trunc`
- the rest of `<cmath>`

`std::sin(0.0)` fails to compile against this file (`no member named 'sin'`).
The header does not `#include <complex>` and does not `#include <math.h>`.
`src/prism/pir/models/libc/stdlib.cpp` was not edited.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/cmath`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_cmath.cpp`. `tests/cxx_models/cmath_trace.cpp` is not a
`_prism_test_sources` line. `tests/cpp/test_qa_conformance.cpp` was not
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
fl15=1
fln15=1
ce15=1
cen15=1
tr15=1
trn15=1
fl2=1
fln2=1
ce2=1
cen2=1
tr2=1
trn2=1
flnan=1
cenan=1
trnan=1
flpinf=1
flninf=1
cepinf=1
ceninf=1
trpinf=1
trninf=1
flmz=1
cemz=1
trmz=1
flpz=1
fl15f=1
fln15f=1
ce15f=1
cen15f=1
tr15f=1
trn15f=1
flnanf=1
cenanf=1
trnanf=1
flpinff=1
flninff=1
cepinff=1
ceninff=1
trpinff=1
trninff=1
flmzf=1
cemzf=1
trmzf=1
fl15l=1
fln15l=1
ce15l=1
cen15l=1
tr15l=1
trn15l=1
flnanl=1
cenanl=1
trnanl=1
flpinfl=1
flninfl=1
cepinfl=1
ceninfl=1
trpinfl=1
trninfl=1
flmzl=1
cemzl=1
trmzl=1
```

`fl15=1` is `floor(1.5) == 1`. `fln15=1` is `floor(-1.5) == -2`. `ce15=1`
is `ceil(1.5) == 2`. `cen15=1` is `ceil(-1.5) == -1`. `tr15=1` is
`trunc(1.5) == 1`. `trn15=1` is `trunc(-1.5) == -1`. `fl2=1`, `ce2=1`, and
`tr2=1` are those functions on `2`. `fln2=1`, `cen2=1`, and `trn2=1` are
those functions on `-2`. `flnan=1` is `isnan(floor(NaN))`. `cenan=1` and
`trnan=1` are the same for `ceil` and `trunc`. `flpinf=1` is
`floor(+infinity) == +infinity`. `flninf=1` is `floor(-infinity) ==
-infinity`. The `ce` and `tr` infinity lines are `ceil` and `trunc`.
`flmz=1` is `floor` of a negative zero with the sign bit set. `cemz=1` and
`trmz=1` are `ceil` and `trunc` of that zero. `flpz=1` is `floor` of a
positive zero with the sign bit clear. The `f` and `l` lines are the
`float` and `long double` overloads. `argv` `neg` aborted with
`CHECK FUNC-CONTRACT: sqrt of a negative value` and printed no `value=`.

`tests/cpp/test_cxx_cmath.cpp` reads the header. The negative `sqrt` check
and the `fabs`, `copysign`, `fmin`, and `fmax` bodies are unchanged. The
header says `floor`, `ceil`, and `trunc` are not PROVED and not CLEAN.
`PROVED` and `CLEAN` appear twice each: the copysign sentence, and this
one. Each new body is the matching builtin and does not return `0`. `sin`,
`cos`, `tan`, `exp`, `log`, and `pow` are still absent.

cmake, ninja, `prism_tests`, and pir were not run.
