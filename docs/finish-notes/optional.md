# optional slice

Branch `finish/cxx-optional`, base `e5355001f09c6bd76fb69d7c6eac96949ea17838`.
`std::optional` is a header file, `src/prism/pir/models/cxx/optional`, the same
shape as `vector`. A directory of that name would make `#include <optional>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for.

## What the model covers

Two states. An optional is empty, or it holds a `T` stored by value inside
the optional object (a union member). That is not a separate heap allocation.
`reset()` on an engaged optional, and the destructor, end the contained `T`'s
lifetime with an explicit destructor call. They do not call `delete`.

`has_value()` and `operator bool` read the engaged flag. `value()`,
`operator*`, and `operator->` return the stored `T` only when the optional is
engaged. A disengaged call is `__prism_check` with class `CXX-OPTIONAL-NULL`
(the same intrinsic the libc models use, literal class and message). The
check does not return a default `T` and does not invent a value.

Constructors from `T` (copy and move) are how a program gets an engaged
optional. The default constructor is the empty one.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- `value_or`
- `emplace`
- `transform`
- `and_then` and `or_else` (and, or)
- comparison
- hash
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It is not edited here.

`CMakeLists.txt` `PRISM_PIR_MODELS` lists `src/prism/pir/models/cxx/optional`,
so the header is embedded as `cxx/optional`. `_prism_test_sources` lists
`tests/cpp/test_cxx_optional.cpp`. The trace source
`tests/cxx_models/optional_trace.cpp` is compiled by that driver, not by
CMake. `tests/cpp/test_qa_conformance.cpp` names `optional` in the public
header array. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

## Tests

`tests/cxx_models/optional_trace.cpp` includes `<optional>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The happy path prints
the same lines: an engaged `optional<int>` yields 42 (not a default 0), a
copy yields 42, `operator->` / `operator*` yield the stored pair, `reset()`
drops `has_value`, and an `E` with no default constructor is constructed,
read back as 5, and destroyed on `reset`.

`argv` `empty`, `star`, and `arrow` call `value()`, `operator*`, and
`operator->` on an empty optional. The model binary aborts with
`CHECK CXX-OPTIONAL-NULL` and does not print `fake=`.

`tests/cpp/test_cxx_optional.cpp` is that comparison as a doctest. Without a
usable clang++ it is `NOTRUN`, same as the other C++ model driver.
`testdata/conformance/prism/cxx/cxx_optional_true.cpp` only reads an engaged
optional (`memsafety` true). `cxx_optional_false.cpp` calls `value()` on an
empty optional when the input is 0 (`false`, witness `[0]`). PIR was not run.
