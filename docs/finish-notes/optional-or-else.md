# optional or_else

Branch `finish/cxx-optional-or-else`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.
`std::optional::or_else` is declared on the existing header
`src/prism/pir/models/cxx/optional`. The header is still a file, not a
directory. `value()`, `value_or`, and `and_then` are unchanged.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What or_else does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The call is direct and
takes no arguments. This header does not include `<functional>` or `<memory>`.

- When the optional is disengaged, `or_else` calls the function and returns
  the optional that function returns. The inactive `T` is not read.
- When the optional holds a `T`, `or_else` does not call the function. It
  returns an optional that holds that stored `T`. The `&`, `const&`, and
  `const&&` overloads copy it. The `&&` overload moves it. The result is
  that `T`, not `T{}`, and not a value of some other type.
- The function must return `optional<T>` for this same `T`. A function that
  returns a different type fails to compile against this header
  (`static_assert` on the return type).
- A disengaged `or_else` is not a precondition failure. `value()` on a
  disengaged optional is still `__prism_check` with class `CXX-OPTIONAL-NULL`,
  and that path still does not return `T{}`. `value_or` still returns the
  stored `T` when engaged and the argument when disengaged. `and_then` on a
  disengaged optional still does not call the function and still returns a
  disengaged optional, not `T{}`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`
- `transform`
- comparison
- hash
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

A pointer-to-member is not a direct call, so that callable fails to compile
against this header and falls back too.

## Tests

`tests/cxx_models/optional_or_else_trace.cpp` is compiled the same way as
the existing optional driver: `#include <optional>` with `-isystem` on
`src/prism/pir/models/cxx`, and again without that include path, under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The
dependency dump names the model header `optional`.

An engaged `optional<int>` holding 42 does not call `make7` and prints
`engaged 1 42`. An empty one calls it and prints `empty 1 7` (`calls 1`).
A stored 0 is kept (`zero 1 0`, `calls2 0`). An empty optional whose
function returns `optional<int>(0)` prints `gotzero` then `zeroE 1 0`.
`make_empty` on the engaged value prints `kept 1 42` and does not print
`make_empty`; on the empty value it prints `make_empty` then `cleared 0`
(`calls3 1`). A chain that recovers 7 does not call the second function and
prints `chain 1 7`. An engaged chain prints `chainE 1 42` (`calls4 1`).

`Call` distinguishes the four value categories of the function. Disengaged
optionals print `call-lv` / `lv 1`, `call-clv` / `clv 3`, `call-rv` / `rv 2`,
and `call-crv` / `crv 4`. Engaged optionals do not print those call lines.
They print `keep-lv 42`, `keep-clv 42`, `keep-rv 42`, and `keep-crv 42`.

`Box` has no default constructor. An engaged `Box` holding 42 prints
`box 1 42 42` (the copy leaves the source at 42). An empty one prints
`makebox` then `boxE 1 9`. An empty one whose function returns an empty
optional prints `boxnone 0`. The `&&` overload moves and prints
`moved 42 -1`. The `const&&` overload copies and prints `cmove 5 5`.

clang++ 18.1.3 produced the same lines from libstdc++ and from this header.
The same run of the older optional trace and the `and_then` trace still
match on the happy path. `argv` `empty` still aborts with
`CHECK CXX-OPTIONAL-NULL` and does not print a value. A `value_or` program
printed the same six lines as libstdc++ (`engaged 42`, `empty 99`,
`moved 42`, `movedE 99`, `held 42`, `none 99`). Without a usable clang++
the doctest case is `NOTRUN`, same as the other C++ model driver.
`CMakeLists.txt` is unchanged; `_prism_test_sources` already lists
`tests/cpp/test_cxx_optional.cpp`. The new trace is compiled by that
driver, not by CMake. The conformance header array was not edited.

`tests/cxx_models/optional_trace.cpp` does not call `or_else`. Its comment
lists `emplace`, `transform`, comparison, and hash as undeclared.
`tests/cxx_models/optional_and_then_trace.cpp` does not call `or_else`
either.
