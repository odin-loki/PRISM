# functional invoke

Branch `finish/cxx-functional-invoke`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::invoke` is two overloads on the existing header file
`src/prism/pir/models/cxx/functional`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_FUNCTIONAL`.
`#pragma GCC system_header`. Namespace `std`. The `std::function` class is
unchanged.

PIR was not run. The solver was not run. This header is not a proof.

## What invoke does

Only a non-variadic function pointer. There is one overload for
`_Res (*)(_Params...)` and one for `_Res (*)(_Params...) noexcept`.

A null pointer calls `__prism_check` with class `FUNC-CONTRACT` and message
`std::invoke called with a null function pointer`, then
`__builtin_unreachable`. That path does not return a default result and does
not call through the null pointer.

A non-null pointer is called as `__f(static_cast<_Args&&>(__args)...)`.
Those are the forwarded arguments. The result is that call. A `void`
function returns void. A reference return is that reference. `E`, which has
no default constructor, is returned from the function that builds it.

## What std::function still does

An empty `operator()` is still `__prism_check` with class
`CXX-STD-FUNCTION-NULL` and a message that names `bad_function_call`, then
`__builtin_unreachable`. That path does not return a default result and does
not call through null. A move still copies the pointer and leaves the source
empty (`__o._M_fn = nullptr`). The move does not call the target.

## What falls through to libstdc++

Not an invoke overload, so the call fails to compile against this header and
pir falls back (`extra.cxx_models`). They are not stubbed:

- a functor, a pointer to a member, and `std::function`
- a function pointer with a C ellipsis
- `bind`, `mem_fn`, `ref`, `cref`, `not_fn`, `bind_front`, and
  `reference_wrapper`

The header does not `#include <memory>` or `<type_traits>`. It includes no
standard header.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/functional` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_functional.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already
lists `functional` in the public header array. That array was not edited.
The skips for `iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not an
invoke model" when the source text contains `std::invoke`, and "is not a
type-erased callable model" when it contains `std::function`, before a
header is inlined. Those files were not edited. The conformance tasks in
`testdata/conformance/` still call `std::function` only, so this slice does
not add an invoke harness those gates would reject.

## Tests

`tests/cpp/test_cxx_functional.cpp` reads the header. It is a regular file.
`operator()` still checks `CXX-STD-FUNCTION-NULL` before
`__builtin_unreachable`, and the call `_M_fn(__args...)` is after that.
Both `invoke` overloads check `FUNC-CONTRACT` before
`__builtin_unreachable`, and the call with `static_cast<_Args&&>` is after
that. Neither invoke body returns `{}`, `_Res{}`, `_Res()`, or `0`. The
move constructor still assigns `__o._M_fn = nullptr`. With line comments
removed, the code does not name `bind`, `mem_fn`, `not_fn`,
`reference_wrapper`, `bind_front`, `cref`, or `ref(`, and it has no
`#include`.

`tests/cxx_models/functional_trace.cpp` includes `<functional>`. Checked
with clang++ 18.1.3 and libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched:

```
empty 0
dtor 0
bool 1
call 42 calls=1
move src=0 dst=1 calls=1
move call 42 calls=2
held 5
reassign 4 calls=3
copy 1 1 calls=3
copy call 42 calls=4
livedtor 4
null 0
invoke 42 calls=5
invoke ptr 42 calls=6
invoke add 42 calls=7
invoke ref 11 n=11 calls=8
invoke rv 10 calls=9
invoke void 10
invoke nx 42 calls=11
invoke slot 9
invoke held 5
```

`move src=0` is the moved-from `std::function`. `invoke ref` updates the
caller's `int` through an `int&` parameter. `invoke rv` passes a prvalue to
an `int&&` parameter. `invoke held` is `E`, which has no default
constructor. `argv` `empty` aborts with
`CHECK CXX-STD-FUNCTION-NULL: std::function::operator() on an empty target (bad_function_call)`
and prints no `fake=`. `argv` `nullfn` aborts with
`CHECK FUNC-CONTRACT: std::invoke called with a null function pointer`
and prints no `fake=`.

Against this header, `std::bind`, `std::mem_fn`, `std::ref`, `std::not_fn`,
`std::invoke` of a functor, of a member pointer, and of `std::function`
fail to compile. libstdc++ accepts that translation unit. `std::invoke` of
a function and of a `noexcept` function compiles against both. Without a
usable clang++ the compile cases are `NOTRUN`. The header-text case does
not need a compiler. cmake, `prism_tests`, and pir were not run.
