# any slice

Branch `finish/cxx-any`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.
`std::any` is a header file, `src/prism/pir/models/cxx/any`. A directory of
that name would make `#include <any>` fail with EISDIR and every such unit
would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. This header is not a proof.

## What the model covers

An empty `any` holds nothing. `has_value()` is false for a default any,
after `reset()`, and after a move from that object. `type()` is not
declared.

The contained object is one heap allocation. `reset`, assignment, and the
destructor run its destructor and free that allocation. A move steals the
pointer and leaves the source empty, so the source destructor does not free
the object the destination now owns. Self-move-assignment is a no-op, as in
libstdc++, so the same object is not freed twice.

`any_cast<T>` on an empty any, or on an any holding a different type, is
`__prism_check(..., "FUNC-CONTRACT", "any_cast requires a contained object of this type")`
and then `throw bad_any_cast`. That path does not return `T{}` and does not
read another type's bytes as `T`. A cast of the type that was stored returns
that object. The empty path does not load a type tag through a null pointer.

libstdc++ keeps a nothrow-movable type that fits in a pointer inside the
`any`. This model always uses the heap, so a move does not call `T`'s move
constructor. A type larger than a pointer takes libstdc++'s external
manager; the trace uses that size, and the construction counts match.

## What falls through to libstdc++

Omitted APIs are not declared. A unit that names one fails to compile
against this header, so pir falls back to libstdc++ (`extra.cxx_models`).
They are not stubbed:

- `make_any`
- `any_cast` pointer overloads
- `type()`
- comparison
- the `initializer_list` `emplace` / `in_place` overloads

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model. It was
not edited. `CMakeLists.txt` lists `src/prism/pir/models/cxx/any` in
`PRISM_PIR_MODELS` and `tests/cpp/test_cxx_any.cpp` in `_prism_test_sources`.
`tests/cpp/test_qa_conformance.cpp` names `any` in the header array. The
iostream file and the `deque/deque` skip in that test are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not an
any model" from the source text, before the header is inlined. Those exits
stay. A bmc run of these tasks is still that NEEDS-HARNESS. PIR is the
stage that puts the header on the include path.

## Tests

`tests/cpp/test_cxx_any.cpp` reads the header: the bad cast is
`__prism_check`, and the file does not return `T{}` or `reinterpret_cast`
the storage. With clang++ 18.1.3, libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`, `any_trace` compiled
twice (`-isystem` the model directory, and without it). Stdout matched,
including construction order for a heap-sized element, move, reset, and a
stored `int`. The model binary's `e` / `w` / `i` modes exit 0: an empty
`any_cast` and a wrong-type `any_cast` are one failed check and
`bad_any_cast` (not an invented `int`); `any_cast<int>` of a stored `42` is
`42`.

`testdata/conformance/prism/cxx/cxx_any_true.cpp` casts the `int` that was
stored (`noassert`, true). `cxx_any_false.cpp` casts `int` from an empty any
when the argument is 0, and from an any holding a `double` otherwise
(`false`, witness `[0]`). `expected` is a YAML boolean.
`testdata/conformance/libc-models/any_contracts.cpp` is the libc-contract
harness (`#include <any>`): round-trip and move true; empty and wrong-type
`any_cast` are `FUNC-CONTRACT`.
