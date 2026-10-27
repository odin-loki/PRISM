# functional slice

Branch `finish/cxx-functional`, base `2d487e2a3effd894f0ea923089a7d130db99a10c`.
`std::function` is a header file, `src/prism/pir/models/cxx/functional`, the
same shape as `optional`. A directory of that name would make
`#include <functional>` fail with EISDIR and every such unit would fall back
to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. `expected` values are YAML booleans. This header is not a proof.

## What the model covers

Only `std::function` for a function-pointer target. The pointer lives in the
object. A general callable is not allocated on the heap, so the destructor,
move-assignment, and an empty function have nothing to free and do not call
the target.

An empty `std::function` has no target. `operator bool` is false. `operator()`
on an empty function is `__prism_check` with class `CXX-STD-FUNCTION-NULL` and
a message that names `bad_function_call`. That path does not return a default
result and does not call a null function pointer. Assigning a function pointer
makes `operator()` call that pointer. A move copies the pointer, leaves the
source empty, and does not call the target during the move. The destructor of
an empty function does not call a target.

## What falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records
the fallback). They are not stubbed. A partial header is intentional:

- `bind`
- `mem_fn`
- `ref` and `cref`
- `invoke`
- `not_fn`
- `bind_front`
- `reference_wrapper`

The header does not `#include <memory>` or `<tuple>`.

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It was not edited.

`CMakeLists.txt` `PRISM_PIR_MODELS` lists
`src/prism/pir/models/cxx/functional`, so the embedded name is
`cxx/functional`. `_prism_test_sources` lists
`tests/cpp/test_cxx_functional.cpp`. The trace source
`tests/cxx_models/functional_trace.cpp` is compiled by that driver, not by
CMake. `tests/cpp/test_qa_conformance.cpp` names `functional` in the public
header array. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return
"is not a type-erased callable model" when the source text contains
`std::function`, before any header is inlined. Those exits are still in
place. This slice does not edit them.

## Tests

`tests/cxx_models/functional_trace.cpp` includes `<functional>` and only uses
`std::function`. Compiled twice (`-isystem` the model directory, and without
it) under clang++ 18.1.3, libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. Stdout matched, including a stored call
that returns 42 (not a made-up 0), a move that leaves the source empty
without calling, and `function<E()>` where `E` has no default constructor
yielding 5.

`argv` `empty` calls `operator()` on an empty `std::function`. The model
binary aborts with `CHECK CXX-STD-FUNCTION-NULL` and `bad_function_call`, and
does not print `fake=`.

`tests/cpp/test_cxx_functional.cpp` is that comparison as a doctest. Without
a usable clang++ it is `NOTRUN`, same as the other C++ model driver.
`testdata/conformance/prism/cxx/cxx_functional_true.cpp` calls a function
pointer that was stored (`memsafety` true). `cxx_functional_false.cpp` calls
an empty `std::function` when the input is 0 (`false`, witness `[0]`).
`testdata/conformance/libc-models/functional_contracts.cpp` is the
libc-contract harness so the embedded-header check sees
`#include <functional>`. PIR was not run. No engine configure, no
`prism_tests`.
