# forward_list pop_front

Branch `finish/cxx-forward-list-pop`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`std::forward_list::pop_front` lives in the existing extensionless file
`src/prism/pir/models/cxx/forward_list`. A directory of that name would make
`#include <forward_list>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_PRISM_MODEL_FORWARD_LIST`. The file
still defines `_GLIBCXX_FORWARD_LIST` and is still a
`#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Empty
`pop_front()` is not `PROVED` and not `CLEAN`. It is not a proof that the
list had an element, and the trap is not a proof that the node was freed.

## What empty pop_front() does

`pop_front()` calls

```
__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",
              "std::forward_list::pop_front on an empty list");
if (this->empty()) __builtin_trap();
```

and only then `_M_erase_after(&_M_head)`. On an empty list the check
argument is 0. The trap is before that erase. `_M_erase_after` is what
calls `_Prism_fwd_drop`, and that drop is `std::destroy_at` of the element,
`std::destroy_at` of the node, then `deallocate`. The empty call does not
reach those. It does not invent an element. It does not return `T{}`,
`value_type{}`, or `value_type()`. `pop_front` is `void` and the body has
no `return`.

`empty()` is still `_M_head._M_next == nullptr`. That read is the sentinel
stored in the container. It is the emptiness test, and it is not a destroy
or a free of a heap node.

A list that is not empty still unlinks the successor of the sentinel and
frees that node. A pointer, reference, or iterator to that element is still
`MEM-UAF`.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. This slice does not add `#include <memory>`. The
header already included it for `destroy_at` and `construct_at`.

Before this slice, empty `pop_front` was `__glibcxx_assert(!this->empty())`
and then the same erase. With libstdc++ 14 that macro calls
`__glibcxx_assert_fail` when `_GLIBCXX_ASSERTIONS` is on. That is an abort,
not `__builtin_trap`, and the message is the condition text. When the macro
is off, the erase ran on a null successor. The body did not trap before
touching that node.

## What stayed

`front()` and `const front()` are unchanged. Each is still the
`FUNC-CONTRACT` check `"std::forward_list::front on an empty list"`, then
`__builtin_trap`, then `return *_S_val(_M_head._M_next)`. `insert_after`,
`erase_after`, and `push_front` are unchanged. `splice_after` and `merge`
are still not declared. They are not stubbed. A unit that names either
fails to compile against this header, and pir falls back
(`extra.cxx_models`).

`testdata/conformance/libc-models/forward_list_contracts.yml` already
expects `forward_list_pop_empty_false` to be false with class
`FUNC-CONTRACT`. This slice does not change that file and does not record
the result as `PROVED` or `CLEAN`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/forward_list`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_forward_list_pop.cpp
```

No existing line was removed. `tests/cpp/test_cxx_forward_list_front.cpp`
still locks `front()`. Its file-wide count of this check and of
`__builtin_trap()` is 3, because `pop_front` is the third. That test now
expects the trap in `pop_front` before `_M_erase_after`.
`tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_forward_list_pop.cpp` reads the header. The
`pop_front` body contains the `FUNC-CONTRACT` check, then
`__builtin_trap()`, then `_M_erase_after(&_M_head)`. The text before the
trap does not name `destroy_at`, `deallocate`, `operator delete`, `free(`,
`_Prism_fwd_drop`, or `_M_erase_after`, and it does not return or construct
a `T`. The message occurs once in that body. `__builtin_trap()` occurs
three times in the header: two `front` overloads and `pop_front`. Both
`front` bodies still trap before the node load and do not use the
`pop_front` message. After comments are stripped, `splice_after` and
`merge` are not identifiers.
