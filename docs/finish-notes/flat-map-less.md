# flat_map less and swap

Branch `port/flat`, parent `fa63548bb` (`origin/main` at this commit). This slice
reimplements the October `operator<` and `swap` behaviour on the current
header. It does not cherry-pick.

`operator<` and `swap` for `std::flat_map` live on the existing header
`src/prism/pir/models/cxx/flat_map`. That path is a file. A directory of the
same name would make `#include <flat_map>` fail with EISDIR. The include
guard is `_PRISM_MODEL_FLAT_MAP`, and the file also defines
`_GLIBCXX_FLAT_MAP`.

The current header already had `operator[]`, `at`, `erase`, `contains`,
`count`, `try_emplace`, `size`, `empty`, `clear`, the sorted-unique and
container constructors, `extract`, `replace`, transparent lookup, iterators,
`find`, and `erase(iterator)`. It did not have `operator==`, `operator!=`,
`operator<`, or `swap`. `swap` was named in the omitted list. This slice
adds `swap` and `operator<` only. It does not add `operator==`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Ordering two flat_maps, and exchanging the node pointer and the size, are
results this header defines. They are not PROVED and not CLEAN.

## What swap does

`void swap(flat_map& __o) noexcept` exchanges `_M_head` and `_M_size`.
The current map does not store a comparator object, so there is nothing
else to exchange. The body does not allocate, free, or construct a key.
It does not call `::operator new`, `::operator delete`, `_M_make`,
`_M_link`, `_M_emplace_args`, or `_M_clear`, and it does not dereference
a node.

```cpp
void swap(flat_map& __o) noexcept {
    if (this == &__o) return;
    if (_M_head == nullptr && __o._M_head == nullptr) return;
    _Node* __h = _M_head;
    size_type __s = _M_size;
    _M_head = __o._M_head;
    _M_size = __o._M_size;
    __o._M_head = __h;
    __o._M_size = __s;
}
```

Self-swap returns before that exchange. It does not call `_M_clear`, so
it does not free the nodes. The map still owns the same head and the same
size.

Two empty maps return on the null-head check, before any `->`. An empty
map's head is null and its size is already 0, so both maps stay empty and
that path does not load a node. A map that has a node falls through and
the pointers and sizes are exchanged. The nodes stay where they were
constructed. The other map becomes the owner. Nothing is allocated or
freed, and no key is constructed.

## What operator< does

`operator<` is lexicographical order of the stored pairs along the node
chain. It is a friend of `flat_map`. It does not return `T{}`. It does
not construct a key or a mapped value. It does not read `_M_size`, so a
shorter map is not ordered by its size alone. It does not call
`operator==` (that operator is not on this header).

Two empty maps compare not-less. Both heads are null, and that arm
returns false before any node is loaded. There is no `->`, `_M_key`,
`_M_mapped`, or `_M_next` on that path.

An empty map is less than a non-empty map. The left head is null and the
right head is non-null, and that arm returns true. The other direction
returns false. Both arms return before a node pointer is copied into the
walk, so neither path loads the empty map's node.

Otherwise the walk starts at `_M_head` and follows `_M_next`. `_M_link`
puts a new node at the head, so the first pair compared is the most
recently inserted pair. The loop condition is both nodes non-null, so the
walk does not read past the shorter map. A deciding `<` returns before
`_M_next` is loaded.

Different sizes use that same walk. When the shared prefix of the chain is
equivalent, the result is true only when the left map is exhausted and the
right map still has a node:

```cpp
return __a == nullptr && __b != nullptr;
```

Same size compares stored keys and then mapped values with `<`:

```cpp
if (__a->_M_key < __b->_M_key)
    return true;
if (__b->_M_key < __a->_M_key)
    return false;
if (__a->_M_mapped < __b->_M_mapped)
    return true;
if (__b->_M_mapped < __a->_M_mapped)
    return false;
```

A key that is greater returns false before the mapped value is read.
Equivalent pairs advance to `_M_next`. When every pair is equivalent the
result is false.

The comparison does not call `operator[]`, `at`, `erase`, `contains`,
`try_emplace`, `count`, `size`, `empty`, `clear`, or `swap`.

`operator>`, `operator<=`, `operator>=`, and `operator<=>` are not
declared for `flat_map`. Iterators still have `operator->`. The header
does not include `<algorithm>` or `<memory>`.

## What stayed

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
after the key is known to be absent, and it still returns `bool`.
`size()` is still `return _M_size`. `empty()` is still
`return _M_size == 0`. `clear()` still calls the existing `_M_clear`.
Iterators still walk in key order. That walk is not what `operator<` uses.

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`): `insert`, `insert_range`,
`emplace`, and the rest. `swap` and `operator<` are no longer in that list.

`src/prism/syntax_gates.cpp` still reports `C++ flat_map unencoded`. This
slice does not edit that file, `stage.cpp`, or `docs/PIR.md`.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. The omitted-name text no longer contains `swap`
or `operator<`. The old contiguous string
`Not declared: insert, insert_range, emplace, swap` is gone.

The swap case checks that self-swap returns before any exchange or free,
that two null heads return before a node field is read, and that the
exchange then writes `_M_head` and `_M_size` both ways. The body does not
allocate, free, or name a key or a mapped value. The comment above `swap`
says this is not PROVED and not CLEAN.

The less case checks that two empty maps return false before `->`,
`_M_key`, `_M_mapped`, or `_M_next`, that an empty map is less before the
walk loads a node, and that a deciding key or mapped `<` returns before
`_M_next`. The final result is `__a == nullptr && __b != nullptr`. The
body does not read `_M_size` and does not return `T{}`. The comment above
`operator<` says this is not PROVED and not CLEAN.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. The
no-argument run, and the `miss`, `contains`, `count`, `try_emplace`, and
`empty` modes, print the same lines as before. `swap` and `less` are
extra arguments. clang++ 18.1.3,
`-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
-fno-sanitize-recover=all -g`, with this header ahead of the system
includes. libstdc++ 13.3 on this machine has no `<flat_map>` header, so
the same driver was not built against it. The no-argument lines are the
ones a libstdc++ compare would have to match; they were not edited.

`swap` printed

```text
empty a=1 b=1 asize=0 bsize=0
exch left=30 lsize=1 right1=10 right2=20 rsize=2 left_old=threw
back left1=10 left2=20 lsize=2 right=30 rsize=1
self left1=10 lsize=2 empty=0
hole leftempty=1 lsize=0 hole1=10 hsize=2
kempty a=1 b=1 copies=0 moves=0 mapped=0 eq=0
kex left=9 right=8 lsize=1 rsize=1 copies=0 moves=0 mapped=0 eq=0
kself value=9 size=1 copies=0 moves=0 mapped=0
```

Two empty maps stayed empty with size 0. After the exchange, the left map
held 30 and `at(1)` threw. Swapping back restored both maps. Self-swap
left the value 10 and size 2. Swapping with an empty map moved the nodes
across and left the original map empty. The counted maps exchanged with
zero key copies, zero key moves, zero mapped activity, and zero equality
calls. Self-swap of the counted map did not copy, move, or free. That run
is not PROVED and not CLEAN.

`less` printed

```text
empty 0
el 1
le 0
oneeq 0
key 0 1
mapped 1 0
prefix 1 0
early 0 1
tail 0 1
kempty 0 key_lt=0 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
kel 1 key_lt=0 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
kle 0 key_lt=0 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
ksame 0 key_lt=2 mapped_lt=2 poison=0 copies=0 moves=0 mapped=0
ktwo 0 key_lt=4 mapped_lt=4 poison=0 copies=0 moves=0 mapped=0
kkey 0 key_lt=2 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
kkeyba 1 key_lt=1 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
kmap 1 key_lt=2 mapped_lt=1 poison=0 copies=0 moves=0 mapped=0
kpre 1 key_lt=2 mapped_lt=2 poison=0 copies=0 moves=0 mapped=0
krev 0 key_lt=2 mapped_lt=2 poison=0 copies=0 moves=0 mapped=0
kearly 0 key_lt=2 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
kearlyba 1 key_lt=1 mapped_lt=0 poison=0 copies=0 moves=0 mapped=0
```

`empty` is two empty maps: not less, and the counted line has zero `<`
calls. `el` is an empty map less than `{1 -> 10}` with zero `<` calls.
`le` is the other direction. `prefix` is a one-element chain that matches
the head of a longer chain, so the shorter map is less and the poison pair
at the tail is not read. `kearly` stops on the head keys and does not read
the poison key. That run is not PROVED and not CLEAN.

`empty` still printed

```text
none empty=1 key_copies=0 key_moves=0 mapped=0 size=0
some empty=0 key_copies=0 key_moves=0 from_int=0 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 size=1 value=8
erased empty=1 erased=1 key_copies=0 key_moves=0 mapped=0 size=0 missing_at=threw
plain none=1 some=0 again=1 size=0
```

`count` still printed

```text
empty count=0 key_copies=0 key_moves=0 mapped=0 size=0
index key_copies=0 key_moves=1 defaults=1 from_int=1 mapped_moves=1 assigns=1 size=1 value=8
count hit=1 missing=0 key_copies=0 key_moves=0 from_int=0 defaults=0 mapped_copies=0 mapped_moves=0 assigns=0 value=8 size=1 missing_at=threw
```
