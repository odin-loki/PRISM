# expected transform

Branch `finish/cxx-expected-transform`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.
`std::expected::transform` is declared on the existing header
`src/prism/pir/models/cxx/expected`. The header is still a file, not a
directory. `value()`, `operator*`, `operator->`, and `error()` are unchanged:
the null check is still `__prism_check` with class `CXX-EXPECTED-NULL`, and
the error check is still `CXX-EXPECTED-ERROR`. Neither check returns a
default `T` or a default `E`. `and_then`, `or_else`, and `value_or` are
unchanged. `and_then` on an error still does not call the function and
returns that same `E`. `or_else` on a value still does not call the function
and returns the stored `T`. `value_or` on an error still returns the
argument converted to `T`.

This header does not include `<memory>`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What transform does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The function is called
with the stored `T` in that value category.

- When the expected holds a `T`, `transform` calls the function with that `T`
  and returns an expected that holds the function's result. The result's
  value type is what the function returns.
- When the expected holds an `E`, `transform` does not call the function. It
  returns that same `E` through the private `_M_from_unex` (copied on `&`,
  `const&`, and `const&&`; moved on `&&`). The value alternative of the
  result is not constructed. `T{}` is not returned.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `transform_error`, `emplace`, `swap`, comparison
- `expected<void, E>`

## Tests

`tests/cxx_models/expected_trace.cpp` includes `<expected>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The previous happy path,
including `and_then`, `or_else`, and `value_or`, is unchanged. The new lines:

- an engaged `expected<int, int>` holding 42, `transform` of `v * 2`, prints
  `transform 1 84`
- an unexpected holding 9 does not call the function and prints
  `transform_unex 0 0 9`; the source still prints `transform_src 9`
- a stored 0 prints `transform_zero 1 0`
- an unexpected holding 0 does not call the function (no `skipZ`) and prints
  `transform_err0 0 0` (the stored error, not 99)
- const lvalues print `transform_const 45` and `transform_const_unex 0 0 3`
- a temporary holding 11 prints `transform_rv 15`; a temporary unexpected
  prints `transform_rv_unex 0 6` and does not print `skipRv`
- a const rvalue holding 8 prints `transform_cmv_val 1 9`; a const rvalue
  unexpected prints `transform_cmv 0 4` and does not print `skipCmv`
- `transform` on an unexpected `expected<E, int>`, where `E` has no default
  constructor, prints `transform_not 0 4` and does not print `skipEt`
- `transform` on an engaged `E` prints `xform 6` and `transform_val 1 6`
- `Box` has no default constructor. An engaged `int` transformed to `Box`
  prints `transform_box 42`. An unexpected prints `transform_box_unex 0 0 9`
  and does not construct a `Box`. `T{}` would not compile
- `transform` on an unexpected `expected<int, R>`, where `R` has no default
  constructor, prints `transform_r 0 3` and does not print `skipEr`

`argv` `value`, `star`, and `error` are unchanged. The model binary aborts
with `CHECK CXX-EXPECTED-NULL` or `CHECK CXX-EXPECTED-ERROR` and does not
print `fake=`.

`tests/cpp/test_cxx_expected.cpp` is that comparison as a doctest. It checks
those lines. Without a usable clang++ it is `NOTRUN`, same as the other C++
model driver. `CMakeLists.txt` is unchanged; `_prism_test_sources` already
lists `tests/cpp/test_cxx_expected.cpp`. The conformance header array is
unchanged.
