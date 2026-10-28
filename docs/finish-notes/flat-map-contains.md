# flat_map contains

Branch `finish/cxx-flat-map-contains`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::flat_map::contains` is declared on the existing header
`src/prism/pir/models/cxx/flat_map`. The header is still a file, not a
directory. `operator[]`, `at()`, and `erase()` are the same functions as
before this slice.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.

## What contains does

`bool contains(const key_type&) const` calls the const `_M_find` and
returns whether that node is non-null.

A missing key, including on an empty container, returns false. `_M_find`
walks the existing nodes and returns nullptr. That path does not call
`::operator new`, `_M_make`, or `_M_link`, and it does not change
`_M_size`. A present key returns true. That path is the same lookup and
does not allocate either.

`operator[]` on a missing key still inserts a node whose mapped object is
`mapped_type{}`. `at()` on a missing key still throws `std::out_of_range`
(`flat_map::at: key is not present`) and does not return a default `T`.
`erase` of a present key still destroys that node and frees it with
`::operator delete`. `erase` of a missing key still returns 0 and frees
nothing.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed: insert,
`emplace`, `try_emplace`, `erase(iterator)`, iterators, `find`, `count`,
`clear`, `swap`, and the `Compare` / `KeyContainer` / `MappedContainer`
parameters. The header does not include `<set>`, `<vector>`, or `<memory>`.

`src/prism/pir/stage.cpp`, `src/prism/bmc.cpp`, and `docs/PIR.md` were not
edited. `CMakeLists.txt` was not edited. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The omitted-name list used to say
`find, contains, count`. It now says `find, count, clear`, and the test
requires `contains` on the class:

`bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }`

The const finder returns nullptr for a miss and does not allocate, insert,
or touch size. The comment above `contains` states the missing-key and
present-key results. `operator[]` still links a node built with
`mapped_type{}`. `at()` still throws and does not return a default mapped
value. `erase` still has one `::operator delete`, only on the found path,
then `return 0`.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. The `contains`
mode uses a key whose copy constructor counts node construction, because
this header allocates a node only by constructing a key into it. clang++
18.1.3, `-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined`,
with this header as the `<flat_map>` on the include path. The `contains`
mode printed

`empty contains=0 copies=0 size=0 inserted=1 hit=1 missing=0 copies=0 size=1 at=8`

so a missing key was false with zero copies and size 0, `operator[]`
inserted one node, and both a present key and another missing key left
size at 1 with zero further copies. `at()` read the stored 8. `miss`
printed `threw`. The default mode printed `erase present=1 missing=0`.
`uaf` aborted with `heap-use-after-free` in `flat_map::erase`
(`operator delete`).
