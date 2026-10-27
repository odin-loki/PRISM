# coroutine slice

Branch `finish/cxx-coroutine`, base `1278b9dc714ceaee54277a298c50212376942489`.

`std::coroutine_handle<void>` is the extensionless file
`src/prism/pir/models/cxx/coroutine`. A directory of that name would make
`#include <coroutine>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is `_GLIBCXX_COROUTINE`, so libstdc++'s own
header is not also included. The file is a `#pragma GCC system_header` and
the type lives in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`. This header is not a proof that a coroutine runs.

## What the model stores

Partial `std::coroutine_handle<void>` only. The primary template is
declared so `coroutine_handle<>` is `coroutine_handle<void>`, and it is not
defined for any other argument. A default handle stores `nullptr` in
`_M_fr_ptr`. `address()` of that handle is `nullptr`.
`from_address(nullptr)` writes that pointer into a handle, so the result is
a null handle. `explicit operator bool` is false for that null pointer. It
is not `done()`.

`resume()` and `destroy()` on a null handle call `__prism_check` with class
`FUNC-CONTRACT` (`_M_fr_ptr != nullptr` is 0) and then `__builtin_trap`.
They do not call a coroutine builtin, they do not allocate a frame, and
they do not return a value. `done()` is the same check and then the same
trap. It does not `return true`. True would look like a finished coroutine.
A non-null address is stored by `from_address` and is not a frame this
header can run, so those three still trap after the check.

The C++ destructor is not `destroy()`. A default handle going out of scope
does not trap.

`__prism_check` is declared `extern "C"` and is not defined in this header.

## What falls back

Not declared, so a unit that names one fails to compile against this header
and pir falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `promise_type`
- `operator co_await`
- `noop_coroutine` (omitted so the use falls back; a no-op handle that
  resumed successfully would pretend a coroutine ran)
- `suspend_always`, `suspend_never`, `coroutine_traits`
- `from_promise` and `coroutine_handle` for a promise argument other than
  `void`
- handle comparisons (`==`, `!=`)

The header does not include `<memory>`, `<thread>`, `<exception>`, or
`<atomic>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `PRISM_PIR_MODELS`
gained one line:

```
src/prism/pir/models/cxx/coroutine
```

`_prism_test_sources` gained `tests/cpp/test_cxx_coroutine.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"coroutine"` next to the other
public header names. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged. `iostream` and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_coroutine.cpp` reads the header text. The file is a
regular file. `resume`, `destroy`, and `done` each contain `__prism_check`
(`FUNC-CONTRACT`, null pointer fails) before `__builtin_trap`, and none of
those bodies returns. `done` does not contain `true`. The header does not
declare `promise_type`, `co_await`, or `noop_coroutine`, and it does not
define `__prism_check`. On a compiler that provides the builtins, a default
handle's `address()` is `nullptr` and `from_address(nullptr)` is a null
handle. Those checks do not call `resume`.

`tests/cxx_models/coroutine_trace.cpp` is `#include <coroutine>`. It prints
`null 1 from 1 live 0` for a default handle and `from_address(nullptr)`.
`argv` `resume`, `destroy`, and `done` call those members on a null handle.
The model must abort with `CHECK FUNC-CONTRACT:` and must not print
`resumed`, `destroyed`, or `done=1`.

`testdata/conformance/prism/cxx/cxx_coroutine_true.cpp` reads the two null
addresses and does not dereference them (`memsafety`, `expected: true`).
`cxx_coroutine_false.cpp` calls `resume()` on a default handle when the
input is 0 (`expected: false`, witness `[0]`).
`testdata/conformance/libc-models/coroutine_contracts.cpp` includes
`<coroutine>`. The null address and `from_address(nullptr)` are true.
`coroutine_resume_null_false`, `coroutine_destroy_null_false`, and
`coroutine_done_null_false` are false, class `FUNC-CONTRACT`.

clang++ 18.1.3 compiled the header, the trace, both prism tasks, and the
harness (`-std=c++23 -Wall -Wextra -Werror`, this file ahead of libstdc++).
The happy-path line was `null 1 from 1 live 0` for the model and for
libstdc++. `resume`, `destroy`, and `done` on a default handle aborted with
`CHECK FUNC-CONTRACT:` and did not print `resumed`, `destroyed`, or
`done=1`. A `__prism_check` that returns still reaches `__builtin_trap`
and does not print those lines.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ coroutine_handle unencoded` (`is not a coroutine_handle model`) from
the source text, before a header is inlined. A `co_await` / `co_yield` /
`co_return` is still `C++ coroutine unencoded` (`is not a coroutine model`).
Those files were not edited. Until that gate is gone, these tasks do not
reach this header. `docs/PIR.md` was not edited.
