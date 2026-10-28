# optional and_then

Branch `finish/cxx-optional-and-then`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::optional::and_then` is declared on the existing header
`src/prism/pir/models/cxx/optional`. The header is still a file, not a
directory. `value()` and `value_or` are unchanged.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What and_then does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The call is direct. This
header does not include `<functional>` or `<memory>`.

- When the optional holds a `T`, `and_then` calls the function with that
  stored `T` and returns what the function returns. The `&` overload passes
  an lvalue, `const&` a const lvalue, `&&` an rvalue, and `const&&` a const
  rvalue.
- When the optional is disengaged, the function is not called and the
  inactive `T` is not read. The result is a value-initialized optional of
  the function's return type. That optional is disengaged. It is not `T{}`.
- The function must return an `optional`. A function that returns `T` fails
  to compile against this header (`static_assert` on the return type).
- A disengaged `and_then` is not a precondition failure. `value()` on a
  disengaged optional is still `__prism_check` with class `CXX-OPTIONAL-NULL`,
  and that path still does not return `T{}`. `value_or` still returns the
  stored `T` when engaged and the argument when disengaged.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`
- `transform`
- `or_else`
- comparison
- hash
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

A pointer-to-member is not a direct call, so that callable fails to compile
against this header and falls back too.

## Tests

`tests/cxx_models/optional_and_then_trace.cpp` is compiled the same way as
the existing optional driver: `#include <optional>` with `-isystem` on
`src/prism/pir/models/cxx`, and again without that include path, under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The
dependency dump names the model header `optional`.

An engaged `optional<int>` holding 42 calls `add1` and prints `engaged 1 43`.
An empty one does not call it and prints `empty 0` (`calls 1`). A function
that returns an empty optional prints `make_empty 42` then `cleared 0`; the
empty optional does not call it (`still 0`, `calls2 1`). `to_box` returns
`optional<Box>` for 42 (`held 1 42`). `Box` has no default constructor, and
the empty path prints `none 0`. A function that returns `optional<int>(0)`
prints `zero 1 0`; the empty path prints `zeroE 0` and does not call it. Two
`add1` calls print `chain 1 44` (`calls3 2`); an empty chain prints
`chainE 0`. `Cat` distinguishes the four ref-qualifiers (`lv 1`, `clv 3`,
`rv 2`, `crv 4`). Empty optionals of each category print `elv 0`, `eclv 0`,
`erv 0`, and `ecrv 0`. An engaged `Box` holding 42 prints `box 1 43`. An
empty `Box` prints `boxE 0`.

clang++ 18.1.3 produced the same lines from libstdc++ and from this header.
The same run of the older trace still matches on the happy path, and `argv`
`empty` still aborts with `CHECK CXX-OPTIONAL-NULL` and does not print a
value. A `value_or` program printed the same six lines as libstdc++
(`engaged 42`, `empty 99`, `moved 42`, `movedE 99`, `held 42`, `none 99`).
Without a usable clang++ the doctest case is `NOTRUN`, same as the other
C++ model driver. `CMakeLists.txt` is unchanged; `_prism_test_sources`
already lists `tests/cpp/test_cxx_optional.cpp`. The new trace is compiled
by that driver, not by CMake. The conformance header array was not edited.

`tests/cxx_models/optional_trace.cpp` does not call `and_then`. Its comment
lists `emplace`, `transform`, `or_else`, comparison, and hash as undeclared.
