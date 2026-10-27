# future slice

Branch `finish/cxx-future`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.
`std::future` is a header file, `src/prism/pir/models/cxx/future`, the same
shape as `optional`. A directory of that name would make `#include <future>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. This header is not a concurrency proof.

## What the model covers

A default `future` has no shared state. `valid()` is false. `get()` on it
is `__prism_check` with class `CXX-FUTURE-GET` (literal class and message,
the same intrinsic the libc models use). That call does not return a
value-initialised `T`.

`promise::set_value` stores one `T` in a shared state the associated future
can see (`get_future`). `get()` on that future returns the stored value and
then `valid()` is false. A second `get()` is the same check.

A second `set_value` is `__prism_check` with class `FUNC-CONTRACT`. The
check runs before the new `T` is allocated, and the function returns without
writing, so the call is not a silent overwrite and it does not leave the
new value beside the old one.

The shared state is one heap object and, once set, one `T`. The link is a
reference count. It is not `std::shared_ptr`.

## What falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records
the fallback). They are not stubbed:

- `wait`, `wait_for`, `wait_until`
- `async`
- `packaged_task`
- `shared_future`, `future<void>`, and `future<T&>`

A blocking wait that returned immediately would be a fake synchronization
success, so those names are omitted rather than modelled as a no-op. The
header does not include `<memory>`, `<thread>`, `<functional>`, or `<mutex>`.

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It is not edited here.

`CMakeLists.txt` `PRISM_PIR_MODELS` lists `src/prism/pir/models/cxx/future`,
so the header is embedded as `cxx/future`. `_prism_test_sources` lists
`tests/cpp/test_cxx_future.cpp`. The trace source
`tests/cxx_models/future_trace.cpp` is compiled by that driver, not by
CMake. `tests/cpp/test_qa_conformance.cpp` names `future` in the public
header array. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

## Tests

`tests/cxx_models/future_trace.cpp` includes `<future>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The happy path prints
the same lines: a default future is not valid, a future from `get_future`
is valid, `set_value(42)` then one `get()` yields 42 and then not valid,
and a move-only `E` with no default constructor is stored and read back as
5.

`argv` `empty`, `twice`, and `get2` call `get()` on a default future,
`set_value` a second time, and `get()` a second time. The model binary
aborts with `CHECK CXX-FUTURE-GET` or `CHECK FUNC-CONTRACT` and does not
print `fake=`.

`tests/cpp/test_cxx_future.cpp` reads the header without running pir: the
empty `get()` is a `__prism_check` and does not return a value-initialised
`T`, and the header does not define `async` or `wait(`. Without a usable
clang++ the compile half is `NOTRUN`, same as the other C++ model drivers.

`testdata/conformance/prism/cxx/cxx_future_true.cpp` sets one value and
reads it once (`memsafety` true). `cxx_future_false.cpp` calls `get()` on a
default future when the input is 0 (`false`, witness `[0]`). Expected
values are YAML booleans. PIR was not run.
`testdata/conformance/libc-models/future_contracts.cpp` is the libc-contract
harness (`#include <future>`): the round trip and a default `valid()` are
true; empty `get()` and a second `get()` are `CXX-FUTURE-GET`; a second
`set_value` is `FUNC-CONTRACT`.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::async unencoded` (`is not a future model`) from the source text
when the function names `std::future`, `std::promise`, or `std::async`,
before any header is inlined. Those files were not edited. Until that gate
is narrowed, a BMC run of these tasks does not reach this header.
`docs/PIR.md` was not edited.
