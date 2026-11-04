# memory_resource::is_equal

Slice `finish/cxx-memory-resource-equal`, base `fe245b58e5fb36a4f551345e3f66ef6d8a0a7d9f`.

`std::pmr::memory_resource::is_equal` is declared on
`src/prism/pir/models/cxx/memory_resource`. The header is still a file, with
include guard `_GLIBCXX_MEMORY_RESOURCE`. It includes `<cstddef>` only. It
does not include `<memory>`.

`memory_resource` and `monotonic_buffer_resource` are separate classes.
`allocate` and `deallocate` stay on `monotonic_buffer_resource`. They are
not declared on `memory_resource`. `is_equal` is not a member of
`monotonic_buffer_resource`.

## What is_equal does

```
bool is_equal(const memory_resource& __other) const noexcept
```

The same object compares equal: `this == &__other` returns true. A different
object compares unequal: the following statement returns false. Neither path
allocates or frees. The body does not call `allocate`, `deallocate`, `free`,
`malloc`, `::operator new`, or `::operator delete`. It does not call
`__prism_check` or `__builtin_trap`. It does not call `do_is_equal`.
`do_is_equal` is not declared.

This is not PROVED and not CLEAN. The comment above `is_equal` says so.

## What stayed

`deallocate(nullptr, 0)` returns immediately. That path does not free and does
not crash. It does not call `__prism_check` and it does not call
`__builtin_trap`.

`deallocate(nullptr, n)` for `n > 0` calls
`__prism_check(0, "PTR-NULL-DEREF", "memory_resource::deallocate null pointer")`
and then `__builtin_trap`. The trap is before any free. That arm does not
call `free` and does not call `::operator delete`.

`allocate` of a nonzero size is unchanged. When `n` plus alignment padding
does not fit, the function returns nullptr before a pointer is formed from
the caller buffer.

`polymorphic_allocator`, `synchronized_pool_resource`, and
`unsynchronized_pool_resource` are still not declared. `operator==` for
`memory_resource` is not declared. A use of those names fails to compile
against this header, and the pir stage falls back to libstdc++. They are not
stubbed.

## Tests

`tests/cpp/test_cxx_memory_resource_equal.cpp` reads the header from
`PRISM_SOURCE_DIR`. It checks that the same-object return is
`this == &__other` and comes before `return false`, that the body between
those returns and after the false return does not allocate or free, and that
the comment above `is_equal` says this is not PROVED and not CLEAN. It checks
that `deallocate(nullptr, 0)` is still the no-op return and that
`deallocate(nullptr, n)` for `n > 0` is still `PTR-NULL-DEREF` then
`__builtin_trap` before any free. The header has one `allocate` and one
`deallocate`.

`CMakeLists.txt` lists `tests/cpp/test_cxx_memory_resource_equal.cpp` as one
new line in `_prism_test_sources`, after
`tests/cpp/test_cxx_memory_resource_dealloc.cpp`. No line was removed.

`tests/cpp/test_cxx_memory_resource.cpp` and
`tests/cpp/test_cxx_memory_resource_dealloc.cpp` were not edited.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Werror`, the model
directory ahead of libstdc++ on the include path. An untracked driver
constructed two `memory_resource` objects. The same object compared equal
(`same 1`, `selfb 1`). A different object compared unequal (`diff 0`,
`rev 0`). `allocate(4, 1)` from an 8-byte caller buffer returned a pointer
inside that buffer. `deallocate(nullptr, 0, 1)` returned. `deallocate` of
the handed-out pointer did not free the buffer. A call to
`monotonic_buffer_resource::is_equal` failed to compile.

`tests/cxx_models/memory_resource_trace.cpp` was not edited. The same flags,
once with the model directory and once with libstdc++, printed:

```
inside 1 1
off 0 4
kept 7
alive 7
```

That run is not PROVED and not CLEAN.

`prism_tests` was not executed. This slice does not configure or build.
cmake and ninja were not run.
