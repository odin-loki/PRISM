# functional not_fn

Branch `finish/cxx-functional-not-fn`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`std::not_fn` is two overloads on the existing header file
`src/prism/pir/models/cxx/functional`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_FUNCTIONAL`.
`#pragma GCC system_header`. Namespace `std`. The `std::function` class and
both `std::invoke` overloads are unchanged.

PIR was not run. The solver was not run. This header is not a proof.

## What not_fn does

Only a non-variadic function pointer whose result is usable as a bool
(`static_cast<bool>` is valid). There is one overload for
`_Res (*)(_Params...)` and one for `_Res (*)(_Params...) noexcept`. `void`
is not usable as a bool. A class with no conversion to `bool` is not either.

`not_fn` returns a wrapper that stores the pointer. Calling that wrapper
with a null pointer calls `__prism_check` with class `FUNC-CONTRACT` and
message `std::not_fn called with a null function pointer`, then
`__builtin_unreachable`. That path does not return `true` or `false` and
does not call through the null pointer. Building the wrapper from a null
pointer does not itself return a predicate and does not run the check.

A non-null pointer is called as
`_M_fn(static_cast<_Args&&>(__args)...)`. Those are the forwarded
arguments. The result is `!static_cast<bool>(...)`, the negation of that
bool. A `bool` result flips. An `int` result is false at 0 and true
otherwise, then flipped. A pointer result is false when null, then flipped.
A reference return is read and flipped; the referred object is not assigned.
An lvalue reference argument is the caller's object. An rvalue argument is
forwarded as an rvalue.

## What std::function and std::invoke still do

An empty `operator()` is still `__prism_check` with class
`CXX-STD-FUNCTION-NULL` and a message that names `bad_function_call`, then
`__builtin_unreachable`. That path does not return a default result and does
not call through null. A move still copies the pointer and leaves the source
empty (`__o._M_fn = nullptr`). The move does not call the target.

A null `std::invoke` is still `__prism_check` with class `FUNC-CONTRACT` and
message `std::invoke called with a null function pointer`, then
`__builtin_unreachable`. That path does not call through the null pointer
and does not return a default result. A non-null pointer is still called
with the forwarded arguments.

## What falls through to libstdc++

Not an overload, so the call fails to compile against this header and pir
falls back (`extra.cxx_models`). They are not stubbed:

- a functor, a pointer to a member, and a function pointer with a C ellipsis
- a function pointer whose result is not usable as a bool, including `void`
- `bind`, `mem_fn`, `ref`, `cref`, `bind_front`, and `reference_wrapper`
- `std::invoke` of a functor, a member pointer, or `std::function`

The header does not `#include <memory>` or `<type_traits>`. It includes no
standard header.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/functional` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_functional.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already
lists `functional` in the public header array. That array was not edited.
No conformance harness was added. `src/prism/bmc.cpp` and
`src/prism/bmc_unenc.inc` were not edited.

## Tests

`tests/cpp/test_cxx_functional.cpp` reads the header. It is a regular file.
`operator()` still checks `CXX-STD-FUNCTION-NULL` before
`__builtin_unreachable`, and the call `_M_fn(__args...)` is after that.
Both `invoke` overloads check `FUNC-CONTRACT` before
`__builtin_unreachable`, and the call with `static_cast<_Args&&>` is after
that. The move constructor still assigns `__o._M_fn = nullptr`. The
`not_fn` call operator checks `FUNC-CONTRACT` before
`__builtin_unreachable`, and
`return !static_cast<bool>(_M_fn(static_cast<_Args&&>(__args)...))` is after
that. The null slice does not return, and it does not contain `true`,
`false`, or a call through `_M_fn`. Both overloads require
`_Res_bool<_Res>::value`, and `_Res_bool<void>` is false. With line comments
removed, the code names `not_fn` and does not name `bind`, `mem_fn`,
`reference_wrapper`, `bind_front`, `cref`, or `ref(`. It has no `#include`.

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
not zero 1 calls=12
not copy 0 1 calls=14
not flag 1 0 calls=16
not ref 0 m=4 calls=17
not rv 0 calls=18
not both 1 calls=19
not nx 1 calls=20
not ptr 1 0 calls=22
not obj 1 0 calls=24
not bit 1 bit=0 calls=25
not hold
```

`move src=0` is the moved-from `std::function`. `not zero 1` is the negation
of `false`. `not copy 0 1` is a copied wrapper: `even(2)` is true and
negates to false, `even(3)` is false and negates to true. `not flag 1 0`
negates an `int` (`0` then `5`). `not ref` updates the caller's `int`
through an `int&` parameter (`m=4`) and negates the returned `bool`.
`not rv` forwards a prvalue to an `int&&` parameter. `not both` forwards
two arguments. `not nx` is a `noexcept` function pointer. `not ptr 1 0`
negates a null pointer result, then a non-null one. `not obj` negates
`Flag`, which has an explicit `operator bool` and no default use as a
stand-in result. `not bit 1 bit=0` negates a `bool&` and leaves that `bool`
false. `not hold` builds `not_fn` from a null pointer and does not call it.
`argv` `empty` aborts with
`CHECK CXX-STD-FUNCTION-NULL: std::function::operator() on an empty target (bad_function_call)`
and prints no `fake=`. `argv` `nullfn` aborts with
`CHECK FUNC-CONTRACT: std::invoke called with a null function pointer`
and prints no `fake=`. `argv` `nullnot` aborts with
`CHECK FUNC-CONTRACT: std::not_fn called with a null function pointer`
and prints no `fake=`.

Against this header, `std::not_fn` of a `bool` function, a `noexcept`
function, an `int` function, a pointer-returning function, an `int&`
parameter, and a `bool&` return compiles. libstdc++ accepts that translation
unit too. `std::not_fn` of a functor, of a member pointer, of `void`, of a
function that returns a type with no conversion to `bool`, and of a C
ellipsis fails to compile against this header. libstdc++ accepts those
translation units. `std::bind`, `std::mem_fn`, `std::ref`, `std::invoke` of
a functor, of a member pointer, and of `std::function` still fail to compile
against this header and still compile with libstdc++. `std::invoke` of a
function and of a `noexcept` function compiles against both. Without a
usable clang++ the compile cases are `NOTRUN`. The header-text case does
not need a compiler. cmake, ninja, `prism_tests`, and pir were not run.
