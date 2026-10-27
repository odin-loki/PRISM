# priority_queue slice

Branch `finish/cxx-priority-queue`, base `bd9af87dd51b02ba62649a673f9acf42f3426926`.
`std::priority_queue` is a header file, `src/prism/pir/models/cxx/priority_queue`.
A directory of that name would make `#include <priority_queue>` fail with
EISDIR. libstdc++ has no header of that name: the class lives in `<queue>`
(`_GLIBCXX_QUEUE`). This file defines `_GLIBCXX_PRIORITY_QUEUE` only, so
`<queue>` is unchanged. A unit that includes both gets two definitions of
`std::priority_queue` and the pir stage lowers it again with libstdc++.

The solver was not run. The yml files record the contract each harness asks
for.

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem` (`lower_to_ir`). It was not edited.
`houdini.inc` was not edited.

This slice does add the header to `PRISM_PIR_MODELS`:

`src/prism/pir/models/cxx/priority_queue`

`write_cxx_models` then emits `cxx/priority_queue`.
`tests/cpp/test_qa_conformance.cpp` lists `priority_queue` with the other
public header names. The `iostream/prism_ostream.hpp` and `deque/deque`
skips are unchanged. The libc harness and the prism/cxx tasks
`#include <priority_queue>`.

Still open, same shape as the deque note:

1. `src/prism/bmc.cpp` (`low.find("priority_queue")`) and
   `src/prism/bmc_unenc.inc` (`priority_queue\s*<`) still refuse the source
   text before the header is inlined. `tests/cpp/test_concolic.cpp` expects
   `pqueue_unenc_bad` to be NEEDS-HARNESS until those gates go.
2. `tests/cxx_models/priority_queue_trace.cpp` is not in `_prism_test_sources`.
   There is no `tests/cpp/test_cxx_priority_queue.cpp` in this slice.
3. `docs/PIR.md` still does not describe this header.

## What the model covers

A binary heap of nodes. One heap node per element, allocated with
`std::allocator` of the node (`operator new` / `operator delete`). The index
is a separate array of those node pointers from `::operator new`. `<vector>`,
`<queue>`, and `<deque>` are not included. The container template argument
is not the storage: the default is `_Prism_priority_queue_nodes<T>`, and any
other sequence whose `value_type` is `T` is accepted and ignored.

`push`, `pop`, `top`, `empty`, and `size` are the operations. The comparator
is `std::less` by default, so the top is the greatest element (`comp(a, b)`
means `a` is ordered before `b`). Equal elements are not stable. Sift swaps
node pointers, so an element stays in the node it was constructed in. `push`
does not free element nodes. `pop` destroys the current top element and
deallocates that node, so a reference from `top()` used after that `pop` is
MEM-UAF.

`top()` and `pop()` on an empty queue are preconditions, not a stand-in
value. Both are `__glibcxx_assert(!this->empty())`. The condition text does
not mention `size()`, so PIR classifies the failure FUNC-CONTRACT. With
`_GLIBCXX_ASSERTIONS` (PIR passes that for C++) the call does not return:
`__glibcxx_assert_fail` runs. Assertions off, the next thing is a load of
the missing root.

Checked with clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-Wall -Wextra -Werror`, and the trace also with
`-fsanitize=address,undefined -O1`. `tests/cxx_models/priority_queue_trace.cpp`
against this header prints the max-heap order `9 6 5 4 3 2 1 1` and the
min-heap order `1 1 2 3 4 5 6 9`. The same two lines from libstdc++ `<queue>`
match. A traced `E` is move-constructed into its node once per `push` and
destroyed on `pop` (`c1 m1 d1 ...`, then `top3 d3 top2 d2 top1 d1`); libstdc++
move-assigns along the vector, so that construction trace is the model's.
`priority_queue_trace uaf` is ASan `heap-use-after-free` on the node `pop`
freed. `top()` on a default-constructed queue aborts with
`Assertion '!this->empty()' failed` (exit 134). It does not return an element.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- `emplace`
- `swap`
- the range and container constructors
- `container_type`
- `from_range` / `push_range`
- an over-aligned `T`
- a container argument whose `value_type` is not `T`

## Tests

`testdata/conformance/libc-models/priority_queue_contracts.cpp` is the
libc-contract harness: push and the max-heap pop order true, a greater-than
comparator true, `top` then `pop` then use of the reference `MEM-UAF`, `top`
and `pop` on empty `FUNC-CONTRACT`, and a wrong "top is the lesser value"
assert `FUNC-CONTRACT`.
`testdata/conformance/prism/cxx/cxx_priority_queue_true.cpp` keeps the
reference from `top()` across `push` (`memsafety` true).
`cxx_priority_queue_false.cpp` uses it after `pop` (`false`, witness `[1]`).
Those TUs compile against the model header. PIR was not run.
