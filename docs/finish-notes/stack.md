# stack slice

Branch `finish/cxx-stack`, base `66d1b3373ddf27f0a3187c35d2d19bd533a599cc`.
`std::stack` is a header file, `src/prism/pir/models/cxx/stack`. A directory
of that name would make `#include <stack>` fail with EISDIR. Include guard
`_PRISM_MODEL_STACK`, and `_GLIBCXX_STACK` so libstdc++'s `<stack>` is not
also included.

The solver was not run. The yml files record the contract each harness asks
for. `stage.cpp` and `houdini.inc` are unchanged. The pir stage already puts
the `cxx/` model directory on the include path.

## What empty pop does

`pop` is void. On an empty stack it does not return 0, does not invent an
element, and does not destroy or free a node. With `_GLIBCXX_ASSERTIONS`
(the pir stage always passes that flag) `pop` and `top` call
`__glibcxx_requires_nonempty()`, which is `__glibcxx_assert(!this->empty())`.
The condition text does not mention `size()`, so PIR classifies it
`FUNC-CONTRACT`. Checked with clang++ 18 against this header: empty `pop`
aborts at `stack:221` with `Assertion '!this->empty()' failed` and empty
stdout. Empty `top` aborts the same way at `stack:202` and does not return
a stand-in zero. libstdc++ 13 and 14 print the same assertion text from
`<bits/stl_stack.h>`.

## Storage

One heap node per element, allocated with `std::allocator` of the node
(`operator new` / `operator delete`). The header does not include `<deque>`,
`<list>`, or `<vector>`. The bottom is the oldest element and the top is the
newest. `pop` destroys the top element and deallocates that node, so a
reference returned by `top` is invalidated by the pop of that element
(MEM-UAF). `push`, and `pop` of a newer element, leave an older reference
in place.

`push` of a temporary is construct, move into the node, destroy the
temporary (`c1 m1 d-1`). `emplace` constructs in the node. Copy walks bottom
to top. Copy-assign assigns the common prefix, then copy-constructs a longer
tail (`=1 cc2`) or destroys a discarded tail from the cut toward the top
(`=1 =2 d6 d7`). Self copy-assign is a no-op. Move steals the pointers.
Move-assign clears first, so a self-move destroys bottom to top and leaves
size 0 (`d1 d2`, size 0). Comparisons are lexicographical from the bottom:
`(1, 3) < (2, 0)` is true. The move constructor is not `noexcept`, matching
libstdc++ (its implicit move moves a deque, and that move is not noexcept),
so `vector<stack<int>>` still copies on reallocation.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++:

- a second template argument (`std::stack<T, Container>`, including deque, list, and vector)
- constructors that take a container or an allocator
- `container_type` and the protected container `c`
- `from_range`, `push_range`, and the container deduction guides

## Wiring

`PRISM_PIR_MODELS` lists `src/prism/pir/models/cxx/stack`, so the embed name
is `cxx/stack` and `write_cxx_models` writes a file named `stack`.
`tests/cpp/test_qa_conformance.cpp` names `stack` in the public-header
array. The iostream absence checks and the `deque/deque` skip are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::stack unencoded: ... is not a stack model` from the source text.
`testdata/planted/cxx_stack.cpp` (`stack_unenc_bad`) stays NEEDS-HARNESS
until those gates are removed. This slice does not edit them.

## Tests

Differential, clang++ 18, libstdc++ 14 (g++ 13 and g++ 14 agree on the
noexcept specifiers), `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. `tests/cxx_models/stack_trace.cpp`
compiled twice (`-isystem` the model directory, and without it). Stdout
matches, including construction and destruction order. `stack_trace uaf`
on the model is ASan `heap-use-after-free`: `operator delete` in `_M_drop`
frees the node `top` referred to. libstdc++ does not free the deque block,
so that run returns the leftover `1` and ASan stays quiet.

There is no `tests/cpp/test_cxx_stack.cpp`, so `prism_tests` does not
compile the trace yet.

`testdata/conformance/prism/cxx/cxx_stack_true.cpp` keeps a reference to an
older element across `pop` of a newer one (`memsafety` true).
`cxx_stack_false.cpp` uses `top`'s reference after `pop` of that element
(`false`, witness `[1]`). `testdata/conformance/libc-models/stack_contracts.cpp`
is the libc-contract harness: push and pop-under true; pop of the referenced
top `MEM-UAF`; empty `top` / empty `pop` and a wrong `top()` assert
`FUNC-CONTRACT`.
