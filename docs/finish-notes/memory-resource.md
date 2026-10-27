# memory_resource slice

Branch `finish/cxx-memory-resource`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<memory_resource>` is a file, `src/prism/pir/models/cxx/memory_resource`. A
directory of that name would make `#include <memory_resource>` fail with
EISDIR. The include guard is `_GLIBCXX_MEMORY_RESOURCE`. `#pragma GCC
system_header`. The type lives in namespace `std::pmr`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans. Nothing here is `PROVED`.

## What the model covers

`std::pmr::monotonic_buffer_resource` over a caller buffer: one pointer and
one size. It does not allocate an upstream buffer.

`allocate(n, align)` returns a pointer inside the remaining buffer only when
`n` bytes fit, including alignment padding. The caller buffer is the
alignment base. Padding is the bytes from the current offset up to the next
multiple of `align` (an absolute address would be `ptrtoint` / `inttoptr`,
which this encoder does not model). If `n` plus that padding does not fit,
`__prism_check` runs with class `MEM-OOB-WRITE`
(`monotonic_buffer_resource::allocate exceeds the caller buffer`) and the
function returns nullptr. It does not return a pointer past the end, and it
does not return a non-null pointer from outside the caller buffer. An
alignment that is not a power of two is `FUNC-CONTRACT`, then nullptr.

A null upstream buffer with a non-zero size is `__prism_check` at
construction (`PTR-NULL-DEREF`). The stored size is then 0, so a later
allocate does not form a pointer from that null base.

`deallocate` of a pointer this resource did not hand out is `__prism_check`
(`MEM-INVALID-FREE`). It does not free the caller buffer and it does not
rewind the bump cursor. The destructor is an empty body. It does not free
the caller buffer. `release()` sets the offset back to the start of that
same buffer and does not free it.

Handed-out pointers are recorded in the object (32 of them). The list is not
a heap allocation. A further allocate that still fits is a check rather than
a pointer deallocate could not recognise.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `polymorphic_allocator`
- `synchronized_pool_resource`
- `unsynchronized_pool_resource`
- `memory_resource` as a polymorphic base, `new_delete_resource`,
  `null_memory_resource`, `get_default_resource`, `set_default_resource`
- constructors that allocate from an upstream `memory_resource` (the default
  constructor, `size_t` initial size, and any constructor that takes an
  upstream pointer)

`src/prism/pir/models/cxx/memory` is a `unique_ptr` model. It does not
provide `allocator`. This header does not include it.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not a pmr
model" when the source text contains `std::pmr` or `pmr::`, before any
header is inlined. This slice does not change those exits, and it does not
edit `stage.cpp`. `stage.cpp` already puts the embedded `cxx/` directory on
the include path.

## Tests

`tests/cpp/test_cxx_memory_resource.cpp` reads the header. The file is not a
directory. `allocate` calls `__prism_check` when `n` plus alignment padding
does not fit in the bytes still left, and that check is before the pointer
is formed from the caller buffer. The failure return is nullptr. The header
does not call `::operator new` and does not include the memory header. The
constructor checks a null upstream buffer with a non-zero size. The
destructor body is empty. `deallocate` checks a pointer that was not handed
out and does not free the caller buffer.

`tests/cxx_models/memory_resource_trace.cpp` includes `<memory_resource>`.
It allocates 4 bytes and then 4 bytes from an 8-byte caller buffer, with
alignment 1, and both pointers are inside that buffer. `deallocate` does
not wipe the byte that was stored, and the buffer is still there after the
resource dies. An allocate larger than the buffer is not in this trace:
libstdc++ takes that from the upstream resource, and the model checks
instead. The doctest does not shell out to the trace.

`testdata/conformance/prism/cxx/cxx_memory_resource_true.cpp` allocates 4
bytes from an 8-byte caller buffer and writes through that pointer
(`no-oob`, true). `cxx_memory_resource_false.cpp` allocates 9 bytes from
that 8-byte buffer (`false`, witness `[0]`).

`testdata/conformance/libc-models/memory_resource_contracts.cpp` is the
libc-contract harness: the same 4-from-8 allocate, and a destructor that
leaves the caller buffer in place; oversized allocate is `MEM-OOB-WRITE`,
a null buffer with a non-zero size is `PTR-NULL-DEREF`, and `deallocate`
of a foreign pointer is `MEM-INVALID-FREE`.

`sanitizer_blind` is set on those false functions. A native replay compiles
against libstdc++, where an oversized allocate comes from the upstream
resource, a null initial buffer is skipped, and `deallocate` is a no-op.
None of those is an ASan or UBSan abort. The model's failure is
`__prism_check`, which that replay does not link.

## Wiring

`PRISM_PIR_MODELS` gained one line, `src/prism/pir/models/cxx/memory_resource`.
`_prism_test_sources` gained `tests/cpp/test_cxx_memory_resource.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"memory_resource"` next to the
other public headers, including `"memory"`. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
