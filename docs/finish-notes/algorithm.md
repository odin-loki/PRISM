# algorithm slice

Branch `finish/cxx-algorithm`, base `1278b9dc714ceaee54277a298c50212376942489`.

`<algorithm>` is one extensionless file, `src/prism/pir/models/cxx/algorithm`.
A directory of that name would make `#include <algorithm>` fail with EISDIR.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. `expected` in the yml is a YAML boolean. This header
is not a proof that a range is sorted. Nothing here is `PROVED`.

## What the model is

A partial `<algorithm>`. `std::min`, `std::max`, and `std::clamp` are the
only algorithms, and only for a type comparable with `operator<`.

`min(a, b)` returns `a` when `!(b < a)`, otherwise `b`. Equal arguments
return the first argument. `max` is the symmetric rule: `a` when
`!(a < b)`, otherwise `b`. The result is one of those two arguments. It is
not a third value, and the body does not add the arguments, so a pair of
extreme ints does not wrap to 0. `min(INT_MIN, INT_MAX)` is `INT_MIN`.
`max(INT_MIN, INT_MAX)` is `INT_MAX`.

`clamp(v, lo, hi)` when `hi < lo` calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`, and it does not return 0 as if the range
were valid. In a constant expression the call is ill-formed. When the range
is valid, the result is `lo` if `v < lo`, `hi` if `hi < v`, otherwise `v`.
`clamp(0, INT_MIN, INT_MAX)` is 0 because 0 is `v` and the range contains
it. That 0 is the argument, not a wrapped sum.

## What is not declared

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). A partial header
is intentional. They are not stubbed:

- `sort`
- `find`
- `copy`
- `fill`
- `for_each`
- every other algorithm, including the comparator overloads and the
  `initializer_list` overloads

No `<memory>`, `<functional>`, `<string>`, `<iterator>`, `<ranges>`,
`<vector>`, or `<compare>` header is included. `__prism_check` is declared
`extern "C"` and is not defined here.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/algorithm`

so the existing embed writes `cxx/algorithm` and the existing `-isystem`
directory is enough for `#include <algorithm>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_algorithm.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`algorithm` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged. The names
`iostream` and `ostream` stay absent.

`testdata/conformance/libc-models/algorithm_contracts.cpp` is the
`#include <algorithm>` that case searches for. It is not a scored task.

## Tests

`tests/cxx_models/algorithm_trace.cpp` includes `<algorithm>`. It is compiled
twice (model directory as `-isystem`, and without it). The constant checks
require `min(INT_MIN, INT_MAX)` not to be 0, and equal arguments to be the
first object. `tests/cpp/test_cxx_algorithm.cpp` reads the header and
rejects a `return` on the inverted-clamp path, a `return 0`, and an
addition in `min` or `max`. Without a usable clang++ the compile comparison
is `NOTRUN`. The header read still runs.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. Stdout matched:

```
min_eq=1
max_eq=1
min_lo=2
max_hi=9
clamp_mid=5 id=1
clamp_lo=2 id=1
clamp_hi=9 id=1
wide_min_is0=0 wide_min_is_min=1
wide_max_is0=0 wide_max_is_max=1
clamp_inside_zero=1
ord_eq_first=1 ord_max=2
ptr_eq_first=1
```

`argv` `invert` calls `clamp` with `hi < lo`. The model binary aborts with
`CHECK FUNC-CONTRACT: clamp: hi < lo` and does not print `value=`.
`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`std::sort`, `std::find`, `std::copy`, `std::fill`, and `std::for_each`
compile against libstdc++ and fail against this header (`no member named`
each of those names).

`#include <algorithm>` with `-isystem` on the model directory resolved to
`models/cxx/algorithm`. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_algorithm_true.cpp` checks `min` and
`max` of two small ints, a valid `clamp`, and that `min` of the extreme
ints is not 0. The yml has the same keys as `cxx_array_true.yml`. The
property is `no-overflow`. `expected` is the boolean `true`.

`cxx_algorithm_false.cpp` calls `clamp(0, lo, hi)` when `hi < lo`
(`expected: false`, witness `[1, 0]`). `sanitizer_blind` names that
function. A native replay compiles without `_GLIBCXX_ASSERTIONS`, and
libstdc++ then returns one of the arguments instead of trapping, so UBSan
stays clean. The model's failure is `__prism_check`, not a wrapped sum.

## Still in front of the model

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and `src/prism/bmc_encoder.inc`
still report `std::clamp` as unencoded from the source text, before the
header is inlined (`C++ std::clamp unencoded` / `is not a clamp model`).
Those files were not edited. `std::min` and `std::max` are not gated there.
Until the clamp gate is gone, a task whose source calls `std::clamp` does
not reach this header. `docs/PIR.md` was not edited.
