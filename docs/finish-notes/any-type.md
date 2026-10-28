# any type()

Branch `finish/cxx-any-type`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.
`std::any::type()` is on the existing header `src/prism/pir/models/cxx/any`.
The header is still a file, not a directory. A directory of that name would
make `#include <any>` fail with EISDIR.

`any_cast`, `make_any`, `reset`, `emplace`, `has_value`, and `swap` are
unchanged. Value `any_cast` on an empty any, or on an any holding a
different type, is still `__prism_check` with class `FUNC-CONTRACT` and
message `any_cast requires a contained object of this type`, then
`throw bad_any_cast`. That path does not return a default `T`. Pointer
`any_cast` is unchanged. Comparison is not declared.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run. This slice is not `PROVED` and not `CLEAN`.

## What type() does

`const type_info& type() const noexcept` does not throw.

An empty any returns `typeid(void)`. That return happens before the holder
is loaded. `_M_ptr == nullptr` is the empty check, including a default
any, an any after `reset()`, and an any after it was moved from.

An any that holds `T` returns the `type_info` of that `T`. The holder did
not store a `type_info` (it stored a per-type tag address used by
`any_cast`). The holder now also stores `const type_info* _M_type`, set to
`&typeid(_Tp)` when `_Prism_any_holder_impl<_Tp>` is created. `type()`
returns that object. It does not return `typeid` of some other type, and
it does not return a default `T`.

`reset()` still only deletes the holder and sets `_M_ptr` to null. That
delete drops the stored `type_info` pointer, so a following `type()` is
`typeid(void)`. Copy builds a new holder, so the copy stores `typeid` of
the same `T`. Move steals the holder. `emplace` and `make_any` build a new
holder, so the stored type is the `T` those calls construct. `swap`
exchanges holders, so the types follow the objects.

`<typeinfo>` was already included. `<memory>` is not included.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- comparison
- the `initializer_list` `emplace` / `in_place` overloads
- the `initializer_list` overload of `make_any`

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model. `CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/any`, and `_prism_test_sources` already
lists `tests/cpp/test_cxx_any.cpp`. The trace cpp is not in that list.
`tests/cpp/test_qa_conformance.cpp` was not edited. No other model file
was edited.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` were not edited. A
function body that contains `std::any` is still `is not an any model`. A
function body that contains `typeid(` is still `C++ typeid unencoded` /
`is not an RTTI model`. Those exits stay in front of this header.

## Tests

`tests/cpp/test_cxx_any.cpp` reads the header. `type()` is
`const type_info& type() const noexcept`. The empty return is
`typeid(void)` and that statement precedes the load of `_M_ptr->_M_type`.
The held result is `*_M_ptr->_M_type`. The body does not throw, does not
call `__prism_check`, does not name `typeid(int)` or `typeid(_Tp)`, and
does not return `T{}`. `&typeid(_Tp)` appears once, in the holder
constructor. The omitted-API list no longer has a `type()` bullet.
Comparison is still listed. `reset()` is still `delete _M_ptr` and
`_M_ptr = nullptr`. The header still has no `return T{}`, no
`reinterpret_cast`, and no `#include <memory>`.

`tests/cxx_models/any_trace.cpp` includes `<any>` and calls `type()`.
clang++ 18.1.3, libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, compiled
twice (`-isystem` the model directory, and without it). Stdout matched,
including the previous construction trace and:

```
typeempty 1
typeheld 1 0
typecopy 1 1
typemove 1 1
typemoveas 1 1
typereset 1
typeemplace 1
typeswap 1 1
typeint 1 0
typeintempty 1 1
typeself 1
typeptr 1 1 1
typemake0 1
typemade 1
typemadei 1
typemaden 1 0
typetwo 1
```

`typeempty` / `typereset` / the moved-from half of `typemove` and
`typemoveas` are `typeid(void)`. `typeheld` is `Big` and not `void`.
`typeint` is `int` and not `double`. `typemaden` is `int` and not `void`.
`typeptr` is an empty any, a `double`, and the stored `int`. `typeself`
stays `Big` after self-move. The `make_any` lines are `Zero`, `Big`,
`int`, `int`, and `Two`.

Mode `t` exited 0 on both binaries: empty is `typeid(void)`, a stored
`int` is `typeid(int)` and not `typeid(double)`, `reset` returns to
`typeid(void)`, move leaves the source `typeid(void)`, `make_any<int>` is
`typeid(int)`, `emplace<double>` replaces that with `typeid(double)`, and
`swap` of an `int` and a `double` exchanges those types. `type()` did not
fail a check. The model binary's `e`, `w`, and `i` modes exited 0: an
empty value cast and a wrong-type value cast are still one failed check
and `bad_any_cast`, and a stored 42 is still 42.
