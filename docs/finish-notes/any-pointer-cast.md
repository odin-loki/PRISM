# any pointer cast

Branch `finish/cxx-any-pointer-cast`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`any_cast<T>(any*)` and `any_cast<T>(const any*)` are on the existing header
`src/prism/pir/models/cxx/any`. The header is still a file, not a directory.
A directory of that name would make `#include <any>` fail with EISDIR.

The value `any_cast` is unchanged. An empty any, or an any holding a
different type, is still `__prism_check` with class `FUNC-CONTRACT` and
message `any_cast requires a contained object of this type`, then
`throw bad_any_cast`. That path does not return a default `T` and does not
read another type's bytes as `T`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What the pointer overloads do

`any_cast<T>(any*)` returns `T*`. `any_cast<T>(const any*)` returns
`const T*`. Both are `noexcept`. `T` is looked up with `remove_cv_t`, so
`any_cast<const int>` addresses an any that holds an `int`.

- A null any pointer returns a null pointer. The tag is not loaded.
- An empty any returns `nullptr`.
- A different contained type returns `nullptr`. That path does not cast the
  holder to the requested type.
- The type that was stored returns a pointer to that object (`_M_value`),
  not a pointer to a temporary `T`. A write through the non-const pointer
  is the value the next value `any_cast` reads.
- Neither overload calls `__prism_check` or throws. A null result is the
  result the pointer form specifies, not a failed value cast.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- `make_any`
- `type()`
- comparison
- the `initializer_list` `emplace` / `in_place` overloads

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model. `CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/any`, and `_prism_test_sources` already
lists `tests/cpp/test_cxx_any.cpp`. `tests/cpp/test_qa_conformance.cpp` was
not edited. No other model file was edited. No standard header was added.

## Tests

`tests/cpp/test_cxx_any.cpp` reads the header. The pointer definitions
return `nullptr`, call `_M_target_ptr`, and do not call `_M_target`,
`__prism_check`, or `throw bad_any_cast`. `_M_target_ptr` returns
`nullptr` before it reads a tag, and the non-null result is `_M_value`.
The sentence that said the pointer overloads are omitted is gone. Value
`any_cast` still calls `_M_target`. The header still has no `return T{}`,
no `reinterpret_cast`, and no `#include <memory>` or `<variant>`.

`tests/cxx_models/any_trace.cpp` includes `<any>`. clang++ 18.1.3,
libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, compiled twice (`-isystem` the model
directory, and without it). Stdout matched, including the previous
construction trace and:

```
ptr 42 42 42
miss 0 0 0 0 0
mut 9
```

`ptr` is `any_cast<int>`, `any_cast<const int>`, and `any_cast<int>` on a
`const any*`, all reading the stored 42. `miss` is an empty any, a
`double` asked for as `int`, a null `any*`, a null `const any*`, and an
`int` asked for as `double`: all null. `mut 9` is a write through the
`int*` followed by value `any_cast<int>`. The model binary's `e`, `w`, and
`i` modes exited 0: an empty value cast and a wrong-type value cast are
still one failed check and `bad_any_cast`, and a stored 42 is still 42.
