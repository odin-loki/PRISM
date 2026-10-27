# source_location slice

Branch `finish/cxx-source-location`, base `40b0f05c0f791a740cabd54b095e18dba674d11d`.

`std::source_location` is one extensionless file,
`src/prism/pir/models/cxx/source_location`. A directory of that name would
make `#include <source_location>` fail. The include guard is
`_GLIBCXX_SOURCE_LOCATION`. `#pragma GCC system_header`. Namespace `std`.
The header includes no other standard header.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What the model is

Partial. `current()` takes the call's `__builtin_FILE()`,
`__builtin_LINE()`, and `__builtin_FUNCTION()` as default arguments, so
those builtins are evaluated at the call. `file_name()` and
`function_name()` return those pointers. `line()` returns that builtin
line. `__builtin_FUNCTION()` is the unqualified name (`main`), not the
signature libstdc++ stores from `__builtin_source_location()`
(`int main()`). A successful location is not a fixed line, a fixed column,
or a stand-in name.

`__builtin_COLUMN()` is used when the compiler provides it
(`__has_builtin(__builtin_COLUMN)`, or GCC 7 and later when that feature
test is absent). `column()` returns that value. When the compiler did not
provide a column, `column()` returns 0. The comment on that path says the
compiler did not provide a column. Column 0 is that absence, not a column
that was invented.

A default-constructed object has not captured a call: `line()` and
`column()` are 0, and the name pointers are empty. That is not `current()`.

Anything else is omitted, so an unmodeled use fails to compile and the pir
stage lowers that translation unit again with libstdc++
(`extra.cxx_models`).

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/source_location`

so the existing embed writes `cxx/source_location` and the existing
`-isystem` directory is enough for `#include <source_location>`.
`_prism_test_sources` gained `tests/cpp/test_cxx_source_location.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `source_location` in the
header-name array. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged.

`testdata/conformance/libc-models/source_location_contracts.cpp` is the
`#include <source_location>` that case searches for. It is not a scored
task: there is no yml and no false twin. This header does not plant a
defect of its own.

## Tests

`tests/cpp/test_cxx_source_location.cpp` reads the header as text. The text
contains `__builtin_FILE` and `__builtin_LINE`, and it does not contain
`unknown`. The path is a regular file.

`tests/cxx_models/source_location_trace.cpp` includes `<source_location>`,
calls `current()`, and returns 0 only when `line()` is not 0.
`testdata/conformance/prism/cxx/cxx_source_location_true.cpp` does the same
check under `no-oob`: `a[1]` is reached only when `line()` is 0, and that
line is not 0, so the task is true. There is no false task.

clang++ 18 compiled the header (`-std=c++23 -Wall -Wextra -Werror`, this
file ahead of libstdc++ on the include path). `current()` in `main`
reported that call's line and, with `__builtin_COLUMN`, that call's column.
`file_name()` was the source path. With `__has_builtin` forced false,
`column()` was 0 and `line()` was still the call's line. The trace, the
true task, and the contracts function each exited 0. cmake, `prism_tests`,
and pir were not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ source_location unencoded` (`is not a source_location model`) from the
source text, before the header is inlined. Those files were not edited.
Until that gate is gone, `cxx_source_location_true` does not reach this
header. `docs/PIR.md` was not edited.
