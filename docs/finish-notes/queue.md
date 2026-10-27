# queue slice

Branch `finish/cxx-queue`, base `66d1b3373ddf27f0a3187c35d2d19bd533a599cc`.
`std::queue` is a header file, `src/prism/pir/models/cxx/queue`. A directory
of that name would make `#include <queue>` fail with EISDIR and every such
unit would fall back to libstdc++. The header does not include `<deque>`,
`<list>`, or `<vector>`.

The solver was not run. The yml files record the contract each harness asks
for.

## What empty front() does

`front()`, `back()`, and `pop()` each begin with
`__glibcxx_assert(!this->empty())`. The condition text does not contain
`size()`, so PIR classifies a reachable `__glibcxx_assert_fail` as
`FUNC-CONTRACT`. The fail function does not return, so the empty call does
not yield an element.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS`, model directory
as `-isystem`: `front()` on an empty `queue<int>` aborts with

```text
Assertion '!this->empty()' failed.
```

and prints no value. `back()` and `pop()` use that same check. Without
`_GLIBCXX_ASSERTIONS` the empty `front()` loads through a null node pointer
and the process dies with SIGSEGV, still with no substitute element. The pir
stage compiles C++ with `_GLIBCXX_ASSERTIONS`.

## What the model covers

A singly linked list. One heap node per element, allocated with
`std::allocator` of the node (`operator new` / `operator delete`). `push`
and `emplace` construct the new element in a new node at the back. They do
not move nodes already in the queue, so a reference to an existing element
stays valid across `push`. `pop` destroys the front element and deallocates
that node. A reference from `front()` or `back()` to that element points
into the freed allocation, and using it is `MEM-UAF`. There is no
invalidation flag. A reference to a different element stays valid: `pop`
frees only the front node.

`empty()` is `_M_head == nullptr`. `size()` is the node count. Copy
constructs elements front to back and frees a partial copy if a constructor
throws. Move steals the pointers and leaves the source empty; a self-move
is a no-op. Copy assign assigns the shared prefix in place, then either
links a side chain of the extra tail or destroys the leftover tail nodes.
The destructor destroys elements front to back and frees every node.
`swap` exchanges the pointers, so references keep the same nodes. A throwing
copy in `push` frees the new node before it is linked, and the queue is
unchanged. The node type is not part of the class layout, so
`struct Rec { std::queue<Rec> kids; };` works.

## What falls through to libstdc++

Not declared, or rejected by `static_assert`, so the unit fails to compile
against this header and `lower_to_ir` lowers it again with libstdc++
(`extra.cxx_models` records the fallback). They are not stubbed:

- `std::priority_queue`
- any container template argument other than the model's default
  (`std::queue<T>` is the modelled form)
- `from_range` and allocator-extended constructors
- `std::pmr::queue`

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It was not edited. `houdini.inc` was
not edited. `CMakeLists.txt` `PRISM_PIR_MODELS` gained one line:

```text
src/prism/pir/models/cxx/queue
```

`tests/cpp/test_qa_conformance.cpp` lists `"queue"` next to `vector`, `map`,
`set`, `unordered_map`, `unordered_set`, `sstream`, and `list`. The
`iostream/prism_ostream.hpp` skip and the `deque/deque` skip are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::queue unencoded: ... is not a queue model` from the function
body. concolic, the fuzzer, FuSeBMC, and interval consult that gate.
`tests/cpp/test_concolic.cpp` expects `queue_unenc_bad` to be
`NEEDS-HARNESS`. Those files were not edited. There is no
`tests/cpp/test_cxx_queue.cpp`, so `prism_tests` does not compile
`tests/cxx_models/queue_trace.cpp`.

## Tests

Differential, clang++ 18.1.3, libstdc++ 14, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-sanitize-recover=all
-g -O1`. `tests/cxx_models/queue_trace.cpp` compiled twice (`-isystem` the
model directory, and without it). 27 stdout lines, identical, including
construction and destruction order. `queue_trace uaf` on the model aborts
with AddressSanitizer `heap-use-after-free`: the read is `front()` after
`pop`, and the free is `operator delete` in `_M_drop`. The libstdc++ binary
returns the stale `1` and ASan stays quiet, because the sequence block is
not freed.

`testdata/conformance/prism/cxx/cxx_queue_true.cpp` keeps a reference to
`back()` across `push` and across `pop` of a different element (`memsafety`
true). `cxx_queue_false.cpp` uses `front()` after `pop` of that element
(`false`, witness `[1]`).
`testdata/conformance/libc-models/queue_contracts.cpp` is the libc-contract
harness: push and pop true; the `front()` reference after `pop` is
`MEM-UAF`; `front` / `back` / `pop` on empty and a wrong `front()` assert
are `FUNC-CONTRACT`. Those three TUs compile against the model header. PIR
was not run.
