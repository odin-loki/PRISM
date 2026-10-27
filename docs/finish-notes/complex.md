# complex slice

Branch `finish/cxx-complex`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.

`std::complex` is the extensionless file `src/prism/pir/models/cxx/complex`.
A directory of that name would make `#include <complex>` fail with EISDIR, and
the unit would fall back to libstdc++. The include guard is `_GLIBCXX_COMPLEX`,
so libstdc++'s own header is not also included. The file is a `#pragma GCC
system_header` and the type lives in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. Nothing here is `PROVED`.

## What the model stores

`real` and `imag` are data members. `real()` and `imag()` return those values.
There is no allocation: the object is two values, and the doctest checks
`sizeof(std::complex<double>) == 2 * sizeof(double)`.

Arithmetic is `+`, `-`, `*`, `/`, the compound forms, and `==`. A scalar
operand of a binary operator is `complex(re, 0)` (the same value libstdc++
uses). Those overloads are separate templates: deduction will not convert a
scalar to `complex`, so `z / 0.0` would otherwise miss `operator/(complex, complex)`.
A zero scalar is therefore the same zero-complex check.

## Division by zero

Both parts zero (including `-0`) is a precondition failure:

```c
__prism_check(!zero, "FUNC-CONTRACT", "complex division by zero");
```

That path returns without dividing. It does not invent a quotient and it does
not produce an infinity. IEEE infinities are not modelled. A divisor with one
part nonzero is the schoolbook quotient `(ac+bd)/(c²+d²)` and
`(bc-ad)/(c²+d²)`. That formula is not libstdc++'s scaled division, so a
magnitude that overflows here and not in the library is outside this model.

## What falls back

`exp`, `log`, `log10`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `abs`, `arg`,
`norm`, `polar`, and `proj` are not declared. A unit that names one fails to compile
against this header, and `lower_to_ir` lowers it again with libstdc++
(`extra.cxx_models`). They are not stubbed. Iostream inserters and the
`complex_literals` suffixes are not declared either.

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`.

## Wiring

`PRISM_PIR_MODELS` is an explicit list. One line was added:

```
src/prism/pir/models/cxx/complex
```

`write_cxx_models` then emits `cxx/complex`, which `#include <complex>` opens.
`_prism_test_sources` gained `tests/cpp/test_cxx_complex.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"complex"` next to the other public
header names. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

The conformance check that every public model header is included from both a
libc-models harness and a prism/cxx task is why
`testdata/conformance/libc-models/complex_contracts.cpp` exists. Its false
function is `complex_div0_false`, class `FUNC-CONTRACT`.

## Tests

`tests/cpp/test_cxx_complex.cpp` includes the header by path.

- A constructed `complex<double>(1.5, -2.25)` has those `real()` and `imag()`
  values. A one-argument constructor stores a zero imag.
- `a / complex<double>(0, 0)` and `a / 0.0` call `__prism_check` with
  `ok == 0` and class `FUNC-CONTRACT`.
- `a / complex<double>(0, 1)` is not that failure. The quotient is `2 - i`.

`tests/cxx_models/complex_trace.cpp` is `#include <complex>` and only
constructs, adds, and reads `real` / `imag`. It does not divide by zero: a
library `<complex>` yields a NaN there, and the model reports the check
instead. MSVC `cl` 14.51, `/std:c++latest`, printed the same two lines with
this header on the include path and with the STL (`ctor 1.5 -2.25`,
`add 2 0`). That compile is not libstdc++, and it is not `prism_tests`.

`testdata/conformance/prism/cxx/cxx_complex_true.cpp` only adds and reads
`real` / `imag`. `cxx_complex_false.cpp` divides by `complex<double>(0, 0)`.
`expected` is a YAML boolean. The false witness is `[0]`. Property is
`memsafety`, the same field the other C++ model tasks use. Prism tasks are
not property-scoped, so a `FUNC-CONTRACT` failure still refutes the function.

`sanitizer_blind` is set on both false functions. A native replay compiles
against libstdc++, where division by `(0, 0)` is a quiet NaN, not an ASan or
UBSan abort. The model's failure is `__prism_check`, which that replay does
not link.
