# priority_queue swap

Branch `finish/cxx-priority-queue-swap`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`std::priority_queue::swap` lives on the existing header
`src/prism/pir/models/cxx/priority_queue`. That path is a file. A directory
of the same name would make `#include <priority_queue>` fail with EISDIR.
libstdc++ has no header of that name: the class lives in `<queue>`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`.

## What swap does

```cpp
void swap(priority_queue& __o) noexcept(is_nothrow_swappable_v<_Compare>) {
    if (this == &__o) return;
    _M_swap(__o);
}
```

`_M_swap` exchanges the heap pointer, the index capacity that belongs to
that pointer, the size, and the comparator. Element nodes stay where they
were constructed. The call does not allocate, does not free, and does not
copy elements. Self-swap returns before any of that, so it changes nothing.
Swapping with an empty queue leaves this empty (`size == 0`) and moves the
nodes to the other queue.

The capacity has to travel with the pointer. An emptied queue whose
capacity stayed behind would still look non-empty to `_M_reserve` and the
next `push` would store through a null index. The comparator has to travel
with the heap. `pop` sifts with the comparator this queue holds; leaving
the old comparator behind reorders the nodes that just arrived.

`std::swap` on two `priority_queue` objects is the same member. The generic
`std::swap` move-assigns, and move-assign clears its target. This overload
does not.

## What stayed

`top()` and `pop()` on an empty queue are still

`__glibcxx_assert(!this->empty())`.

The condition text does not mention `size()`. Neither call invents an
element. With `_GLIBCXX_ASSERTIONS` both abort:

```text
Assertion '!this->empty()' failed.
```

exit 134. `pop` still destroys the current top element and deallocates only
that node (`_M_drop(__dead)`). A reference from `top()` used after that
`pop` is still ASan `heap-use-after-free`. `emplace` still constructs in a
new node and returns `void`. `push` is unchanged.

Not declared, so a use fails to compile against this header and the pir
stage lowers it again with libstdc++. They are not stubbed:

- the range and container constructors
- `container_type`
- `from_range` / `push_range`

`priority_queue(std::from_range, a)` is ill-formed (no matching
constructor). `q.push_range(a)` is ill-formed (no member named
`push_range`).

The header does not include `<vector>`, `<queue>`, `<deque>`, or `<memory>`.

`CMakeLists.txt` was not edited. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited. `priority_queue` is
already in that list. `src/prism/pir/stage.cpp` was not edited.

## Tests

Checked with clang++ 18.1.3, libstdc++ 14, `-std=c++23
-D_GLIBCXX_ASSERTIONS -Wall -Wextra -Werror`, and the trace also with
`-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. The
include path was a directory that contains only this header.

`tests/cxx_models/priority_queue_trace.cpp` still prints the previous
max-heap order `9 6 5 4 3 2 1 1`, min-heap order `1 1 2 3 4 5 6 9`, and
`emplace 9 4 3 1`. After that, swap:

```text
swap left=7 right=8 kept=8 lsize=2 rsize=3
swap back left=8 right=7
self top=8 size=3
with empty leftempty=1 holesize=3 holetop=8
repush top=5 size=1
hole 8 3 1
empty swap 1 1
keyed lo 4 2 1 hi 1 2 4
```

`kept=8` is the node `top()` named before the swap. It is still that node
after the pointer exchange, now reached from the other queue. `repush`
inserts into the queue that the swap left empty, so the index that arrived
with the null pointer is a real empty index. `keyed` is two queues of the
same comparator type with opposite orders (`1 4 2`). After swap the pop
orders stay with the heaps (`4 2 1` and `1 2 4`), which is the comparator
moving with the nodes. libstdc++ `<queue>` prints those same lines.

`static_assert` requires `left.swap(right)` and `std::swap(left, right)` to
be `void` and `noexcept`. A traced element is constructed only by
`emplace`, then swap prints nothing, then `pop` destroys the same nodes:

```text
 c1 c3 c2 | | a top2 d2 hole top3 d3 top1 d1
```

There is no copy, no move, and no destructor between the bars. A separate
counter on `operator new` / `operator delete` around member swap, self-swap,
and swap with an empty queue reported `delta new=0 del=0`. The following
`push` allocated. `priority_queue_trace uaf` is still ASan
`heap-use-after-free` on the node `pop` freed.

`testdata/conformance/libc-models/priority_queue_contracts.cpp` no longer
says swap is absent. The range constructors stay omitted. The harness
functions and the yml verdicts are unchanged.
`testdata/conformance/prism/cxx/cxx_priority_queue_true.cpp` and
`cxx_priority_queue_false.cpp` are unchanged. Those TUs compile against
this header. PIR was not run.
