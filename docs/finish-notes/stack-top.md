# stack top

Branch `finish/cxx-stack-top`, base `f636171290aae9a1711c3a1e459bc51722bdbc4f`.

`std::stack::top` lives in the existing extensionless file
`src/prism/pir/models/cxx/stack`. A directory of that name would make
`#include <stack>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_PRISM_MODEL_STACK`. The file still
defines `_GLIBCXX_STACK` and is still a `#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Empty `top` is
not `PROVED` and not `CLEAN`. It is not a proof that the stack had an
element.

## What empty top does

`top()` and `const top()` each call

```
__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",
              "std::stack::top on an empty stack");
if (this->empty()) __builtin_trap();
```

and only then `return *_M_top->_M_valptr()`. On an empty stack the check
argument is 0. The trap is before that node load, so the empty call does
not dereference `_M_top` and does not read `_M_valptr`. The empty path has
no `return`. It does not invent an element and does not return `T{}`,
`value_type{}`, or `value_type()`. A stack that is not empty still returns
a reference to the element in the top node.

`empty()` is still `_M_top == nullptr`. That read is the pointer stored in
the container. It is the emptiness test, and it is not a load of a heap
node.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

Previously `top` called `__glibcxx_requires_nonempty()`, which is empty
unless `_GLIBCXX_ASSERTIONS` is set. With that flag, libstdc++ aborts from
`__glibcxx_assert_fail` on `!this->empty()`. That is not `__builtin_trap`.
The model check and trap run whether or not the flag is set.

## What stayed

`pop`, `push`, `emplace`, `swap`, `empty`, and `size` are unchanged.
Empty `pop` is still

```
__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",
              "std::stack::pop on an empty stack");
if (this->empty()) __builtin_trap();
```

and only then `_M_pop()`. That trap is still before a destroy or a free.
`push` and `emplace` still construct in a new node. `swap` still exchanges
the three fields. `empty()` is still `_M_top == nullptr`. `size()` is still
`_M_size`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/stack`. `CMakeLists.txt` was not edited.
`tests/cpp/test_cxx_stack.cpp` was already one `_prism_test_sources` line.
No line was added and no line was removed.
`tests/cpp/test_qa_conformance.cpp` was not edited. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
`testdata/conformance/libc-models/stack_contracts.yml` was not edited.
`stack_top_empty_false` stays `false` with class `FUNC-CONTRACT`.
`stack_pop_empty_false` stays `false` with class `FUNC-CONTRACT`.

## Tests

`tests/cpp/test_cxx_stack.cpp` reads the header. Each `top` body contains
the `FUNC-CONTRACT` check, then `__builtin_trap()`, then
`return *_M_top->_M_valptr()`. The text before the trap does not name
`_M_top`, `_M_valptr`, `_M_pop`, `_M_drop`, `destroy_at`, or `deallocate`,
and it does not return. Neither body returns a value-initialized `T`. The
message occurs once in each body and once in the file comment.
`__builtin_trap()` occurs three times in the header: the two `top`
overloads and `pop`. The `pop` body still contains its check, then
`__builtin_trap()`, then `_M_pop()`. `push`, `emplace`, `swap`, `empty`,
and `size` do not call `__prism_check` or `__builtin_trap`.

`tests/cxx_models/stack_trace.cpp` was not edited. Its `__prism_check`
prints nothing when the check passes, and the trace calls `top` only on a
stack that has an element.

Checked with clang++ 18.1.3 and libstdc++ 14,
`-std=c++23 -Wall -Wextra -Wpedantic -Werror`. The doctest translation
unit was compiled on its own, with `PRISM_SOURCE_DIR` pointing at this
worktree, and its 135 assertions passed. `prism_tests` was not built.

A by-path include of the header, with a `__prism_check` that prints and
returns, printed `before ctors=0 dtors=0` and
`CHECK FUNC-CONTRACT: std::stack::top on an empty stack`, then died with
SIGILL (`rc=132`) at `-O0` and at `-O2`, with and without
`-D_GLIBCXX_ASSERTIONS`. `const top()` did the same. The element type has
a deleted default constructor. The run printed no constructor line, no
destructor line, and no `after`. The same driver against libstdc++ 14 with
`-D_GLIBCXX_ASSERTIONS` printed `before ctors=0 dtors=0` and
`Assertion '!this->empty()' failed.` and died with SIGABRT (`rc=134`).

Empty `pop` of that same type printed
`CHECK FUNC-CONTRACT: std::stack::pop on an empty stack` and died with
SIGILL (`rc=132`) at `-O0` and at `-O2`, with no destructor line. A
non-empty `stack<E>` top left top `3`, const top `3`, size `1`, one
constructor, and no destructor until the stack left scope, matching
libstdc++ 14 at `-O0` and `-O2`. A non-empty `stack<int>` left top `2`,
then after one `pop` top `1` and size `1`. `noexcept(top())`, const
`top()`, and `noexcept(pop())` are false, matching libstdc++.
