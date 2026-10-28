# flat_map clear

Branch `finish/cxx-flat-map-clear`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::flat_map::clear` is declared on the existing header
`src/prism/pir/models/cxx/flat_map`. The header is still a file, not a
directory. `operator[]`, `at()`, `erase()`, `contains()`, `try_emplace()`,
`count()`, `size()`, and `empty()` are the same functions as before this
slice.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
`clear()` leaving the container empty is the result this header defines by
calling the existing `_M_clear`. It is not PROVED and not CLEAN.

## What clear does

`void clear() noexcept` calls `_M_clear()`. That function was already
there. Its body is unchanged: while the head is non-null it destroys each
node, releases that node with `::operator delete`, and then sets
`_M_size` to 0.

```cpp
void _M_clear() noexcept {
    while (_M_head != nullptr) {
        _Node* __n = _M_head;
        _M_head = __n->_M_next;
        __n->~_Node();
        ::operator delete(__n);
    }
    _M_size = 0;
}

void clear() noexcept { _M_clear(); }
```

`clear()` does not allocate, does not insert, and does not construct a
mapped value. It does not call `::operator new`, `_M_make`, `_M_link`, or
`_M_emplace_args`. An empty map has a null head, so the walk does not run
and `::operator delete` is not called. After `clear()`, `_M_size` is 0, so
`empty()` is true and `size()` is 0. The comment above `clear` says this
is not PROVED and not CLEAN.

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
after the key is known to be absent. `size()` is still `return _M_size`.
`empty()` is still `return _M_size == 0`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed: insert,
`insert_range`, `emplace`, `try_emplace` with an iterator hint,
`erase(iterator)`, iterators, `find`, `swap`, and the
`Compare` / `KeyContainer` / `MappedContainer` parameters. A call that
expects `pair<iterator, bool>` (`.second`, or a structured binding) does
not match `try_emplace`'s `bool` and falls back the same way. The header
does not include `<set>`, `<vector>`, or `<memory>`.

`src/prism/pir/stage.cpp`, `src/prism/bmc.cpp`, and `docs/PIR.md` were not
edited. `CMakeLists.txt` was not edited, and no CMake test line was
deleted. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited.
`testdata/conformance/libc-models/flat_map_contracts.cpp` was not edited.
Its harnesses still use `operator[]`, `at()`, and `erase()`.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The omitted-name list used to say
`iterators, find, clear,`. It now says `iterators, find, swap,` and does
not name `clear` or `empty`. The test requires `clear` on the class, in
the public section, after the private `_M_clear` and before `empty()`:

`void clear() noexcept { _M_clear(); }`

The text from that signature until the `empty` comment does not mention
`::operator new`, `_M_make`, `_M_link`, `_M_emplace_args`,
`::operator delete`, `mapped_type{}`, `mapped_type(`, `_M_mapped`,
`operator[]`, `at(`, `erase(`, `try_emplace`, `contains(`, or `while`. The
comment above `clear` states that it calls the existing `_M_clear`, that
it does not allocate, does not insert, and does not construct a mapped
value, that an empty map does not call `::operator delete`, and that after
`clear()`, `empty()` is true and `size()` is 0. That comment says this is
not PROVED and not CLEAN. The test also reads `_M_clear` through the
default constructor and checks the existing order: `while (_M_head !=
nullptr)`, then `__n->~_Node()`, then one `::operator delete(__n)`, then
`_M_size = 0`. That body does not allocate or construct a mapped value.
`operator[]` still links a node built with `mapped_type{}`. `at()` still
throws and does not return a default mapped value. `erase` still has one
`::operator delete`, only on the found path, then `return 0`. `contains`
is still the const `_M_find` result. `count` is still that finder as 1 or
0. Both `try_emplace` overloads still return false from `_M_find` before
`_M_emplace_args`. `size()` is still `return _M_size`. `empty()` is still
`return _M_size == 0`, and the text from that signature until the end of
the header still does not mention `::operator delete`.

`tests/cxx_models/flat_map_trace.cpp` was not edited. This slice did not
compile it.
