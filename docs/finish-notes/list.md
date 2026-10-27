# list slice

Branch `finish/cxx-list`, base `e59a8b3df966e33c78ffbd9ced0a8fc90465b16d`.
`std::list` is a header file, `src/prism/pir/models/cxx/list`, the same shape
as `vector`. A directory of that name would make `#include <list>` fail with
EISDIR and every such unit would fall back to libstdc++. iostreams, deque,
`unordered_map` / `unordered_set`, and `sstream` are other slices.

The solver was not run. The yml files record the contract each harness asks
for.

## Wiring (not done in this slice)

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem` (`lower_to_ir`). It does not need an
edit. Two CMake lines do, and this slice does not touch `CMakeLists.txt`:

1. Embed the header. `PRISM_PIR_MODELS` is an explicit list. Add

   `src/prism/pir/models/cxx/list`

   next to `src/prism/pir/models/cxx/vector`. `write_cxx_models` then emits
   `cxx/list`. Until that line exists, `qa conformance: every C++ library
   model header is embedded and exercised` fails: it walks
   `src/prism/pir/models/cxx` and requires each file in `CMakeLists.txt`.
   The harness and the prism/cxx tasks already `#include <list>`, which is
   the other half of that check.

2. Link the doctest. `tests/cpp` is not a glob. `_prism_test_sources` needs

   `tests/cpp/test_cxx_list.cpp`

   next to `tests/cpp/test_cxx_models.cpp`. The trace source
   `tests/cxx_models/list_trace.cpp` is compiled by that driver, not by CMake.

## What the model covers

A circular doubly linked list. One heap node per element, allocated with
`std::allocator` of the node (`operator new` / `operator delete`). `erase`,
`pop_front`, `pop_back`, `remove`, `remove_if`, `unique`, `clear`, and the
destructor free exactly the nodes they drop, so a pointer, reference, or
iterator to a removed element is a real use-after-free. `insert` does not
invalidate. A foreign iterator and `erase(end())` fail
`__glibcxx_assert` (the condition text does not mention `size()`, so PIR
classifies it `FUNC-CONTRACT`). `front` / `back` / `pop_*` require
`!this->empty()`.

`max_size()` is `size_t(-1) / sizeof` of a layout that matches libstdc++ 13's
`_List_node` (two pointers plus `__aligned_membuf`). The model node is one
pointer larger (`_M_owner`, the same idea as the map model) so a foreign
iterator is a failed precondition. On libstdc++ 13 that prints
`768614336404564650` for both `list<int>` and `list<char>` (both nodes are
24 bytes).

`empty()` follows the links, not `_M_size`, so `sort` can move nodes out
without touching the size. `sort` is libstdc++'s iterative bottom-up
mergesort with the transfers inlined. Each merge takes the comparator by
value. A throw puts every scratch chain back on the list; the trace's
throwing comparator leaves `[4, 1, 5, 9, 2, 6, 1, 3]` from
`[3, 1, 4, 1, 5, 9, 2, 6]`, then a later `sort()` is `[1, 1, 2, 3, 4, 5, 6, 9]`.
`reverse` swaps next and prev around the circle (`[1, 2, 3, 2]` becomes
`[2, 3, 2, 1]`). Neither constructs elements.

`insert` of n copies, a range, or an initializer list builds a side chain and
transfers it in one step, so a throw leaves the list unchanged (the trace
throws on the third copy of `E(7)`). `remove` / `remove_if` / `unique` hook
removed nodes at the front of a side list and destroy that list at the end,
which is reverse order of removal. `resize` rolls back a throwing default
constructor with `pop_back`. Copy assign is element-wise, then erase the tail
or insert the remainder. Move assign clears and steals; a self-move is a
no-op, as in libstdc++. The node type is not instantiated in the class body,
so `struct Rec { std::list<Rec> kids; };` works.

Also present: the usual constructors (including the deduction guide),
`assign`, `swap`, `emplace` / `push` at both ends, bidirectional iterators
with asserts on `*` / `->` / `++` and on `--` after the move, `operator==`,
`operator<=>`, non-member `swap`, `std::erase`, `std::erase_if`. There is no
invented `length_error`. libstdc++ 13's list does not throw one.

`_GLIBCXX_ASSERTIONS` preconditions are stricter than libstdc++'s own
`<list>` (its `front()` is `*begin()` and does not assert). Programs that
meet the standard's preconditions match the library.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- every `splice` overload
- every `merge` overload
- any allocator other than `std::allocator<T>`
- `std::pmr::list` (the alias is declared and the class `static_assert`s on
  `polymorphic_allocator`, same as `pmr::vector`)

## Tests

Differential, clang++ 18, libstdc++ 13, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. `tests/cxx_models/list_trace.cpp` compiled
twice (`-isystem` the model directory, and without it). 38 stdout lines,
identical, including construction and destruction order. Both binaries, run
with `uaf`, abort with `heap-use-after-free` after `erase` of the iterator
that is then dereferenced.

`tests/cpp/test_cxx_list.cpp` is that comparison as a doctest. Without
`<bits/c++config.h>` it is `NOTRUN`, same as the other C++ model driver.
`testdata/conformance/prism/cxx/cxx_list_true.cpp` uses an iterator after
erasing a different element (`memsafety` true). `cxx_list_false.cpp` uses it
after erasing that element (`false`, witness `[1]`).
`testdata/conformance/libc-models/list_contracts.cpp` is the libc-contract
harness: push and erase-other true; clear and erase of the iterator itself
`MEM-UAF`; `front` / `pop_front` on empty and a wrong `front()` assert
`FUNC-CONTRACT`. Those three TUs compile against the model header. PIR was
not run.
