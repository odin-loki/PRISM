# forward_list front

Branch `finish/cxx-forward-list-front`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::forward_list::front` lives in the existing extensionless file
`src/prism/pir/models/cxx/forward_list`. A directory of that name would make
`#include <forward_list>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_PRISM_MODEL_FORWARD_LIST`. The file
still defines `_GLIBCXX_FORWARD_LIST` and is still a
`#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Empty `front()`
is not `PROVED` and not `CLEAN`. It is not a proof that the list has an
element.

## What empty front() does

`front()` and `const front()` each call

```
__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",
              "std::forward_list::front on an empty list");
if (this->empty()) __builtin_trap();
```

and only then `return *_S_val(_M_head._M_next)`. On an empty list the check
argument is 0. The trap is before that node load, so the empty call does not
dereference a null successor. The empty path has no `return`. It does not
return `T{}`, `value_type{}`, or `value_type()`. A list that is not empty
still returns a reference to the first element.

`empty()` is still `_M_head._M_next == nullptr`. That read is the sentinel
stored in the container. It is the emptiness test, and it is not a load of
a heap node.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What stayed

`insert_after`, `erase_after`, `push_front`, and `pop_front` are unchanged.
`pop_front` on an empty list is still `__glibcxx_assert(!this->empty())`
and then `_M_erase_after` of the sentinel. `splice_after` and `merge` are
still not declared. They are not stubbed. A unit that names either fails to
compile against this header, and pir falls back (`extra.cxx_models`).

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/forward_list`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_forward_list_front.cpp
```

No existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not
edited. The skips for `iostream/prism_ostream.hpp` and `deque/deque` are
unchanged. No new conformance task was added, so this slice does not record
an `expected` value for `front`.

## Tests

`tests/cpp/test_cxx_forward_list_front.cpp` reads the header. Each `front`
body contains the `FUNC-CONTRACT` check, then `__builtin_trap()`, then
`return *_S_val(_M_head._M_next)`. The text before the trap does not name
`_S_val` and does not return. Neither body returns a value-initialized `T`.
The message occurs once in each body. `__builtin_trap()` occurs twice in
the header, once per overload. `pop_front` still asserts `!this->empty()`
and does not trap. `push_front`, `insert_after`, and `erase_after` do not
call `__prism_check` or `__builtin_trap`. After comments are stripped,
`splice_after` and `merge` are not identifiers.
