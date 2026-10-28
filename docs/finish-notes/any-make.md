# any make_any

Branch `finish/cxx-any-make`, base `47a2dd91ae976ca9c21b1a43a626cab4da4e06e8`.
`std::make_any<T>(args...)` is on the existing header
`src/prism/pir/models/cxx/any`. The header is still a file, not a directory.
A directory of that name would make `#include <any>` fail with EISDIR.

The value `any_cast` is unchanged. An empty any, or an any holding a
different type, is still `__prism_check` with class `FUNC-CONTRACT` and
message `any_cast requires a contained object of this type`, then
`throw bad_any_cast`. That path does not return a default `T` and does not
read another type's bytes as `T`.

The pointer `any_cast` is unchanged. A null any pointer, an empty any, or a
different contained type returns `nullptr`. Those overloads do not throw.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What make_any does

`make_any<T>(args...)` is declared when `any` is constructible from
`in_place_type<T>` and those arguments
(`is_constructible_v<any, in_place_type_t<T>, Args...>`). The body is

```cpp
return any(in_place_type<T>, std::forward<Args>(args)...);
```

That is the existing in-place constructor. It allocates one heap holder and
constructs `T` there. The arguments are forwarded into that constructor. An
empty argument list value-initializes that `T`. The contained object is that
`T`. An empty argument list does not build an object of some other type, and
it does not return an empty any.

`make_any<Zero>()` value-initializes one `Zero`. `make_any<Big>(7)`
constructs one `Big` from `7`. `make_any<int>()` holds `0`.
`make_any<int>(8)` holds `8`. `make_any<Two>(1, 2)` constructs one `Two`
from both arguments. `any_cast` of each object reads that value.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- `type()`
- comparison
- the `initializer_list` `emplace` / `in_place` overloads
- the `initializer_list` overload of `make_any`

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model. `CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/any`, and `_prism_test_sources` already
lists `tests/cpp/test_cxx_any.cpp`. `tests/cpp/test_qa_conformance.cpp` was
not edited. No other model file was edited. No standard header was added.

## Tests

`tests/cpp/test_cxx_any.cpp` reads the header. `make_any` is constrained by
`is_constructible_v<any, in_place_type_t<_Tp>, _Args...>` and returns
`any(in_place_type<_Tp>, std::forward<_Args>(__args)...)`. That body does
not return `T{}`, `_Tp{}`, `any{}`, or `any()`, and it does not call
`__prism_check` or throw. The omitted-API list no longer has a `make_any`
bullet. `type()` and comparison are still listed. Value `any_cast` still
calls `_M_target`. Pointer `any_cast` still returns `nullptr` and does not
call `_M_target`, `__prism_check`, or `throw bad_any_cast`. The header still
has no `return T{}`, no `reinterpret_cast`, and no `#include <memory>` or
`<variant>`.

`tests/cxx_models/any_trace.cpp` includes `<any>` and calls `make_any`.
clang++ 18.1.3, libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, compiled
twice (`-isystem` the model directory, and without it). Stdout matched,
including the previous construction trace and:

```
 z after make0
make0 1 0
 c7 after make
made 7
madei 1 0
maden 8
 t1,2 after maketwo
two 1 2
 leaving
 td1,2 d7 zd0 d6 d4 d5 d1done
```

`z` is one value-initialization of `Zero`. `make0 1 0` is an any that holds
that `Zero` with value 0. `c7` / `made 7` is one `Big` built from `7`.
`madei 1 0` is `make_any<int>()` (value 0). `maden 8` is `make_any<int>(8)`.
`t1,2` / `two 1 2` is one `Two` built from both arguments. The destructors
`td1,2`, `d7`, and `zd0` each run once, with the older objects.

The model binary's `e`, `w`, and `i` modes exited 0: an empty value cast
and a wrong-type value cast are still one failed check and `bad_any_cast`,
and a stored 42 is still 42.
