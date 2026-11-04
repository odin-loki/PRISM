# flat_map greater

Branch `port/wip-c`, parent `0a9571725` (`main` at this commit). This slice
ports an uncommitted `operator>` from base `befa4456e` onto the current
header. The header, the doctest, and the trace hunks were there. This note
was not, and the trace had not been run.

`operator>` for two `std::flat_map` objects of the same type lives on the
existing header `src/prism/pir/models/cxx/flat_map`. That path is still a
file. A directory of the same name would make `#include <flat_map>` fail
with EISDIR.

Nothing here is PROVED. Nothing here is CLEAN. A map comparing greater is
the result this header defines. It is not a proof about containers this
model does not cover, and it is not a fuzzer CLEAN result. PIR was not run.
The solver was not run.

## What greater does

`operator>` is a friend on `flat_map`, after `operator<`. Its body is one
line:

```cpp
friend bool operator>(const flat_map& __x, const flat_map& __y) { return __y < __x; }
```

It does not name `_M_head`, `_M_size`, `_M_key`, `_M_mapped`, or `_M_next`.
Every load is in the existing `operator<`.

- Two empty maps compare not-greater. `operator<` of two empty maps returns
  false before a node is loaded.
- A non-empty map is greater than an empty map. `operator<` with the empty
  map on the left returns true before either map's node is loaded.
- An empty map is not greater than a non-empty map. `operator<` with the
  empty map on the right returns false before a node is loaded.
- Otherwise the stored keys and mapped values are compared with `<`, with
  the arguments swapped. Key and mapped `operator>` are not called. The
  walk stops at the first deciding pair and does not read past the shorter
  map.

It does not construct a key or a mapped value and it does not return
`T{}`.

`operator<` walks the node chain from `_M_head`, and each insert links at
the head, so the first pair compared is the most recently inserted, not
the smallest key. `operator>` inherits that order. When two maps were
filled in a different key order than libstdc++'s sorted order, the result
can differ from libstdc++. The trace fills its maps so that the two orders
agree.

C++23 `std::flat_map` declares `operator==` and `operator<=>`. `a > b`
against libstdc++ is the rewritten `(a <=> b) > 0`, and the synthesised
three-way compare uses `operator<` on keys and mapped values that have no
`<=>`. This header calls `operator<` too.

## What still falls through to libstdc++

Container `operator==`, `operator!=`, `operator<=`, `operator>=`, and
`operator<=>` are not declared. A use fails to compile against this header
and the unit falls back to libstdc++. They are not stubbed. The header
comment now names `operator>` among the declared names and says that
container equality and the other relational operators are not declared.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header without a compiler. The
less test no longer requires that `operator>(const flat_map&` is absent,
and its `operator<` slice now ends at the comment above `operator>`. A new
test checks that `operator>` is declared once, after `swap` and
`operator<`, and that its body is `return __y < __x;` with no node field,
no `T{}`, no `return true` or `return false`, and no other member function.
It checks that `operator<` still returns on the three empty-map paths
before any `->`, `_M_key`, `_M_mapped`, or `_M_next`, and that the comment
above `operator>` says it does not load a node, does not load the empty
map, and is not PROVED and not CLEAN. It also checks that container
`operator==` and `operator!=` are still not declared.

`tests/cxx_models/flat_map_trace.cpp greater` is not run by `prism_tests`.
It was run by hand with clang++ 23 against this header, under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1 -w`. ASan stayed quiet and it returned 0.
Every `greater` line is the `less` line with the two maps swapped:
`empty 0`, `eg 0`, `ge 1`, `oneeq 0`, `key 1 0`, `mapped 0 1`,
`prefix 0 1`, `early 1 0`, `tail 1 0`. The `CountKey` lines show no key
or mapped `<` call for `kempty`, `keg`, or `kge`. `kkey 1` makes one key
`<` call and `kkeyba 0` makes two, which are the `kkeyba` and `kkey` counts
of the less run. `poison`, `copies`, `moves`, and `mapped` are 0 on every
line. The `less` lines and the no-argument lines did not change.

The libstdc++ on this machine (`__GLIBCXX__ 20240908`, GCC 14) has no
`<flat_map>`, so the trace was not compiled against it. That is not a
differential result.
