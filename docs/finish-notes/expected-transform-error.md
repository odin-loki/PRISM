# expected transform_error

Branch `finish/cxx-expected-transform-error`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.
`std::expected::transform_error` is declared on the existing header
`src/prism/pir/models/cxx/expected`. The header is still a file, not a
directory. `value()`, `operator*`, `operator->`, and `error()` are unchanged:
the null check is still `__prism_check` with class `CXX-EXPECTED-NULL`, and
the error check is still `CXX-EXPECTED-ERROR`. Neither check returns a
default `T` or a default `E`. `and_then`, `or_else`, `value_or`, and
`transform` are unchanged. `and_then` on an error still does not call the
function and returns that same `E`. `or_else` on a value still does not call
the function and returns the stored `T`. `value_or` on an error still returns
the argument converted to `T`. `transform` on an error still does not call
the function and returns that same `E` through `_M_from_unex`.

This header does not include `<memory>`.

Status is not PROVED and not CLEAN. PIR was not run. The solver was not run.
cmake, ninja, and `prism_tests` were not run.

## What transform_error does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The function is called
with the stored `E` in that value category. The result type is
`expected<T, decltype(f(E))>`. That decltype is unevaluated, so naming the
inactive alternative does not read it.

- When the expected holds an `E`, `transform_error` calls the function with
  that `E` and returns an expected that holds the function's result as the
  error. The error alternative is built through `unexpected` and the existing
  expected constructor that takes `unexpected`. The value alternative of the
  result is not constructed. `T{}` is not returned.
- When the expected holds a `T`, `transform_error` does not call the function.
  It returns an expected that holds that same `T` (copied on `&`, `const&`,
  and `const&&`; moved on `&&`). The error alternative of the result is not
  constructed. `E{}` is not returned.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`, `swap`, comparison
- `expected<void, E>`

## Tests

`tests/cxx_models/expected_trace.cpp` includes `<expected>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The previous happy path,
including `and_then`, `or_else`, `value_or`, and `transform`, is unchanged.
The new lines:

- an unexpected `expected<int, int>` holding 9, `transform_error` of `e * 2`,
  prints `transform_error 0 18`; the source still prints
  `transform_error_src_err 9`
- an engaged value holding 42 does not call the function and prints
  `transform_error_val 0 1 42`; the source still prints `transform_error_src 42`
- a stored 0 does not call the function (no `skipZt`) and prints
  `transform_error_zero 1 0`
- an unexpected holding 0 calls the function and prints
  `transform_error_err0 0 99` (the function result, not a value-initialized
  error)
- an unexpected holding 5 whose function returns 0 prints
  `transform_error_zero_err 0 0` (that 0 is the error, not a value)
- const lvalues print `transform_error_const_val 42` and
  `transform_error_const 0 10`; the value path does not print `skipTeC`
- a temporary holding 11 prints `transform_error_rv_val 11` and does not
  print `skipRvT`; a temporary unexpected prints `transform_error_rv 0 7`
- a const rvalue holding 8 prints `transform_error_cmv_val 1 8` and does not
  print `skipCmvT`; a const rvalue unexpected prints `transform_error_cmv 0 5`
- `Box` has no default constructor. An unexpected `expected<Box, int>` prints
  `transform_error_not 0 9` and does not construct a `Box`. An engaged `Box`
  prints `transform_error_held 1 6` and does not print `skipBox`. `T{}` would
  not compile
- an unexpected transformed to `Box` prints `transform_error_box 9`. An
  engaged `int` whose function would return `Box` does not call it and prints
  `transform_error_box_val 0 42`. `E{}` would not compile
- `transform_error` on an unexpected `expected<int, R>`, where `R` has no
  default constructor, prints `terr 3` and `transform_error_r 0 4`
- `transform_error` on an engaged `expected<int, R>` does not call the
  function (no `callTe`) and prints `transform_error_kept 1 42`
- the four value categories of the stored error print `transform_error_cat 11`,
  `transform_error_cat_const 21`, `transform_error_cat_rv 31`, and
  `transform_error_cat_cmv 41`

`argv` `value`, `star`, and `error` are unchanged. The model binary aborts
with `CHECK CXX-EXPECTED-NULL` or `CHECK CXX-EXPECTED-ERROR` and does not
print `fake=`.

`tests/cpp/test_cxx_expected.cpp` is that comparison as a doctest. It checks
those lines. Without a usable clang++ it is `NOTRUN`, same as the other C++
model driver. `CMakeLists.txt` is unchanged; `_prism_test_sources` already
lists `tests/cpp/test_cxx_expected.cpp`. The conformance header array is
unchanged.
