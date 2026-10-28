# stack pop

Branch `finish/cxx-stack-pop`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::stack::pop` lives in the existing extensionless file
`src/prism/pir/models/cxx/stack`. A directory of that name would make
`#include <stack>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_PRISM_MODEL_STACK`. The file still
defines `_GLIBCXX_STACK` and is still a `#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Empty `pop` is
not `PROVED` and not `CLEAN`. It is not a proof that the stack had an
element.

## What empty pop does

`pop` is void. On an empty stack it calls

```
__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",
              "std::stack::pop on an empty stack");
if (this->empty()) __builtin_trap();
```

and only then `_M_pop()`. The check argument is 0 when the stack is empty.
The trap is before `_M_pop`, so that path does not destroy the element, does
not deallocate a node, and does not load through `_M_top`. `empty()` is
still `_M_top == nullptr`. That read is the pointer stored in the container.
It is the emptiness test, and it is not a load of a heap node.

The empty path has no `return`. It does not invent an element and does not
return `T{}`, `value_type{}`, or `value_type()`. A stack that is not empty
still destroys only the top node and frees it.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

Previously `pop` called `__glibcxx_requires_nonempty()`, which is empty
unless `_GLIBCXX_ASSERTIONS` is set. With that flag, libstdc++ aborts from
`__glibcxx_assert_fail` on `!this->empty()`. That is not `__builtin_trap`.
The model check and trap run whether or not the flag is set.

## What stayed

`top`, `push`, `emplace`, `swap`, `empty`, and `size` are unchanged.
`top()` is still `__glibcxx_requires_nonempty()` and then
`*_M_top->_M_valptr()`. It does not trap. `_M_pop` still drops the top node
after unlinking it. Non-empty `pop` still calls `_M_pop`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/stack`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_stack.cpp
```

No existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not
edited. The skips for `iostream/prism_ostream.hpp` and `deque/deque` are
unchanged. `testdata/conformance/libc-models/stack_contracts.yml` was not
edited. `stack_pop_empty_false` stays `false` with class `FUNC-CONTRACT`.

## Tests

`tests/cpp/test_cxx_stack.cpp` reads the header. The `pop` body contains the
`FUNC-CONTRACT` check, then `__builtin_trap()`, then `_M_pop()`. The text
before the trap does not name `_M_pop`, `_M_drop`, `_M_top`, `destroy_at`,
or `deallocate`, and it does not return. The body does not return a
value-initialized `T`. The message occurs once in that body and once in the
file comment. `__builtin_trap()` occurs once in the header. `top` still
requires nonempty and does not trap. `push`, `emplace`, `swap`, `empty`,
and `size` do not call `__prism_check` or `__builtin_trap`.

`tests/cxx_models/stack_trace.cpp` defines a host `__prism_check` that
prints nothing when the check passes. Non-empty `pop` takes that path. The
trace does not pop an empty stack.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`,
on ext4. The doctest translation unit was compiled on its own, with
`PRISM_SOURCE_DIR` pointing at a copy of this header, and its 82 assertions
passed. `prism_tests` was not built.

A by-path include of the header, with a `__prism_check` that prints and
returns, printed `before` and
`CHECK FUNC-CONTRACT: std::stack::pop on an empty stack`, then died with
SIGILL (`rc=132`) at `-O0` and at `-O2`. It printed no destructor line and
no `after`. The same driver against libstdc++ 14 with
`-D_GLIBCXX_ASSERTIONS` printed `before` and
`Assertion '!this->empty()' failed.` and died with SIGABRT (`rc=134`). A
non-empty `stack<int>` pop left top `1`, size `1`, and `noexcept(pop())`
false, matching libstdc++ at `-O0` and `-O2`. A non-empty `stack<E>` pop
printed the same construction and destruction order as libstdc++.
`stack_trace` stdout matched libstdc++ at `-O1` with `_GLIBCXX_ASSERTIONS`.
