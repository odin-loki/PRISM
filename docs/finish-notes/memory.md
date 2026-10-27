# memory slice

Branch `finish/cxx-memory`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.
`<memory>` is a file, `src/prism/pir/models/cxx/memory`, the same shape as
`vector` and `list`. A directory of that name would make `#include <memory>`
fail with EISDIR. The header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected_verdict`
is a YAML boolean.

## What the model covers

`std::unique_ptr` only, with `default_delete` as its deleter type.

`unique_ptr<T>` owns one `T` that was allocated with `::operator new`.
`reset` stores the new pointer, then destroys the previous object and frees
it with one `::operator delete`. The destructor is `reset()`. Move
construction and move-assignment call `release()` on the source, which
stores nullptr and does not free, then the destination's `reset` takes that
pointer. The source is empty, so its destructor does not free. The
destination owns the object. The previous object of the destination is the
one `reset` frees. `release()` returns the pointer and leaves it with the
caller. `get()` on an empty `unique_ptr` is nullptr.

`operator*` and `operator->` on an empty `unique_ptr` call `__prism_check`
(`PTR-NULL-DEREF`, same class `translate.cpp` gives a libstdc++ nullptr
precondition). They then use the stored pointer. They do not invent a `T`.
`unique_ptr<T[]>` `reset` frees with one array-delete expression, so element
destructors run and the array allocation is released once. `operator[]` on
an empty array `unique_ptr` is the same check.

`default_delete::operator()` builds a temporary `unique_ptr` and lets that
destructor `reset`. The free text stays in `reset` (one `::operator delete(`
and one array delete in the header).

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). A partial
header is intentional. They are not stubbed:

- `shared_ptr`, `weak_ptr`, `allocator`, `pointer_traits`, `uninitialized_*`
- `make_unique` / `make_unique_for_overwrite`
- a deleter other than `default_delete` (static_assert)
- `unique_ptr` of `void`, of a reference, or of a bounded array

`<list>` includes `<memory>` and calls `std::construct_at` /
`std::destroy_at`. Those names are not in this header. A translation unit
compiled with the list model header included will fail and fall back to
libstdc++ for the whole unit. This slice does not edit other model files or
`src/prism/pir/stage.cpp`. `stage.cpp` already puts the embedded `cxx/`
directory on the include path.

`testdata/conformance/libc-models/cxx_std_contracts.cpp` already includes
`<memory>` and uses the operations this model defines (own, `*`, `release`,
`reset`, `get`, bool, destructor). Its comment still says `unique_ptr` is
not replaced. That file was not edited.

## Tests

`tests/cpp/test_cxx_memory.cpp` reads the header. Empty `operator*` is a
`__prism_check` and returns `*_M_ptr`. Object `reset` contains one
`::operator delete(` after the pointer is replaced, guarded by a null check.
Array `reset` contains one array delete the same way. The header has each
of those frees once. Move-assignment is `reset(__u.release())` with no free
of its own. `release` stores nullptr and does not free. The destructor is
`reset()`, so a moved-from `unique_ptr` does not free.

`tests/cxx_models/memory_trace.cpp` includes `<memory>` and uses only
`unique_ptr`. It owns, reads, resets, moves, move-assigns, releases, and
uses `unique_ptr<int[]>`.

`testdata/conformance/prism/cxx/cxx_memory_true.cpp` owns one `int`, reads
it, and lets the destructor free it (`memsafety` true).
`cxx_memory_false.cpp` uses `operator*` on an empty `unique_ptr` when `c`
is 0 (`memsafety` false, witness `[0]`).

## Wiring

`PRISM_PIR_MODELS` gained `src/prism/pir/models/cxx/memory`.
`_prism_test_sources` gained `tests/cpp/test_cxx_memory.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"memory"` next to the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.
