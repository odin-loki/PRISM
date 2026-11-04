# flat_set greater

Branch `port/wip-c`, parent `0a9571725` (`main` at this commit). This slice
ports an uncommitted `operator>` from base `befa4456e` onto the current
header. The header, the doctest, and the trace hunks were there. This note
was not, and the trace had not been run.

`operator>` for two `std::flat_set` objects of the same type lives on the
existing header `src/prism/pir/models/cxx/flat_set`. That path is still a
file. A directory of the same name would make `#include <flat_set>` fail
with EISDIR.

Nothing here is PROVED. Nothing here is CLEAN. A set comparing greater is
the result this header defines. It is not a proof about containers this
model does not cover, and it is not a fuzzer CLEAN result. PIR was not run.
The solver was not run.

## What greater does

`operator>` is a friend on `flat_set`, after `operator<`. Its body is one
line:

```cpp
friend bool operator>(const flat_set& __x, const flat_set& __y) { return __y < __x; }
```

It does not name `_M_head`, `_M_size`, `_M_key`, or `_M_next`. Every load is
in the existing `operator<`.

- Two empty sets compare not-greater. `operator<` of two empty sets returns
  false before a node is loaded.
- A non-empty set is greater than an empty set. `operator<` with the empty
  set on the left returns true before either set's node is loaded.
- An empty set is not greater than a non-empty set. `operator<` with the
  empty set on the right returns false before a node is loaded.
- Otherwise the stored keys are compared in link order with key
  `operator<`, with the arguments swapped. Key `operator>` is not called.
  The walk does not read past the shorter set.

It does not construct a key and it does not return `T{}`.

C++23 `std::flat_set` declares `operator==` and `operator<=>`. `a > b`
against libstdc++ is the rewritten `(a <=> b) > 0`, and the synthesised
three-way compare uses key `operator<` when the key has no `<=>`. This
header calls key `operator<` too.

## What still falls through to libstdc++

`operator<=`, `operator>=`, and `operator<=>` are not declared. A use fails
to compile against this header and the unit falls back to libstdc++. They
are not stubbed. The other omissions in the header comment are unchanged.

## Tests

`tests/cpp/test_cxx_flat_set.cpp` reads the header without a compiler. The
less test now expects one `operator>(const flat_set&`. A new test checks
that the body is `return __y < __x;`, that it names no node field, no
`T{}`, no `return true` or `return false`, and no other member function.
It checks that `operator<` still returns on the two empty-set paths before
any `->` or `_M_key`, and that the comment above `operator>` says it does
not load a node, does not load the empty set, and is not PROVED and not
CLEAN.

`tests/cxx_models/flat_set_trace.cpp` prints `gt`, `kgt`, and `ord` lines
after the less lines. That driver is not run by `prism_tests`. It was run
by hand with clang++ 23 against this header, under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1 -w`. ASan stayed quiet and it returned 0.

- `gt empty 0 nonempty 1 rev 0 same 0 mid 1 lt 0 longer 1 prefix 0 one 1
  self 0 const 0`.
- `kgt` two empty sets: 0, no key `<` calls, no key constructed. A
  non-empty set against an empty set: 1, no key `<` calls. The reverse: 0,
  no calls. The poison key 100 is never read (`ppoison`, `lpoison`,
  `mpoison`, `hpoison`, and `tpoison` are 0).
- `ord` every `gt` count is 0, so key `operator>` was not called.

The libstdc++ on this machine (`__GLIBCXX__ 20240908`, GCC 14) has no
`<flat_set>`, so the trace was not compiled against it. That is not a
differential result.
