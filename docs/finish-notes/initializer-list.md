# initializer_list slice

Branch `finish/cxx-initializer-list`, base `e5355001f09c6bd76fb69d7c6eac96949ea17838`.

`std::initializer_list` is one extensionless file,
`src/prism/pir/models/cxx/initializer_list`. A directory of that name would
make `#include <initializer_list>` fail with EISDIR. The include guard is
`_GLIBCXX_INITIALIZER_LIST` (and the older `_INITIALIZER_LIST`, so both
headers are not active together). `#pragma GCC system_header`. Namespace
`std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What the model is

A non-owning view of the array the compiler builds for a braced list: a
`const T*` and a size. It does not allocate and does not free. The
destructor is defaulted, so it does not free that array; the compiler owns
it. The private constructor only stores the pointer and the length the
compiler passes in. An empty list has size 0 and `begin() == end()` (the
same pointer, with no arithmetic on null and no fabricated element).

Members are `begin()`, `end()`, and `size()`, plus the standard free
`begin` / `end`. Subscript is not part of `std::initializer_list` and is
not declared. Anything else is omitted so an unmodeled use fails to compile
and `lower_to_ir` lowers that translation unit again with libstdc++
(`extra.cxx_models`). No container header is included.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/initializer_list`

so the existing embed writes `cxx/initializer_list` and the existing
`-isystem` directory is enough for `#include <initializer_list>`.
`_prism_test_sources` gained `tests/cpp/test_cxx_initializer_list.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `initializer_list` in the
header-name array. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged.

`testdata/conformance/libc-models/initializer_list_contracts.cpp` is the
`#include <initializer_list>` that case searches for. It is not a scored
task: a false twin would not be a defect this header checks.

## Tests

`tests/cxx_models/initializer_list_trace.cpp` includes `<initializer_list>`
and is compiled twice (model directory as `-isystem`, and without it), the
same way `tests/cpp/test_cxx_list.cpp` includes `<list>`. clang++ against
libstdc++ 14, with `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`, printed the same lines both ways:

```
empty size=0 eq=1 hits=0
copy_dtors=0 copy_frees=0
still=6
walk=60
empty_walk=0
```

An empty list is size 0 and `begin()==end()`. Destroying a copy does not
run element destructors or class `operator delete`, and the original
elements still sum to 6. The walk uses `size()` as the bound. cmake,
`prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_initializer_list_true.cpp` is memsafety
true: it walks `size()` elements of `{1, 2, 3, 4}` and does not read
`end()`. There is no false task. This model does not plant a check of its
own, and a past-the-end read is not a defect it can name.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ initializer_list unencoded` (`is not a temporary-lifetime model`)
from the source text, before the header is inlined. Those files were not
edited. Until that gate is gone, `cxx_initializer_list_true` does not reach
this header. `docs/PIR.md` was not edited.
