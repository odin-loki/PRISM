# flat_map equality

Branch `port/flat-map-equal`, parent `03cf1640e` (`main` at that point).
This slice ports `operator==` from `39754a2e3` (branch
`finish/cxx-flat-map-equal`) onto the current header, which has since
gained `swap`, `operator<` and `operator>`.

The body went through three versions. The original walked the two node
chains side by side. That was wrong because the chain is in link order,
not key order: two maps holding the same pairs inserted in different
orders compared unequal. The first port looked each pair of `__x` up in
`__y` with `_M_find`. That was wrong too. `_M_find` matches by `_Compare`
equivalence, and the standard compares with `==`. When equivalence is
finer than `==`, the results differ. With a comparator `Sgn` that orders
by sign bit and then `<`, `flat_map<double, int, Sgn>` `{{-0.0, 1}}`
against `{{0.0, 1}}` is equal in the standard (libc++ 23), and the lookup
version said unequal. The body that landed walks both maps in key order
and compares position by position with `==`.

`operator==` for two `std::flat_map` objects of the same type lives on the
existing header `src/prism/pir/models/cxx/flat_map`. That path is still a
file. A directory of the same name would make `#include <flat_map>` fail
with EISDIR.

Nothing here is PROVED. Nothing here is CLEAN. Two maps comparing equal is
the result this header defines. PIR was not run. The solver was not run.

## What equality does

`operator==` is container equality ([flat.map.defn]: same size, then
`std::equal` over the two ranges in key order, comparing `value_type` with
`==`). It is not iterator equality. It is a friend on `flat_map`, after
`erase(iterator)` and before `operator<`.

```cpp
friend bool operator==(const flat_map& __x, const flat_map& __y) {
    if (__x._M_head == nullptr && __y._M_head == nullptr)
        return true;
    if (__x._M_size != __y._M_size)
        return false;
    const _Node* __a = _M_key_next(__x._M_head, nullptr);
    const _Node* __b = _M_key_next(__y._M_head, nullptr);
    while (__a != nullptr && __b != nullptr) {
        if (!(__a->_M_key == __b->_M_key && __a->_M_mapped == __b->_M_mapped))
            return false;
        __a = _M_key_next(__x._M_head, __a);
        __b = _M_key_next(__y._M_head, __b);
    }
    return __a == nullptr && __b == nullptr;
}
```

- Two empty maps compare equal. That return is before any node is loaded.
- Different sizes compare unequal before any node is loaded.
- Otherwise both maps are walked in key order. `_M_key_next(head, cur)` is
  the private helper `operator<` uses (`flat-map-less.md`, "Key order"):
  one scan of the chain for the smallest key above `cur` under `_Compare`,
  or the smallest key when `cur` is null. The pairs are matched by
  position, the k-th smallest of `__x` with the k-th smallest of `__y`,
  as `std::equal` over the sorted ranges does.
- Each pair is compared with key `==` and mapped `==`. A key mismatch does
  not evaluate the mapped `==`. A mismatch returns false before the next
  pair is found. There is no lookup and no `_Compare` equivalence test in
  the body, so keys that are `==` but not equivalent still match, and keys
  that are equivalent but not `==` do not.
- The loop needs both nodes, so it does not read past either map. Sizes
  are already equal, so both walks run out together on success.
- Insertion order does not matter. The cost is O(n^2) `_Compare` calls.
  Nothing is sorted, nothing is allocated, and no key or mapped value is
  constructed.

`operator!=` is not declared. In C++20 `a != b` is rewritten to
`!(a == b)`, which is what [flat.map.defn] relies on (it declares only
`==` and `<=>`). The header's "Declared besides the above" list now names
`operator==`, says `a != b` is its rewrite, and says `<=`, `>=` and `<=>`
are not declared. Before this slice the list said container equality was
not declared.

`operator<` and `operator>` used to walk the node chain, newest pair
first, not key order. That was a separate defect, fixed in a separate
commit on this branch (`flat-map-less.md` "Key order"). The same review
also restored the words "not load" in the file comment for `operator>`,
where a reflow had dropped them ("that path does not load the empty
map").

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The `operator>` case used to check that container
`operator==` and `operator!=` are absent. It now checks that
`operator==(const flat_map&` is declared once, before `operator<`, and
that `operator!=(const flat_map&` is still absent.

The equality case checks the `operator==` body:

- The empty return comes before any `->`, `_M_key`, `_M_mapped`, `_M_next` or
  `_M_key_next`.
- The size check returns false before those loads.
- Both walks start with `_M_key_next(..., nullptr)` and step with
  `_M_key_next(head, cur)`. `_M_key ==` and then `_M_mapped ==` come
  before the one mismatch `return false`, and the success result is
  `return __a == nullptr && __b == nullptr`.
- The body has no `_M_find`, no `_Compare`, no `->_M_next`, and no walk
  that starts at a raw `_M_head`. It does not name `key_type`,
  `mapped_type`, `new`, `operator[]`, `at(`, `erase`, `insert` or
  `operator<`.
- The comment above it says both maps are walked in key order and paired
  position by position, with `==` and not by `_Compare` equivalence. It
  says that `==`-but-not-equivalent keys match, that `operator!=` is not
  declared, and that this is not PROVED and not CLEAN.
- The test also checks that the file comment has the whole `operator>`
  sentence.

`tests/cxx_models/flat_map_trace.cpp` has an `equal` mode. It was built
with clang++ from `~/opt/llvm23`, `-std=c++23 -Wall -Wextra -Werror
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, with only the model headers on the
include path and a `__prism_check` stub that prints failures. It printed

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
sgn 1 ne 0 rev 1
sgn2 1 ne 0
abs 0 ne 1
kempty 1 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
kshort 0 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
kshortne 1 key_eq=0 mapped_eq=0 key_lt=0 copies=0 moves=0 mapped=0
korder 1 key_eq=2 mapped_eq=2 key_lt=10 copies=0 moves=0 mapped=0
korderne 0 key_eq=2 mapped_eq=2 key_lt=10 copies=0 moves=0 mapped=0
kstop 0 key_eq=2 mapped_eq=1 key_lt=12 copies=0 moves=0 mapped=0
mstop 0 key_eq=2 mapped_eq=2 key_lt=12 copies=0 moves=0 mapped=0
```

- `order` is `{1 -> 10, 2 -> 20}` against the same pairs inserted 2 then
  1: equal both ways. The side-by-side link walk printed `order 0` here.
- `keydiff` is the same size with a different key. `three` is an
  initializer-list map against the same pairs inserted in another order.
  `erased` removes the extra pair from `longer`, and the maps are equal
  again.
- `sgn` is the review's failing input: `flat_map<double, int, Sgn>`
  `{{-0.0, 1}}` against `{{0.0, 1}}`. The two keys are not equivalent under
  `Sgn`, but `-0.0 == 0.0`, so the maps are equal, as in the standard. The
  lookup version printed `sgn 0 ne 1 rev 0`.
- `sgn2` adds a second pair on each side, built in different orders.
- `abs` uses a comparator by magnitude: `-1` and `1` are equivalent but not
  `==`, so `{{-1, 1}}` and `{{1, 1}}` are unequal.
- The `k` lines count key and mapped operations after the maps are built.
  Empty and different-size comparisons make no key or mapped call and no
  `<` call.
- `korder` is equal with two key `==` and two mapped `==` calls. The key
  `<` calls are the `_M_key_next` scans.
- `kstop` is `(1,10) (2,20) (3,30)` against `(1,10) (8,80) (3,30)`. In key
  order 1 matches, then 2 against 3 differs, so it returns false without
  comparing the third position or the second mapped value.
- `mstop` has the middle mapped value 20 against 99: false at the second
  pair.
- Nothing is constructed on any comparison. ASan and UBSan reported
  nothing.

The `less`, `greater`, `swap`, `empty`, `count`, `contains`,
`try_emplace`, `miss` and default modes return 0 with no sanitizer report,
and `uaf` still aborts with `heap-use-after-free`. libstdc++ 14 on this
machine has no `<flat_map>`, so the driver was not built against it and
there is no differential. The libc++ result quoted for `sgn` comes from
the review, not from a run here.

## Still in front of the model

`src/prism/syntax_gates.cpp` still reports `C++ flat_map unencoded` from
the source text before a header is inlined. Until that gate is gone, a
flat_map task does not reach this header. `docs/PIR.md` was not edited.
