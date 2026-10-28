# flat_map count

Branch `finish/cxx-flat-map-count`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`std::flat_map::count` is declared on the existing header
`src/prism/pir/models/cxx/flat_map`. The header is still a file, not a
directory. `operator[]`, `at()`, `erase()`, `contains()`, and `try_emplace()`
are the same functions as before this slice.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.

## What count does

`size_type count(const key_type&) const` calls the existing const `_M_find`
and returns 1 when that node is non-null, 0 when it is null.

```cpp
size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }
```

A missing key, including on an empty container, returns 0. `_M_find`
walks the existing nodes and returns nullptr. That path does not call
`::operator new`, `_M_make`, `_M_emplace_args`, or `_M_link`, and it does
not construct a mapped value. A present key returns 1. That path is the
same lookup. It does not allocate, does not insert, and does not overwrite
the stored mapped object.

`operator[]` on a missing key still inserts a node whose mapped object is
`mapped_type{}`. `at()` on a missing key still throws `std::out_of_range`
(`flat_map::at: key is not present`) and does not return a default `T`.
`erase` of a present key still destroys that node and frees it with
`::operator delete`. `erase` of a missing key still returns 0 and frees
nothing. `contains` of a missing key still returns false and does not
allocate. `try_emplace` on a present key still returns false before any
allocation and does not overwrite the stored mapped object. On a missing
key it still constructs the mapped object from the arguments only after
the key is known to be absent.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed: insert,
`insert_range`, `emplace`, `try_emplace` with an iterator hint,
`erase(iterator)`, iterators, `find`, `clear`, `swap`, and the
`Compare` / `KeyContainer` / `MappedContainer` parameters. A call that
expects `pair<iterator, bool>` (`.second`, or a structured binding) does
not match `try_emplace`'s `bool` and falls back the same way. The header
does not include `<set>`, `<vector>`, or `<memory>`.

`src/prism/pir/stage.cpp`, `src/prism/bmc.cpp`, and `docs/PIR.md` were not
edited. `CMakeLists.txt` was not edited. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited.
`testdata/conformance/libc-models/flat_map_contracts.cpp` was not edited.
Its harnesses still use `operator[]`, `at()`, and `erase()`.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The omitted-name list used to say
`find, count, clear`. It now says `find, clear`, and the test requires
`count` on the class:

`size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }`

The text from that signature until `try_emplace` does not mention
`::operator new`, `_M_make`, `_M_link`, `_M_emplace_args`, `_M_size`,
`mapped_type{}`, `mapped_type(`, or `_M_mapped`. The comment above `count`
states that a missing key returns 0 and does not construct a mapped value,
a present key returns 1, and the lookup does not allocate, insert, or
overwrite the mapped object. `operator[]` still links a node built with
`mapped_type{}`. `at()` still throws and does not return a default mapped
value. `erase` still has one `::operator delete`, only on the found path,
then `return 0`. `contains` is still the const `_M_find` result. Both
`try_emplace` overloads still return false from `_M_find` before
`_M_emplace_args`.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. The `count`
mode counts key copies and moves, and mapped constructions, copies, moves,
and assignments. clang++ 18.1.3,
`-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined`,
with a directory that contains only this header as the `<flat_map>` on
the include path. The `count` mode printed

```text
empty count=0 key_copies=0 key_moves=0 mapped=0 size=0
index key_copies=0 key_moves=1 defaults=1 from_int=1 mapped_moves=1 assigns=1 size=1 value=8
count hit=1 missing=0 key_copies=0 key_moves=0 from_int=0 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 value=8 size=1 missing_at=threw
```

An empty container returned 0 with zero key copies, zero key moves, zero
mapped activity, and size 0. `operator[]` then inserted one node (one key
move, one default mapped object, one `CountMapped(int)` assigned over that
default) and `at()` read 8. `count` of that key returned 1 and `count` of
another key returned 0. Those two calls added no key copy or move, no
mapped construction, copy, move, or assignment, left the stored value at
8, and left size at 1. `at()` of the missing key threw.

`try_emplace` still printed

```text
lvalue inserted=1 hit=0 key_copies=1 key_moves=0 from_int=1 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 size=1 value=20
rvalue inserted=1 dup=0 key_moves=1 from_int=2 mapped_moves=0 size=2 value=7
bare inserted=1 dup=0 defaults=1 from_int=2 value=-1
index key_copies=1 defaults=1 size=4 at=4
copy size=4 key_copies=4 mapped_copies=4 value=20
throw size=0
throw retry inserted=1 size=1 value=2
plain hit=0 inserted=1 at=9 erase_missing=0 erase_present=1 missing_at=-1 size=1
```

`contains` still printed

`empty contains=0 copies=0 size=0 inserted=1 hit=1 missing=0 copies=0 size=1 at=8`

`miss` printed `threw`. The default mode printed `erase present=1 missing=0`.
`uaf` aborted with `heap-use-after-free` in `flat_map::erase`
(`operator delete`).
