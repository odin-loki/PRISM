# optional transform

Branch `finish/cxx-optional-transform`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.
`std::optional::transform` is declared on the existing header
`src/prism/pir/models/cxx/optional`. The header is still a file, not a
directory. `value()`, `value_or`, `and_then`, and `or_else` are unchanged.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What transform does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The call is direct. This
header does not include `<functional>` or `<memory>`.

- When the optional holds a `T`, `transform` calls the function with that
  stored `T` and returns an optional that holds what the function returns.
  The `&` overload passes an lvalue, `const&` a const lvalue, `&&` an rvalue,
  and `const&&` a const rvalue. cv on that result object is removed, so a
  function that returns `const int` yields `optional<int>`.
- When the optional is disengaged, the function is not called and the
  inactive `T` is not read. The result is a disengaged optional of the
  function's result type. It is not `T{}` and it does not hold a
  value-initialized result.
- The result is not flattened. A function that returns `optional<int>` yields
  `optional<optional<int>>`. That outer optional is engaged when this one
  was, and it holds the optional the function returned.
- The function must return an object type. A reference or `void` fails to
  compile against this header (`static_assert`).
- A disengaged `transform` is not a precondition failure. `value()` on a
  disengaged optional is still `__prism_check` with class `CXX-OPTIONAL-NULL`,
  and that path still does not return `T{}`. `value_or` still returns the
  stored `T` when engaged and the argument when disengaged. `and_then` on a
  disengaged optional still does not call the function and still returns a
  disengaged optional. `or_else` on an engaged optional still does not call
  the function and still returns the stored `T`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`
- comparison
- hash
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

A pointer-to-member is not a direct call, so that callable fails to compile
against this header and falls back too.

## Tests

`tests/cxx_models/optional_transform_trace.cpp` is compiled the same way as
the existing optional driver: `#include <optional>` with `-isystem` on
`src/prism/pir/models/cxx`, and again without that include path, under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The
dependency dump names the model header `optional`.

An engaged `optional<int>` holding 42 calls `add1` and prints `engaged 1 43`.
An empty one does not call it and prints `empty 0` (`calls 1`). A stored 0
is engaged: `add1` runs and prints `zeroIn 1 1` (`calls2 1`). A function that
returns 0 prints `zero 1 0`; the empty path prints `zeroE 0` and does not
call it. A function that returns a null `int*` prints `nullp 1 1` (engaged,
holding null). The empty path prints `nullpE 0`. `to_box` returns `Box` for
42 (`to_box 42`, then `held 1 42`). `Box` has no default constructor. The
empty path prints `none 0` and does not print `to_box`. A function that
returns `const int` prints `cv 43`; `value()` is an `int&`. The empty path
prints `cvE 0`. A function that returns `optional<int>` prints `wrap 1 1 43`.
The empty path prints `wrapE 0`. Two `add1` calls print `chain 1 44`
(`calls3 2`); an empty chain prints `chainE 0`. `Cat` distinguishes the four
ref-qualifiers (`lv 1`, `clv 3`, `rv 2`, `crv 4`). Empty optionals of each
category print `elv 0`, `eclv 0`, `erv 0`, and `ecrv 0`. An engaged `Box`
holding 42, passed as an lvalue, prints `box 1 43 42` (the source stays 42).
An empty `Box` prints `boxE 0`. The `&&` overload passes an rvalue: the
function moves from the stored `Box` and prints `moved 42 -1`. The `const&&`
overload passes a const rvalue and prints `cmove 5 5`.

clang++ 18.1.3 produced the same lines from libstdc++ and from this header.
The same run of the older optional trace, the `and_then` trace, and the
`or_else` trace still match. `argv` `empty` still aborts with
`CHECK CXX-OPTIONAL-NULL` and does not print a value. A `value_or` program
printed the same six lines as libstdc++ (`engaged 42`, `empty 99`,
`moved 42`, `movedE 99`, `held 42`, `none 99`). Without a usable clang++
the doctest case is `NOTRUN`, same as the other C++ model driver.
`CMakeLists.txt` is unchanged; `_prism_test_sources` already lists
`tests/cpp/test_cxx_optional.cpp`. The new trace is compiled by that
driver, not by CMake. The conformance header array was not edited.

`tests/cxx_models/optional_trace.cpp` does not call `transform`. Its comment
lists `emplace`, comparison, and hash as undeclared.
`tests/cxx_models/optional_and_then_trace.cpp` and
`tests/cxx_models/optional_or_else_trace.cpp` do not call `transform` either.
