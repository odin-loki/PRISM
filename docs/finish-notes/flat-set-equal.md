# flat_set equality

Branch `finish/cxx-flat-set-equal`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`operator==` and `operator!=` for two `std::flat_set` objects of the same
type live on the existing header `src/prism/pir/models/cxx/flat_set`. That
path is a file. A directory of the same name would make `#include <flat_set>`
fail with EISDIR. The include guard is `_GLIBCXX_FLAT_SET`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`. Two empty sets comparing equal is the
result this header defines. It is not a proof that two containers this
model does not cover are equal, and it is not a fuzzer `CLEAN` result.

## What equality does

`operator==` is container equality. It is not the iterator equality already
on this header. Iterator `operator==` is still the two node pointers.

Two empty sets compare equal. An empty set has no node (`_M_head` is
nullptr). That path returns true before any node is loaded, so it does not
dereference a node.

Different sizes compare unequal. That return is the size check. It happens
before the walk, so the longer set's extra nodes are not read. The walk
itself stops when either node is missing (`__a != nullptr && __b != nullptr`),
so it does not read past the shorter set. If one side still has a node when
the other does not, the result is false.

Same size compares the stored keys with key equality, `__a->_M_key == __b->_M_key`.
The keys already in the nodes are the operands. The body does not construct
a new key. A mismatch returns false before the next node is loaded. The
comparison does not call the comparator, and it does not call `insert`,
`erase`, `find`, `contains`, `count`, `empty`, `clear`, or `swap`.

`operator!=` is the negation, `!(__x == __y)`. It does not walk the nodes
again.

## What stayed

`insert` of an equivalent key still returns the existing node before
`new _Node` and before `++_M_size`, and that prefix does not assign
`_M_key`. `erase` of a missing key still returns 0 before `delete`.
`find` is still `_M_find_node`. `contains` still returns false for a
missing key and true for a present key, and that body does not allocate.
`count` is still `contains` ? 1 : 0. `empty()` is still
`_M_head == nullptr`. `clear` still erases the head until no node remains.
`swap` is still the member that swaps the head, tail, size, and comparator.
`end()` is still

`iterator end() const noexcept { return iterator(nullptr); }`.

`operator*` and `operator->` still call `__prism_check` on a null node,
class `PTR-NULL-DEREF`, message `flat_set iterator dereference at end`.

The header does not include `<set>`, `<vector>`, `<memory>`, `<algorithm>`,
`<flat_map>`, or `<initializer_list>`.

## Tests

`tests/cpp/test_cxx_flat_set.cpp` reads the header. The new case checks
container `operator==`: the empty return is before any `->`, `_M_key`, or
`_M_next`; the size mismatch returns false before those loads; the walk
requires both nodes and compares `_M_key` with `==`; a mismatch returns
false before `_M_next`; both sides exhausted is the success result after
the walk. The body does not name `key_type` or `value_type` and has no
`new`. `operator!=` is `!(__x == __y)` and does not touch `_M_`. The
comment above `operator==` says this is not PROVED and not CLEAN. Iterator
`operator==` is still `return __a._M_node == __b._M_node`. The same file's
earlier case still checks the duplicate-insert prefix, `contains`, and the
missing-key `erase` return of 0.

`CMakeLists.txt` was not edited. It already lists
`tests/cpp/test_cxx_flat_set.cpp`, and this slice does not delete that
line. The conformance header array in `tests/cpp/test_qa_conformance.cpp`
is unchanged. `flat_set` stays in that list.

`tests/cxx_models/flat_set_trace.cpp` is unchanged. Its `empty_eq` line is
`begin() == end()`, which is iterator equality, not container equality.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Werror
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, this header ahead of the system
includes. A separate translation unit, not a tracked test, printed:

```
empty 1 ne 0
short 0 ne 1
one 1 ne 0
miss 0 ne 1
order 1 ne 0
longer 0 ne 1
kempty 1 ne 0 eqcalls 0 ctor 0 copy 0 move 0
ksame 1 ne 0 eqcalls 4 ctor 0 copy 0 move 0
kmismatch 0 eqcalls 2 ctor 0 copy 0 move 0
kstop 0 eqcalls 2 ctor 0 copy 0 move 0
kshort 0 eqcalls 0 ctor 0 copy 0 move 0
```

`empty` is two empty `flat_set<int>` values: equal, and `!=` is false.
`short` is one key against an empty set. `one` is two sets that each hold
1. `miss` is `{1}` against `{2}`. `order` is `{1, 2}` inserted in opposite
orders. `longer` is that set against a three-key set. The `k` lines use a
key type that counts constructors and `operator==`. `kempty` compares two
empty sets with zero key-equality calls and zero constructions. `ksame`
is equal; `eqcalls` is 4 because `==` and `!=` each compare the two stored
keys, and neither call constructs a key. `kmismatch` is `{1, 2}` against
`{1, 9}` and compares both keys. `kstop` is `{1, 2, 3}` against
`{1, 8, 3}`: the second key differs, the third key is not compared, and
nothing is constructed. `kshort` is two keys against one: unequal, with
zero key-equality calls and zero constructions. That run is not PROVED
and not CLEAN. libstdc++ on this machine has no `<flat_set>` header, so
the same driver was not built against it.

## Still in front of the model

`src/prism/bmc.cpp` still reports `C++ flat_set unencoded` from the source
text before a header is inlined. `src/prism/pir/stage.cpp` was not edited.
Until that gate is gone, a flat_set task does not reach this header.
`docs/PIR.md` was not edited.
