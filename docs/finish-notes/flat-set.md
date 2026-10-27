# flat_set slice

Branch `finish/cxx-flat-set`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<flat_set>` is a file, `src/prism/pir/models/cxx/flat_set`. A directory of
that name would make `#include <flat_set>` fail with EISDIR. Include guard
`_GLIBCXX_FLAT_SET`, so libstdc++'s `<flat_set>` is not also included. The
header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Werror`, this
header first on the include path. A driver agreed on size, `contains`,
`find` of a missing key, and `erase` of a missing key. Under ASan, `erase`
of the key a saved pointer addresses is a heap-use-after-free in `erase`.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans.

## What the model covers

`std::flat_set<Key, Compare = less<Key>>` only. Each key is stored in a
node this header allocates with one new-expression. The key is a member of
that node, not a second allocation and not a `vector`.

`insert` of a new key allocates one node and links it in compare order.
`insert` of a key that is already present (equivalent under the comparator)
returns the existing iterator. That path does not allocate another node and
does not assign over the stored key, so a second key that compares
equivalent can still differ in fields the comparator ignores. `size()` is
the number of nodes, so that second insert does not grow size.

`find` of a missing key is the end iterator. It does not allocate a node
and it does not return an iterator to a key it just made up. `contains` of
a missing key is `find` against `end`, so it is false. An empty container
has no node: `_M_head` is nullptr, so `begin()` equals `end()`.

`erase` of a present key unlinks that node, destroys it, and frees it with
`delete`. `erase` of a missing key returns 0 and does not reach that
`delete`. `clear` and the destructor call `erase`, so the only `delete`
expressions in the header are in `erase`. A pointer taken from an iterator
(`&*begin()`, or `operator->`) addresses the key inside the node. After
`erase` of that key, a use of the pointer is a use after the node was
freed (MEM-UAF). `erase` of some other key frees only that other node.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and the pir
stage lowers that translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- ranges (`insert_range` and the `from_range` constructors)
- `extract`
- `merge`
- a third template argument (the key container, including `vector`)
- an `initializer_list` constructor

`src/prism/bmc.cpp` still reports `C++ flat_set unencoded` from the source
text. This slice does not edit that, and it does not edit `stage.cpp`.

## Tests

`tests/cpp/test_cxx_flat_set.cpp` reads the header. Both `insert` overloads
return the existing node before `new _Node` and before `++_M_size`, and
that prefix does not assign `_M_key`. `size()` returns `_M_size`. `find`'s
lookup does not allocate. The key `erase` returns 0 for a missing key
before `delete`. The `delete` expressions in the header are the ones in
the two `erase` overloads.

`tests/cxx_models/flat_set_trace.cpp` includes `<flat_set>` and prints
empty `begin == end`, a duplicate insert that does not grow size, a
missing `contains` / `find`, and erase of a missing key then a present one.

`testdata/conformance/prism/cxx/cxx_flat_set_true.cpp` inserts one key and
uses `contains` (`memsafety` true). `cxx_flat_set_false.cpp` saves `&*begin()`
and, when `c` is nonzero, erases that key and reads the pointer (`false`,
witness `[1]`). That is not a missing-key `contains`: a missing key is
false, which the model defines.
`testdata/conformance/libc-models/flat_set_contracts.cpp` is the
libc-contract harness: contains, duplicate insert (size and the stored
key), missing find / contains, and erase of a missing key are true; the
pointer used after erase is `MEM-UAF`; a wrong `size() == 2` after a
duplicate insert is `FUNC-CONTRACT`.

## Wiring

`PRISM_PIR_MODELS` gained `src/prism/pir/models/cxx/flat_set`.
`_prism_test_sources` gained `tests/cpp/test_cxx_flat_set.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"flat_set"` next to the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.
