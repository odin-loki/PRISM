# expected and_then and or_else

Branch `finish/cxx-expected-and-then`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::expected::and_then` and `std::expected::or_else` are declared on the
existing header `src/prism/pir/models/cxx/expected`. The header is still a
file, not a directory. `value()`, `operator*`, `operator->`, and `error()`
are unchanged: the null check is still `__prism_check` with class
`CXX-EXPECTED-NULL`, and the error check is still `CXX-EXPECTED-ERROR`.
Neither check returns a default `T` or a default `E`.

PIR was not run. The solver was not run.

## What and_then does

Four overloads: `&`, `const&`, `&&`, and `const&&`. The function is called
with the stored `T` in that value category.

- When the expected holds a `T`, `and_then` calls the function with that `T`
  and returns what the function returns.
- When the expected holds an `E`, `and_then` does not call the function. It
  returns an expected that holds that same `E` (copied on `&`, `const&`, and
  `const&&`; moved on `&&`). The value alternative of the result is not
  constructed. A default `T` is not returned.

## What or_else does

Four overloads, same value categories. The function is called with the stored
`E` in that value category.

- When the expected holds an `E`, `or_else` calls the function with that `E`
  and returns what the function returns.
- When the expected holds a `T`, `or_else` does not call the function. It
  returns that stored `T` inside an expected (copied on `&`, `const&`, and
  `const&&`; moved on `&&`). The error alternative of the result is not
  constructed. A default `E` is not returned.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `transform`
- `value_or`
- `transform_error`, `emplace`, `swap`, comparison
- `expected<void, E>`

## Tests

`tests/cxx_models/expected_trace.cpp` includes `<expected>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The previous happy path
is unchanged. The new lines:

- an engaged `expected<int, int>` holding 42, `and_then` of `v * 2`, prints
  `and_then 1 84`
- an unexpected holding 9 does not call the function and prints
  `and_then_unex 0 0 9`
- `or_else` on that engaged value does not call the function and prints
  `or_else 0 1 42`
- `or_else` on the unexpected calls the function and prints `or_else_unex 1 11`
- `and_then` on an unexpected `expected<E, int>`, where `E` has no default
  constructor, prints `and_then_not 0 4` and does not print `callT`
- `or_else` on an engaged `expected<int, R>`, where `R` has no default
  constructor, prints `or_else_not 1 42` and does not print `callR`
- `and_then` on an engaged `E` prints `got 6` and `and_then_val 6`
- `or_else` on an engaged `E` prints `or_else_e 1 2` and does not print `callEo`
- `and_then` on an unexpected `R` prints `and_then_r 0 3` and does not print
  `callRu`

Const and rvalue overloads print `and_then_const 45`, `or_else_const 10`,
`and_then_rv 15`, `or_else_rv 7`, `and_then_rv_unex 0 6`, `or_else_rv_val 1 8`,
`and_then_cmv 0 4`, and `or_else_cmv 1 8`. The skipped calls do not print
`callRv`, `callOr`, `callCmv`, or `callCOr`.

`argv` `value`, `star`, and `error` are unchanged. The model binary aborts
with `CHECK CXX-EXPECTED-NULL` or `CHECK CXX-EXPECTED-ERROR` and does not
print `fake=`.

`tests/cpp/test_cxx_expected.cpp` is that comparison as a doctest. It checks
those lines. Without a usable clang++ it is `NOTRUN`, same as the other C++
model driver. `CMakeLists.txt` is unchanged; `_prism_test_sources` already
lists `tests/cpp/test_cxx_expected.cpp`.
