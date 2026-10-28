# numeric lcm

Branch `finish/cxx-numeric-lcm`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::lcm` is declared on the existing header
`src/prism/pir/models/cxx/numeric`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

`std::midpoint` and `std::gcd` are unchanged. `gcd(0, 0)` is 0. Midpoint still
takes the distance in the corresponding unsigned type and applies half of that
distance to one argument.

The solver was not run. This header is not a proof.

## What lcm does

`std::lcm` takes two integer arguments other than `bool`, the same constraint
as `std::gcd`. The result type is `common_type_t` of the two arguments.

Magnitudes are the absolute values in the unsigned common type. A negative
signed value, including the minimum of that type, converts by unsigned
subtraction. There is no signed negation.

`lcm(0, x)`, `lcm(x, 0)`, and `lcm(0, 0)` return 0. That includes
`lcm(0, INT_MIN)` and `lcm(INT_MIN, 0)`. The zero check is before any product.

Otherwise the value is `|m| / gcd(|m|, |n|) * |n|` in that unsigned type. Both
magnitudes are nonzero, so the quotient is at least 1. If that product does
not fit in the return type, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::lcm product does not fit in the return type")`

and then `__builtin_trap`. That path has no return. It does not return a
wrapped product, and it does not return 0 for a nonzero pair.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

A product that fits is cast back to the return type. `lcm(INT_MAX, 1)` is
`INT_MAX`. `lcm(INT_MAX, 2)` does not fit in `int`. `lcm(INT_MIN, 1)` does not
fit in `int`. `lcm(INT_MAX, 2LL)` fits in `long long`.

## What is still not declared

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). A partial header
is intentional:

- `accumulate`
- `reduce`
- `transform_reduce`
- `inner_product`
- `adjacent_difference`
- `partial_sum`
- `iota`
- `exclusive_scan`

Floating-point and pointer `midpoint` are not declared. `lcm` of a
non-integer or of `bool` is not a candidate. No container header is included.
The header does not include `<memory>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/numeric`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_numeric.cpp`. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They still report `std::lcm` as
unencoded (`C++ std::lcm unencoded`) before a header is inlined. The
`std::gcd` gate is earlier in the same list. Until those gates are gone, a
task that names `std::lcm` does not reach this header. `docs/PIR.md` was not
edited.

## Tests

`tests/cpp/test_cxx_numeric.cpp` used to require that the header text not
contain `lcm(`. It now requires the `lcm` definition, the quotient
`__um / __g`, the `FUNC-CONTRACT` check, `__builtin_trap` before the product
return, and no return on the failing path. The midpoint text still rejects
`a + b`. The header is still a regular file. The declaration of
`__prism_check` is not a definition.

`tests/cxx_models/numeric_trace.cpp` is `#include <numeric>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled twice
(`-isystem` the model directory, and without it). Stdout matched:

```
hi_neg=0 hi_zero=0 hi_eq=1
near_neg=0 near_zero=0 near=2147483646
small=3
gcd00=0
gcd=6
gcd0=15
gcd_neg=2
lcm00=0
lcm0=0
lcm_x0=0
lcm=36
lcm_neg=12
lcm_both=12
lcm_max=2147483647
lcm_ll=4294967294
lcm_u=36
lcm_mix=24
```

`argv` `zero` on the model printed `zero_min=0 min_zero=0 zero_zero=0`.
`argv` `overflow` aborted with
`CHECK FUNC-CONTRACT: std::lcm product does not fit in the return type` and
printed no `wrapped=` line. `constexpr int v = std::lcm(INT_MAX, 2)` is not a
constant expression: that call evaluates `__prism_check`.

libstdc++ with `_GLIBCXX_ASSERTIONS` aborts an overflowing `lcm(INT_MAX, 2)`
on `!__overflow`, and it rejects a magnitude of `INT_MIN` inside its
absolute-value helper. Those runs are separate from the matched stdout.
In-range values, including `lcm(0, 0)`, `lcm(-4, 6)`, and `lcm(INT_MAX, 2LL)`,
matched.

cmake, ninja, `prism_tests`, and pir were not run.
