# memory dereference

Branch `finish/cxx-memory-deref`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`operator*` and `operator->` for `std::unique_ptr<T>` are on the existing
file `src/prism/pir/models/cxx/memory`. That path is a file. A directory of
the same name would make `#include <memory>` fail with EISDIR. The include
guard is `_GLIBCXX_MEMORY`. `#pragma GCC system_header`. Namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The solver was
not run. This is not PROVED and not CLEAN. The check records the empty
dereference. It is not a proof that every `unique_ptr` is non-null, and it
is not a fuzzer `CLEAN` result.

## What an empty dereference does

`operator*` and `operator->` each call

```
__prism_check(_M_ptr != nullptr, "PTR-NULL-DEREF",
              "std::unique_ptr::operator* null pointer");
if (_M_ptr == nullptr) __builtin_trap();
```

`operator->` uses the message `std::unique_ptr::operator-> null pointer`.
The trap is the next statement. `return *_M_ptr` and `return _M_ptr` are
after that statement.

On an empty `unique_ptr` the check argument is 0. The trap runs before the
stored pointer is dereferenced. That path has no `return`. It does not
invent an element. It does not return `T{}`, `element_type{}`, or
`element_type()`. A `unique_ptr` that holds an object still returns a
reference to that object from `operator*`, and still returns the stored
pointer from `operator->`.

`__prism_check` is declared
`extern "C" void __prism_check(int __ok, const char* __cls, const char* __msg);`
and is not defined in the header.

## What stayed

`release`, `reset`, `operator bool`, `get`, `swap`, `make_unique`,
`operator==`, and `operator!=` are unchanged. Object `reset` still has one
`::operator delete(` after the pointer is replaced. Array `reset` still has
one array delete. `release` still stores nullptr and does not free.
`get()` is still `return _M_ptr`. `operator bool` is still
`_M_ptr != nullptr`. `operator==` still compares the two stored pointers
and does not dereference. `operator!=` is still `!(__x == __y)`.

`operator[]` on `unique_ptr<T[]>` is still the previous
`PTR-NULL-DEREF` check with the message
`unique_ptr::operator[] on an empty unique_ptr`, then `return _M_ptr[__i]`.
That body does not call `__builtin_trap`.

The header does not include `<memory>`. The words `#include <memory>`
appear once, in the comment at the top of the file.

No `shared_ptr`, no `weak_ptr`, and no new array `unique_ptr` were added.
`class unique_ptr<_Tp[], _Dp>` is the array specialization already on this
header. It appears once.

## Tests

`tests/cpp/test_cxx_memory_deref.cpp` reads the header. Each of `operator*`
and `operator->` contains the `PTR-NULL-DEREF` check, then
`if (_M_ptr == nullptr) __builtin_trap();`, then the use of the stored
pointer. The text before the trap does not return and does not name
`*_M_ptr`. Neither body returns a value-initialized `T`. The star message
and the arrow message each occur once. `operator->` does not contain
`*_M_ptr`. The comment above `operator*` says this is not PROVED and not
CLEAN. That sentence appears twice in the header: once there, and once on
the equality operators, which already had it. `__builtin_trap();` occurs
three times: the two operators and the existing `make_unique<T[]>` overflow
arm.

`release`, `reset`, and `swap` still have no check and no trap. `get` and
`operator bool` are still the previous one-liners. `operator==` and
`operator!=` are still the previous bodies. The single-object `make_unique`
still returns `new _Tp(...)` and does not trap. The array `make_unique`
trap is still before `new _Elem[__n]()`. `class shared_ptr` and
`class weak_ptr` are absent. `class unique_ptr<_Tp[], _Dp>` appears once.

`tests/cpp/test_cxx_memory.cpp` still reads the same header. Its file-wide
count of `This is not PROVED and not CLEAN.` is 2, for the equality comment
and the dereference comment. Compiled the same way, that file's ownership
and `make_unique` cases passed. The equality case still fails the check for
the contiguous text `A mismatch returns false and does not dereference.`
The header already breaks that sentence across two comment lines, and those
lines were not edited.

`CMakeLists.txt` lists `tests/cpp/test_cxx_memory_deref.cpp` as one new line
in `_prism_test_sources`, directly after `tests/cpp/test_cxx_memory.cpp`. No
existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited.
`memory` stays in the header-name array.

`tests/cpp/test_cxx_memory_deref.cpp` was compiled by itself with clang++
18.1.3, `-std=c++23 -Wall -Wextra -Werror`, and its `TEST_CASE` passed
(138 assertions). cmake, ninja, and `prism_tests` were not run.

A separate translation unit, not a tracked test, included `<memory>` with
this directory first on the include path. clang++ 18.1.3, `-std=c++23
-Wall -Wextra -Werror -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. A live `unique_ptr<int>` printed `own 7`.
A live `unique_ptr` of a struct printed `arrow 8`. Two empty `unique_ptr`s
compared equal, and `!=` was false. `get()` on an empty `unique_ptr` was
nullptr, `operator bool` was false, and `release` left the owner empty.
That process exited 0. ASan and UBSan stayed quiet. Empty `operator*`
printed `CHECK ok=0 PTR-NULL-DEREF: std::unique_ptr::operator* null pointer`
and then died with SIGILL from `__builtin_trap`. Empty `operator->` printed
`CHECK ok=0 PTR-NULL-DEREF: std::unique_ptr::operator-> null pointer` and
then died with SIGILL. Neither run printed an invented value. That run is
not PROVED and not CLEAN.
