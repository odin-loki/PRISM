# cmath slice

Branch `finish/cxx-cmath`, base `3e0ebbb46361d4d3c7557657eeeb590d2ae3aa19`.

`<cmath>` is one extensionless file, `src/prism/pir/models/cxx/cmath`. A
directory of that name would make `#include <cmath>` fail with EISDIR and
every such unit would fall back to libstdc++. The include guard is
`_GLIBCXX_CMATH`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml files record the contract each harness asks
for. `expected` is a YAML boolean. This header is not a proof of numeric
accuracy.

## What the model covers

`sqrt` of a negative value is a precondition failure: `__prism_check` with
class `FUNC-CONTRACT` and the message `sqrt of a negative value` (the same
intrinsic the libc models use, literal class and message). The check is the
first thing in the function, before any builtin and before any returned
value. The negative path then calls `__builtin_trap` and does not call a
library `sqrt`. It does not return a quiet NaN and it does not return an
invented non-negative number. If that path is reached, the trap is also
`CXX-UNREACHABLE`.

`sqrt` of zero returns zero, with no builtin. `sqrt` of a positive value is
the real sqrt: `__builtin_sqrt` / `__builtin_sqrtf` / `__builtin_sqrtl`.
PIR lowers `llvm.sqrt` and `@sqrt` to `Op::FSqrt` for float and double.
`long double` is not one of those widths.

`fabs` and the floating `abs` overloads are `constexpr` wrappers around
`__builtin_fabs` / `fabsf` / `fabsl`. That builtin clears the sign and keeps
the value bits, so a finite value's magnitude is the result. `fabs(-2)` is
2. `fabs(-2.5)` is 2.5. libstdc++ `<bits/std_abs.h>` defines the same
floating `abs` overloads unless `__CORRECT_ISO_CPP_MATH_H_PROTO` is set.
This header sets that macro, which is the guard `std_abs.h` already uses,
so those overloads are the ones here and not a second definition.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). A partial header is intentional.
They are not stubbed:

- `sin`, `cos`, `tan`
- `exp`, `log`, `pow`
- the rest of `<cmath>`

`std::sin` fails to compile against this file and compiles against
libstdc++. The header does not `#include <complex>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `PRISM_PIR_MODELS`
lists `src/prism/pir/models/cxx/cmath`, so the embedded name is `cxx/cmath`.
`_prism_test_sources` lists `tests/cpp/test_cxx_cmath.cpp`.
`tests/cpp/test_qa_conformance.cpp` names `cmath` in the header array. The
skips for `iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
`src/prism/pir/models/libc/stdlib.cpp` was not edited.

## Tests

`tests/cxx_models/cmath_trace.cpp` includes `<cmath>`. Compiled twice
(`-isystem` the model directory, and without it) with clang++ 18.1.3 and
libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. Stdout matched:

```
sqrt0=0
sqrt4=2
fabs2=2
fabs25=1
abs25=1
```

`fabs2=2` is `fabs(-2) == 2`. `fabs25=1` is `fabs(-2.5) == 2.5`. `sqrt0=0`
is `sqrt(0) == 0`. `argv` `neg` calls `sqrt` on `-1`. The model binary
aborts with `CHECK FUNC-CONTRACT: sqrt of a negative value` and does not
print `value=`.

`tests/cpp/test_cxx_cmath.cpp` reads the header: in `sqrt(double)` the
`__prism_check` is before `__builtin_trap`, before `return 0.0`, and before
`__builtin_sqrt`. The negative branch does not call `__builtin_sqrt` and
does not return. With clang++ it includes the model the way
`tests/cpp/test_cxx_list.cpp` includes `<list>`. Without a usable clang++
that half is `NOTRUN`.

`testdata/conformance/prism/cxx/cxx_cmath_true.cpp` calls `sqrt(0)` and
`fabs(-2)` and checks both results (`no-oob`, `expected: true`).
`cxx_cmath_false.cpp` calls `sqrt` on a negative (`expected: false`,
witness `[-1]`). The yml shape is `cxx_array_*.yml`.
`testdata/conformance/libc-models/cmath_contracts.cpp` is the
`#include <cmath>` the header case searches for. It is not a scored task.

cmake, ninja, `prism_tests`, and pir were not run.
