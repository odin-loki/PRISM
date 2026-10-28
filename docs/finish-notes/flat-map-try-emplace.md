# flat_map try_emplace

Branch `finish/cxx-flat-map-try-emplace`, base `020c4b6ffaf889df75f6fe7beb103349d38fbfaf`.

`std::flat_map::try_emplace` is declared on the existing header
`src/prism/pir/models/cxx/flat_map`. The header is still a file, not a
directory. `operator[]`, `at()`, `erase()`, and `contains()` are the same
functions as before this slice.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.

## What try_emplace does

Two overloads, `try_emplace(const key_type&, args...)` and
`try_emplace(key_type&&, args...)`. This header has no `insert` and no
iterators, so the result is `bool`: false when the key was already
present, true when a node was inserted.

A present key returns false before any allocation. `_M_find` walks the
existing nodes. That path does not call `::operator new`, `_M_emplace_args`,
or `_M_link`, and it does not assign the stored mapped object. The
caller's arguments stay arguments: the mapped type is not constructed
on this path.

A missing key calls `_M_emplace_args`, which allocates one node and
constructs the mapped member from the argument pack inside that node
(`_M_mapped(static_cast<_Args&&>(__args)...)`). The key is copied from an
lvalue, or moved from an rvalue, only on this path. `_M_link` then links
the node. If that constructor throws, the allocation is released with
`::operator delete` and the node is not linked.

```cpp
template <typename... _Args>
bool try_emplace(const key_type& __k, _Args&&... __args) {
    if (_M_find(__k) != nullptr) return false;
    _M_link(_M_emplace_args(__k, static_cast<_Args&&>(__args)...));
    return true;
}
```

The rvalue-key overload is the same lookup, then it moves the key into
the new node.

`operator[]` on a missing key still inserts a node whose mapped object is
`mapped_type{}`. `at()` on a missing key still throws `std::out_of_range`
(`flat_map::at: key is not present`) and does not return a default `T`.
`erase` of a present key still destroys that node and frees it with
`::operator delete`. `erase` of a missing key still returns 0 and frees
nothing. `contains` of a missing key still returns false and does not
allocate.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed: insert,
`insert_range`, `emplace`, `try_emplace` with an iterator hint,
`erase(iterator)`, iterators, `find`, `count`, `clear`, `swap`, and the
`Compare` / `KeyContainer` / `MappedContainer` parameters. A call that
expects `pair<iterator, bool>` (`.second`, or a structured binding) does
not match this `bool` and falls back the same way. The header does not
include `<set>`, `<vector>`, or `<memory>`.

`src/prism/pir/stage.cpp`, `src/prism/bmc.cpp`, and `docs/PIR.md` were not
edited. `CMakeLists.txt` was not edited. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited.
`testdata/conformance/libc-models/flat_map_contracts.cpp` was not edited.
Its harnesses still use `operator[]`, `at()`, and `erase()`.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The omitted-name list used to name `try_emplace`.
It now keeps `find, count, clear`, and the test rejects
`insert ranges, try_emplace,` and `try_emplace, erase(iterator)`.

Both `try_emplace` overloads contain
`if (_M_find(__k) != nullptr) return false;` before `_M_emplace_args`.
Those bodies do not mention `mapped_type{}`, `mapped_type(`, `::operator new`,
or `_M_mapped`. The comment above the const-key overload states the
present-key result: return false, do not allocate, do not overwrite, and
do not construct until the key is known to be absent. `_M_emplace_args`
allocates with `::operator new` and builds
`_Node{nullptr, static_cast<_Uk&&>(__k), static_cast<_Args&&>(__args)...}`.
The node constructor initializes `_M_mapped` from that pack.
`operator[]` still links a node built with `mapped_type{}`. `at()` still
throws and does not return a default mapped value. `erase` still has one
`::operator delete`, only on the found path, then `return 0`. `contains`
is still the const `_M_find` result.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. The
`try_emplace` mode counts key copies and moves, and mapped constructions,
copies, moves, and assignments. clang++ 18.1.3,
`-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined`,
with a directory that contains only this header as the `<flat_map>` on
the include path. The `try_emplace` mode printed

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

An lvalue key already stored returned false: one key copy from the
insert, no further key copy or move, one in-place `CountMapped(int)`, no
mapped copy, move, or assignment, and `at()` stayed 20. An rvalue insert
moved the key once and constructed the mapped object in place (`value=7`);
the duplicate returned false and did not move the key or construct another
mapped object. `try_emplace(key)` value-initialized one mapped object
(`value=-1`); a later `try_emplace(key, 5)` left that value in place.
`operator[]` on a missing key still default-constructed one mapped object
(`defaults=1`) and copied the key once. A throwing mapped constructor left
`size=0` and the retry inserted the value. On a plain `flat_map<int, int>`,
`try_emplace` of a present key left `at()` at 9, `erase` of a missing key
returned 0, and `at()` of a missing key threw.

`contains` still printed

`empty contains=0 copies=0 size=0 inserted=1 hit=1 missing=0 copies=0 size=1 at=8`

`miss` printed `threw`. The default mode printed `erase present=1 missing=0`.
`uaf` aborted with `heap-use-after-free` in `flat_map::erase`
(`operator delete`).
