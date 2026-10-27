# optional value_or

Branch `finish/cxx-optional-value-or`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`std::optional::value_or` is declared on the existing header
`src/prism/pir/models/cxx/optional`. The header is still a file, not a
directory. The disengaged `value()` / `operator*` / `operator->` checks are
unchanged: they stay `__prism_check` with class `CXX-OPTIONAL-NULL`.

PIR was not run. The solver was not run.

## What value_or does

Two overloads, `const&` and `&&`, each taking a forwarding reference.

- When the optional holds a `T`, `value_or` returns that `T`. The `const&`
  overload copies it. The `&&` overload moves it. The fallback argument is
  not the result.
- When the optional is disengaged, `value_or` returns the argument converted
  to `T` (`static_cast<T>` of the forwarded argument). That path does not
  read the inactive union member, does not return `T{}`, and does not invent
  a value that was not passed in.
- A disengaged `value_or` is not a precondition failure. `value()` on a
  disengaged optional still is.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`
- `transform`
- `and_then` and `or_else` (and, or)
- comparison
- hash
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

## Tests

`tests/cpp/test_cxx_optional.cpp` compiles a small program the same way as
the existing optional driver: `#include <optional>` with `-isystem` on
`src/prism/pir/models/cxx`, and again without that include path, under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The
dependency dump names the model header `optional`.

An engaged `optional<int>` holding 42 prints `engaged 42` for `value_or(99)`,
and an empty one prints `empty 99`. A temporary uses the `&&` overload and
prints the same pair (`moved 42`, `movedE 99`). `Box` has a converting
constructor from `int` and no default constructor: an engaged `Box` holding
42 prints `held 42`, and an empty one prints `none 99`. The libstdc++ binary
prints the same lines. Without a usable clang++ the case is `NOTRUN`, same
as the other C++ model driver. `CMakeLists.txt` is unchanged;
`_prism_test_sources` already lists `tests/cpp/test_cxx_optional.cpp`.
