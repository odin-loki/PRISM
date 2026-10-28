# coroutine equality

Branch `finish/cxx-coroutine-equal`, base `fc5655e019dc4f99d49cc78713c3849bf82c2dc1`.

`std::coroutine_handle<void>` stays the extensionless file
`src/prism/pir/models/cxx/coroutine`. A directory of that name would make
`#include <coroutine>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_COROUTINE`. `#pragma GCC
system_header`. Namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`. Comparing two handles is not a proof that a
coroutine ran.

## What changed

`operator==` for `coroutine_handle<void>` is `address() == address()`.
`operator!=` is the negation of that comparison. Two default handles compare
equal, and each compares equal to `from_address(nullptr)`, because every one
of those addresses is `nullptr`. A default handle is not equal to
`from_address` of a non-null pointer. Two handles built from the same
non-null pointer compare equal. The pointer is still only stored.
`from_address` does not turn it into a frame, and the comparison does not
resume one.

## What stayed

`resume()`, `destroy()`, and `done()` are unchanged. Each is a
`__prism_check` with class `FUNC-CONTRACT` (`_M_fr_ptr != nullptr` is 0 or
1) and then `__builtin_trap`, including when `from_address` stored a
non-null pointer. They do not call a coroutine builtin and they do not
return a value. `done()` does not `return true`. True would look like a
finished coroutine. A non-null address makes the check succeed (the
predicate is the null check) and the trap still runs, so the call does not
report success. The check messages still say "on a null handle".

The C++ destructor is not `destroy()`. A default handle going out of scope
does not trap.

## What falls back

Not declared, so a unit that names one fails to compile against this header
and pir falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `promise_type`
- `operator co_await`
- `noop_coroutine`
- `suspend_always`, `suspend_never`, `coroutine_traits`
- `from_promise` and `coroutine_handle` for a promise argument other than
  `void`
- `operator<=>`

The header does not include `<memory>`, `<thread>`, `<exception>`,
`<atomic>`, or `<compare>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/coroutine` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_coroutine.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already
lists `"coroutine"` in the header-name array. That array was not edited.

## Tests

`tests/cpp/test_cxx_coroutine.cpp` still reads the header text. `resume`,
`destroy`, and `done` each contain `__prism_check` (`FUNC-CONTRACT`, null
pointer fails) before `__builtin_trap`, and none of those bodies returns.
`done` does not contain `true`. The equality bodies return
`__a.address() == __b.address()` and `!(__a == __b)`. They do not check and
they do not trap. The header does not declare `promise_type`, `co_await`,
`noop_coroutine`, `suspend_always`, `suspend_never`, or `operator<=>`. On a
compiler that provides the builtins, two default handles compare equal, a
default handle equals `from_address(nullptr)`, and a default handle is not
equal to `from_address` of a non-null pointer. Those checks do not call
`resume`.

`tests/cxx_models/coroutine_trace.cpp` is `#include <coroutine>`. It prints
`null 1 from 1 live 0` and then `eq 1 diff 1 same 1`. `eq` is a default
handle compared with `from_address(nullptr)`. `diff` is a default handle
compared with `from_address` of a non-null pointer. `same` is two handles
from that same pointer, and `address()` is that pointer. `argv` `resume`,
`destroy`, and `done` still call those members on a null handle.

`testdata/conformance/prism/cxx/cxx_coroutine_true.cpp` is unchanged.
`cxx_coroutine_equal_true.cpp` compares two default handles and a default
handle with `from_address` of a local `int`. It does not dereference that
address (`memsafety`, `expected: true`). `cxx_coroutine_false.cpp` is
unchanged.
`testdata/conformance/libc-models/coroutine_contracts.cpp` adds
`coroutine_equal_defaults_true` and `coroutine_equal_address_true`. Both are
true. The null `resume`, `destroy`, and `done` functions stay false, class
`FUNC-CONTRACT`.

clang++ 18.1.3 compiled the header, the trace, the doctest translation unit,
both existing prism tasks, the equality task, and the harness
(`-std=c++23 -Wall -Wextra -Werror`, this file ahead of libstdc++). The
trace was also built with `-fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. Stdout matched the model and libstdc++:

```
null 1 from 1 live 0
eq 1 diff 1 same 1
```

Both binaries exited 0. `resume`, `destroy`, and `done` on a default handle
aborted with `CHECK FUNC-CONTRACT:` and did not print `resumed`,
`destroyed`, or `done=1`. A `__prism_check` that returns still reaches
`__builtin_trap` on a null handle and does not print those lines. The same
three calls on `from_address` of a non-null pointer print `CHECK 1
FUNC-CONTRACT:` and then trap. They do not print those lines either.
`prism_tests` was not run, so the doctest `TEST_CASE` bodies were not
executed. Compiling that file did evaluate the `static_assert`s.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ coroutine_handle unencoded` (`is not a coroutine_handle model`) from
the source text, before a header is inlined. A `co_await` / `co_yield` /
`co_return` is still `C++ coroutine unencoded` (`is not a coroutine model`).
Those files were not edited. Until that gate is gone, these tasks do not
reach this header. `docs/PIR.md` was not edited.
