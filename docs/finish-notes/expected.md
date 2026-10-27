# expected slice

Branch `finish/cxx-expected`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.
`std::expected` is a header file, `src/prism/pir/models/cxx/expected`, the same
shape as `vector`. A directory of that name would make `#include <expected>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for.

## What the model covers

An `expected<T, E>` holds either a `T` or an `E`, stored inline in one union.
It is never both. Neither alternative is a separate heap allocation. The
destructor ends the active alternative's lifetime with an explicit destructor
call. It does not call `delete`.

`std::unexpected<E>` holds that `E` by value. Constructing an `expected` from
an `unexpected` stores the error alternative. Constructing from a `T` (copy
or move), and the default constructor, store a `T`. The default constructor
value-initializes `T`, which is the engaged state, not an error.

`has_value()` and `operator bool` report which alternative is stored.
`value()`, `operator*`, and `operator->` return the stored `T` only when it
is active. That call on an unexpected is `__prism_check` with class
`CXX-EXPECTED-NULL` (the same intrinsic the libc models use, literal class
and message). `error()` returns the stored `E` only when the error
alternative is active. `error()` on a success value is `__prism_check` with
class `CXX-EXPECTED-ERROR`. Neither failure path returns a default `T` or a
default `E`, and neither invents a value.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- `and_then`
- `or_else`
- `transform`
- `value_or`
- `transform_error`, `emplace`, `swap`, comparison
- `expected<void, E>`

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It is not edited here.

`CMakeLists.txt` `PRISM_PIR_MODELS` lists `src/prism/pir/models/cxx/expected`,
so the header is embedded as `cxx/expected`. `_prism_test_sources` lists
`tests/cpp/test_cxx_expected.cpp`. The trace source
`tests/cxx_models/expected_trace.cpp` is compiled by that driver, not by
CMake. `tests/cpp/test_qa_conformance.cpp` names `expected` in the public
header array. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

## Tests

`tests/cxx_models/expected_trace.cpp` includes `<expected>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The happy path prints
the same lines: a success `expected<int, int>` yields 42 (not a default 0),
a copy yields 42, `operator->` / `operator*` yield the stored pair, an
`unexpected<int>` yields 3, an unexpected `expected` yields error 7,
`has_value()` is false, an `E` with no default constructor is constructed
and read back as 5, and an error type with no default constructor is read
back as 9.

`argv` `value` and `star` call `value()` and `operator*` on an unexpected.
`argv` `error` calls `error()` on a success value. The model binary aborts
with `CHECK CXX-EXPECTED-NULL` or `CHECK CXX-EXPECTED-ERROR` and does not
print `fake=`.

`tests/cpp/test_cxx_expected.cpp` is that comparison as a doctest. Without a
usable clang++ it is `NOTRUN`, same as the other C++ model driver.
`testdata/conformance/prism/cxx/cxx_expected_true.cpp` reads `value()` only
when the expected holds a `T` (`memsafety` true). `cxx_expected_false.cpp`
calls `value()` on an unexpected (`false`, witness `[0]`). The verdicts in
those yml files are YAML booleans. PIR was not run.

`testdata/conformance/libc-models/expected_contracts.cpp` is the contract
harness (`#include <expected>`). `expected_value_false` plants
`CXX-EXPECTED-NULL`. `expected_error_false` plants `CXX-EXPECTED-ERROR`.
