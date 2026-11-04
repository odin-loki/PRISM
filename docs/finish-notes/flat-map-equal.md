# flat_map equality

Branch `port/flat-map-equal`, parent `03cf1640e` (`main` at this commit).
This slice ports `operator==` from `39754a2e3` (branch
`finish/cxx-flat-map-equal`) onto the current header, which has since
gained `swap`, `operator<` and `operator>`. The old body walked the two
node chains side by side. That was wrong: the chain is in link order, not
key order, so two maps holding the same pairs inserted in different orders
compared unequal. The body that landed looks each pair up by key instead.

`operator==` for two `std::flat_map` objects of the same type lives on the
existing header `src/prism/pir/models/cxx/flat_map`. That path is still a
file. A directory of the same name would make `#include <flat_map>` fail
with EISDIR.

Nothing here is PROVED. Nothing here is CLEAN. Two maps comparing equal is
the result this header defines. PIR was not run. The solver was not run.

## What equality does

`operator==` is container equality ([flat.map.defn]: same size and the
same pairs in key order). It is not iterator equality. It is a friend on
`flat_map`, after `erase(iterator)` and before `operator<`.

```cpp
friend bool operator==(const flat_map& __x, const flat_map& __y) {
    if (__x._M_head == nullptr && __y._M_head == nullptr)
        return true;
    if (__x._M_size != __y._M_size)
        return false;
    for (const _Node* __a = __x._M_head; __a != nullptr; __a = __a->_M_next) {
        const _Node* __b = __y._M_find(__a->_M_key);
        if (__b == nullptr)
            return false;
        if (!(__a->_M_key == __b->_M_key && __a->_M_mapped == __b->_M_mapped))
            return false;
    }
    return true;
}
```

- Two empty maps compare equal. That return is before any node is loaded.
- Different sizes compare unequal before any node is loaded, so the shorter
  map is not read past.
- Otherwise each node of `__x` is looked up in `__y` with the existing
  `_M_find` (equivalence under `_Compare`). A missing key returns false.
  The stored keys and the stored mapped values are then compared with
  `==`, as the standard does. A key mismatch does not evaluate the mapped
  `==`. A mismatch returns false.
- Why this is element-wise equality in key order: keys are unique on both
  sides, so distinct keys of `__x` find distinct nodes of `__y`. Same size
  plus every key found is a one-to-one pairing of equivalent keys, and
  equivalent keys sit at the same position in sorted order. So the pairing
  is exactly the sorted-position pairing `std::equal` would use. Insertion
  order does not matter. The cost is O(n^2) key compares, the same order
  as this model's other lookups. Nothing is sorted and nothing is
  allocated.
- No key or mapped value is constructed and `T{}` is not returned.

`operator!=` is not declared. In C++20 `a != b` is rewritten to
`!(a == b)`, which is what [flat.map.defn] relies on (it declares only
`==` and `<=>`). The header's "Declared besides the above" list now names
`operator==`, says `a != b` is its rewrite, and says `<=`, `>=` and `<=>`
are not declared. Before this slice the list said container equality was
not declared.

`operator<` and `operator>` walked the node chain, newest pair first, not
key order. That was a separate defect. It is fixed in the next commit on
this branch (`flat-map-less.md` "Key order").

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The `operator>` case used to check that container
`operator==` and `operator!=` are absent. It now checks that
`operator==(const flat_map&` is declared once, before `operator<`, and
that `operator!=(const flat_map&` is still absent. The new case checks the
`operator==` body: the empty return is before any `->`, `_M_key`,
`_M_mapped`, `_M_next` or `_M_find`; the size check returns false before
those loads; the walk is over `__x` only, each key goes through
`__y._M_find`, a null result returns false, then `_M_key ==` and
`_M_mapped ==`, then false on a mismatch, then `return true`. It checks
that the body never steps `__b = __b->_M_next` and never starts a walk at
`__y._M_head`, so nodes are not paired by position. The body does not
name `key_type`, `mapped_type`, `new`, `operator[]`, `at(`, `erase`,
`insert` or `operator<`. The comment above it says the chain is link
order, not key order, that the same pairs inserted in a different order
compare equal, that `operator!=` is not declared, and that this is not
PROVED and not CLEAN.

`tests/cxx_models/flat_map_trace.cpp` has an `equal` mode. It was built
with clang++ from `~/opt/llvm23`, `-std=c++23 -Wall -Wextra -Werror
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, with a directory holding only this
header as the `<flat_map>` on the include path. It printed

```text
empty 1 ne 0
short 0 ne 1 rev 0
one 1 ne 0
miss 0 ne 1
same 1 ne 0
order 1 ne 0 rev 1
keydiff 0 ne 1
three 1 ne 0
longer 0 ne 1
erased 1 ne 0
kempty 1 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
kshort 0 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
kshortne 1 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
korder 1 key_eq=2 mapped_eq=2 key_lt=5 copies=0 moves=0 mapped=0
korderne 0 key_eq=2 mapped_eq=2 key_lt=5 copies=0 moves=0 mapped=0
kstop 0 key_eq=1 mapped_eq=1 key_lt=7 copies=0 moves=0 mapped=0
mstop 0 key_eq=2 mapped_eq=2 key_lt=6 copies=0 moves=0 mapped=0
```

`order` is `{1 -> 10, 2 -> 20}` against the same pairs inserted 2 then 1:
equal both ways. The old side-by-side walk printed `order 0` here.
`keydiff` is the same size with a different key. `three` is an
initializer-list map against the same pairs inserted in another order.
`erased` removes the extra pair from `longer` and the maps are equal
again. The `k` lines count key and mapped operations after the maps are
built. Empty and different-size comparisons make no key or mapped call.
`korder` is equal with two key `==` and two mapped `==`; the key `<`
calls are `_M_find`. `kstop` is `(1,10) (2,20) (3,30)` against
`(1,10) (8,80) (3,30)`: key 3 is found and equal, key 2 is not found, so
it returns false without comparing key 1. `mstop` has the middle mapped
value 20 against 99: false at the second pair. Nothing is constructed on
any comparison. AddressSanitizer and UBSan reported nothing in `equal`.
The `less`, `greater`, `swap`, `empty`, `count`, `contains`,
`try_emplace`, `miss` and default modes still print their recorded lines,
and `uaf` still aborts with `heap-use-after-free`. libstdc++ 14 on this
machine has no `<flat_map>`, so the driver was not built against it and
there is no differential.

## Still in front of the model

`src/prism/syntax_gates.cpp` still reports `C++ flat_map unencoded` from the source
text before a header is inlined. Until that gate is gone, a flat_map task
does not reach this header. `docs/PIR.md` was not edited.
