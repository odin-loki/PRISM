# memory_resource deallocate of a null pointer

Slice `finish/cxx-memory-resource-dealloc`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::pmr::monotonic_buffer_resource::deallocate` in
`src/prism/pir/models/cxx/memory_resource` distinguishes a null pointer by
size. The header is still a file, with include guard `_GLIBCXX_MEMORY_RESOURCE`.
It includes `<cstddef>` only. It does not include `<memory>`.

`deallocate(nullptr, 0)` returns immediately. That path does not free and does
not crash. It does not call `__prism_check` and it does not call
`__builtin_trap`.

`deallocate(nullptr, n)` for `n > 0` calls
`__prism_check(0, "PTR-NULL-DEREF", "memory_resource::deallocate null pointer")`
and then `__builtin_trap`. The trap is before any free. That arm does not
call `free`, does not call `::operator delete`, and does not run the
handed-out check. It does not free the caller buffer.

A non-null pointer this resource did not hand out is still
`MEM-INVALID-FREE` (`monotonic_buffer_resource::deallocate pointer was not
handed out by this resource`). That path does not free the caller buffer and
does not rewind the bump cursor.

`allocate` of a nonzero size is unchanged. When `n` plus alignment padding
does not fit, the function returns nullptr before a pointer is formed from
the caller buffer and before that pointer is stored in the handed-out list.
A null result on that path is not a handed-out object. The failure return
stays before `_M_buf +` and before `_M_hand[_M_nhand] = __p`.

This is not PROVED and not CLEAN.

## Tests

`tests/cpp/test_cxx_memory_resource_dealloc.cpp` reads the header from
`PRISM_SOURCE_DIR`. It checks the size-0 return, the order of the
`PTR-NULL-DEREF` check and `__builtin_trap`, that the trap arm has no free
and no handed-out check, and that a failing `allocate` returns nullptr
before the pointer is recorded. The header text contains `not PROVED` and
`not CLEAN`.

`CMakeLists.txt` lists `tests/cpp/test_cxx_memory_resource_dealloc.cpp` as
one new line in `_prism_test_sources`, after
`tests/cpp/test_cxx_memory_resource.cpp`. No line was removed.

`tests/cpp/test_cxx_memory_resource.cpp` still reads `deallocate`. The body
still has the handed-out check. It has no `free(` call and no
`::operator delete`.

`prism_tests` was not executed. This slice does not configure or build.
cmake and ninja were not run.
