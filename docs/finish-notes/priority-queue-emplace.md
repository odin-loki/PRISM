# priority_queue emplace

Branch `finish/cxx-priority-queue-emplace`, base `b9b25b0c57a337947a22e655e0af197b6f619b6b`.

`std::priority_queue::emplace` lives on the existing header
`src/prism/pir/models/cxx/priority_queue`. That path is a file. A directory
of the same name would make `#include <priority_queue>` fail with EISDIR.
libstdc++ has no header of that name: the class lives in `<queue>`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`.

## What emplace does

```cpp
template <typename... _Args>
void emplace(_Args&&... __args) { _M_push_node(_M_create(std::forward<_Args>(__args)...)); }
```

That is the same pair of calls as `push`: `_M_create` allocates one node
and constructs the element there, then `_M_push_node` links that node and
sifts it. Nodes already in the heap are not destroyed and not deallocated.
If the constructor throws, `_M_create` drops the new node. If the index
allocation throws, `_M_push_node` drops the new node. Either way the
existing nodes stay.

The return type is `void`, the same as libstdc++ 14. The call does not
hand back an element. The new value is not necessarily `top()`.

A traced element is constructed in the node (` c4 c6 c5`) and destroyed on
`pop`. There is no move-construction and no move-assignment along the heap.
libstdc++ `push_heap` move-assigns inside its container, so that
construction trace is the model's, as it already was for `push`.

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
`pop` is still ASan `heap-use-after-free`.

Not declared, so a use fails to compile against this header and the pir
stage lowers it again with libstdc++. They are not stubbed:

- `swap`
- the range and container constructors
- `container_type`
- `from_range` / `push_range`

The header does not include `<vector>`, `<queue>`, `<deque>`, or `<memory>`.
`std::construct_at` and `std::destroy_at` come in through
`<bits/alloc_traits.h>`.

`CMakeLists.txt` was not edited. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` was not edited. `priority_queue` is
already in that list. `src/prism/pir/stage.cpp` was not edited.

## Tests

Checked with clang++ 18.1.3, libstdc++ 14, `-std=c++23
-D_GLIBCXX_ASSERTIONS -Wall -Wextra -Werror`, and the trace also with
`-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. The
include path was a directory that contains only this header.

`tests/cxx_models/priority_queue_trace.cpp` prints the previous max-heap
order `9 6 5 4 3 2 1 1` and min-heap order `1 1 2 3 4 5 6 9`. After
`emplace(3)`, `emplace(1)`, `emplace(4)`, a reference to `top()` (4), and
`emplace(9)`:

```text
emplace kept=4 top=9 size=4
emplace 9 4 3 1
```

The kept `4` is the node from before `emplace(9)`. libstdc++ `<queue>`
prints the same pop order `9 4 3 1`. `static_assert` requires
`decltype(em.emplace(0))` to be `void`. `int x = q.emplace(1)` is
ill-formed. `a.swap(b)` and `priority_queue(std::from_range, a)` are
ill-formed. `priority_queue_trace uaf` is still ASan
`heap-use-after-free` on the node `pop` freed.

`testdata/conformance/libc-models/priority_queue_contracts.cpp` no longer
says emplace is absent. `priority_queue_push_true` inserts `x` with
`emplace` between two `push` calls. The yml verdicts are unchanged.
`testdata/conformance/prism/cxx/cxx_priority_queue_true.cpp` keeps the
reference from `top()` across `push` and across `emplace` of a value at
most 3 (`memsafety` true). `cxx_priority_queue_false.cpp` is unchanged:
the reference is used after `pop`. Those TUs compile against this header.
PIR was not run.
