# flat_map empty

Branch `finish/cxx-flat-map-empty`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::flat_map::empty` is declared on the existing header
`src/prism/pir/models/cxx/flat_map`. The header is still a file, not a
directory. `operator[]`, `at()`, `erase()`, `contains()`, `try_emplace()`,
`count()`, and `size()` are the same functions as before this slice.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
`empty()` being true is the result this header defines from its node count.
It is not PROVED and not CLEAN.

## What empty does

`bool empty() const noexcept` returns `_M_size == 0`. `_M_size` is the
existing node count that `size()` already returns. A default-constructed
map has `_M_size == 0`, so `empty()` is true. After a node is linked,
`_M_size` is at least 1, so `empty()` is false. After the last node is
erased, `_M_size` is 0 again, so `empty()` is true.

```cpp
bool empty() const noexcept { return _M_size == 0; }
```

The body reads that count. It does not call `::operator new`, `_M_make`,
`_M_link`, `_M_emplace_args`, or `::operator delete`. It does not construct
a mapped value. It does not call `operator[]` or `at()`. The comment above
the function says this is not PROVED and not CLEAN.

`operator[]` on a missing key still inserts a node whose mapped object is
`mapped_type{}`. `at()` on a missing key still throws `std::out_of_range`
(`flat_map::at: key is not present`) and does not return a default `T`.
`erase` of a present key still destroys that node and frees it with
`::operator delete`. `erase` of a missing key still returns 0 and frees
nothing. `contains` of a missing key still returns false and does not
allocate. `count` of a missing key still returns 0 and does not construct
a mapped value. `try_emplace` on a present key still returns false before
any allocation and does not overwrite the stored mapped object. On a
missing key it still constructs the mapped object from the arguments only
after the key is known to be absent. `size()` is still
`return _M_size`.

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
and it does not run pir. The omitted-name list still says
`iterators, find, clear,` and does not name `empty`. The test requires
`empty` on the class:

`bool empty() const noexcept { return _M_size == 0; }`

The text from that signature until the end of the header does not mention
`::operator new`, `_M_make`, `_M_link`, `_M_emplace_args`,
`::operator delete`, `mapped_type{}`, `mapped_type(`, `_M_mapped`,
`operator[]`, `at(`, `erase(`, `try_emplace`, or `contains(`. The comment
above `empty` states that no nodes is true, at least one node is false,
the body uses the existing size, and it does not allocate, insert, erase,
or construct a mapped value, and it does not call `operator[]` or `at()`.
That comment says this is not PROVED and not CLEAN. `operator[]` still
links a node built with `mapped_type{}`. `at()` still throws and does not
return a default mapped value. `erase` still has one `::operator delete`,
only on the found path, then `return 0`. `contains` is still the const
`_M_find` result. `count` is still that finder as 1 or 0. Both
`try_emplace` overloads still return false from `_M_find` before
`_M_emplace_args`. `size()` is still `return _M_size`.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. The `empty`
mode counts key copies and moves, and mapped constructions, copies, moves,
and assignments around the `empty()` calls. clang++ 18.1.3,
`-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined`,
with a directory that contains only this header as the `<flat_map>` on
the include path. The `empty` mode printed

```text
none empty=1 key_copies=0 key_moves=0 mapped=0 size=0
some empty=0 key_copies=0 key_moves=0 from_int=0 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 size=1 value=8
erased empty=1 erased=1 key_copies=0 key_moves=0 mapped=0 size=0 missing_at=threw
plain none=1 some=0 again=1 size=0
```

An empty container returned true with zero key copies, zero key moves,
zero mapped activity, and size 0. `operator[]` then inserted one node.
`empty()` of that container returned false and added no key copy or move,
no mapped construction, copy, move, or assignment, left the stored value
at 8, and left size at 1. `erase` of that key returned 1. `empty()` then
returned true with no further key or mapped activity and size 0. `at()` of
the missing key threw. A `flat_map<int, int>` was empty, then not empty
after one insert, then empty again after `erase`, with size 0. g++ 13.3.0
compiled the same trace without the sanitizers and printed the same
`empty` lines. That run is not PROVED and not CLEAN.

`count` still printed

```text
empty count=0 key_copies=0 key_moves=0 mapped=0 size=0
index key_copies=0 key_moves=1 defaults=1 from_int=1 mapped_moves=1 assigns=1 size=1 value=8
count hit=1 missing=0 key_copies=0 key_moves=0 from_int=0 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 value=8 size=1 missing_at=threw
```

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
`uaf` aborted with `heap-use-after-free`. With `-fno-inline` the free is
`flat_map::erase` (`operator delete` at the header's delete of the node).
