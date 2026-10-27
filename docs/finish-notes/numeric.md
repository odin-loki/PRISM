# numeric slice

Branch `finish/cxx-numeric`, base `3e0ebbb46361d4d3c7557657eeeb590d2ae3aa19`.

`<numeric>` is one extensionless file, `src/prism/pir/models/cxx/numeric`.
A directory of that name would make `#include <numeric>` fail with EISDIR.
The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. `expected` in the yml is the boolean `true`. This
header is not a proof.

## What the model is

A partial `<numeric>`. `std::midpoint` for an integer type other than `bool`
takes the distance in the corresponding unsigned type and applies half of
that distance to one argument. It does not form the sum of the two
arguments, so a large positive pair does not wrap. `midpoint` of `INT_MAX`
and `INT_MAX` is `INT_MAX`. The result is one of the two values or strictly
between them.

`std::gcd` is the Euclidean algorithm on the absolute values. Both arguments
zero leaves the loop unentered, so `gcd(0, 0)` is 0.

`lcm` is not declared. A product that does not fit the return type is a
precondition in the standard algorithm. This header does not return a
wrapped product as success.

## What is not declared

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

Floating-point and pointer `midpoint` are not declared either. No container
header is included.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/numeric`

so the existing embed writes `cxx/numeric` and the existing `-isystem`
directory is enough for `#include <numeric>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_numeric.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`numeric` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/numeric_contracts.cpp` is the
`#include <numeric>` that case searches for. It is not a scored task.

## Tests

`tests/cxx_models/numeric_trace.cpp` includes `<numeric>`. It is compiled
twice (model directory as `-isystem`, and without it), the same way
`tests/cpp/test_cxx_list.cpp` compiles its trace. The constant checks require
`midpoint(INT_MAX, INT_MAX)` to be `INT_MAX` and `gcd(0, 0)` to be 0.
`tests/cpp/test_cxx_numeric.cpp` reads the header and rejects the expression
`a + b` as the midpoint, including inside the `midpoint` body. Without a
usable clang++ the compile comparison is `NOTRUN`. The header read still
runs.

Checked with clang++ 18, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. Stdout matched:

```
hi_neg=0 hi_zero=0 hi_eq=1
near_neg=0 near_zero=0 near=2147483646
small=3
gcd00=0
gcd=6
gcd0=15
gcd_neg=2
```

`#include <numeric>` with `-isystem` on the model directory resolved to
`models/numeric`. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_numeric_true.cpp` checks `midpoint` of
two small ints and `gcd(0, 0) == 0`. The yml has the same keys as
`cxx_array_true.yml`. The property is `no-overflow`. There is no false task.
`cxx_midpoint_false.cpp` is already the wrapped sum of two parameters. This
slice does not add another, and the model does not produce a wrapped result.

The in-house pair check still wants a `_false` stem for every `_true` stem.
`cxx_initializer_list_true` is already unpaired on this base. This slice does
not add a wrapped-sum false to balance that count.

## Still in front of the model

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and `src/prism/bmc_encoder.inc`
still report `std::midpoint`, `std::gcd`, and `std::lcm` as unencoded from
the source text, before the header is inlined. Those files were not edited.
Until that gate is gone, `cxx_numeric_true` does not reach this header.
`docs/PIR.md` was not edited.
