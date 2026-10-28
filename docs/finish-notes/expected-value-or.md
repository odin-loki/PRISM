# expected value_or

Branch `finish/cxx-expected-value-or`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.
`std::expected::value_or` is declared on the existing header
`src/prism/pir/models/cxx/expected`. The header is still a file, not a
directory. `value()`, `operator*`, `operator->`, and `error()` are unchanged:
the null check is still `__prism_check` with class `CXX-EXPECTED-NULL`, and
the error check is still `CXX-EXPECTED-ERROR`. Neither check returns a
default `T` or a default `E`. `and_then` and `or_else` are unchanged.
`and_then` on an error still does not call the function and returns that
same `E`. `or_else` on a value still does not call the function and returns
the stored `T`.

This header does not include `<memory>`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What value_or does

Two overloads, `const&` and `&&`, each taking a forwarding reference. Same
shape as `optional::value_or`.

- When the expected holds a `T`, `value_or` returns that `T`. The `const&`
  overload copies it. The `&&` overload moves it. The fallback argument is
  not the result.
- When the expected holds an `E`, `value_or` returns the argument converted
  to `T` (`static_cast<T>` of the forwarded argument). That path does not
  read the inactive union member, does not return `T{}`, and does not invent
  a value that was not stored or passed in. It does not return the stored `E`.
- An unexpected `value_or` is not a precondition failure. `value()` on an
  unexpected expected still is. `error()` on a value still is.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `transform`
- `transform_error`, `emplace`, `swap`, comparison
- `expected<void, E>`

## Tests

`tests/cxx_models/expected_trace.cpp` includes `<expected>`. Compiled twice
(`-isystem` the model directory, and without it) under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The previous happy path,
including `and_then` and `or_else`, is unchanged. The new lines:

- an engaged `expected<int, int>` holding 42, `value_or(99)`, prints
  `value_or 42`
- an unexpected holding 9 prints `value_or_unex 99` (the argument, not 9
  and not 0)
- a temporary holding 42 prints `value_or_rv 42`; a temporary unexpected
  prints `value_or_rv_unex 99`
- a stored 0 prints `value_or_zero 0`, not the fallback 99
- const lvalues print `value_or_const 42` and `value_or_const_unex 99`
- `Box` has a converting constructor from `int` and no default constructor.
  An engaged `Box` holding 42 prints `value_or_held 42`. An unexpected prints
  `value_or_none 99`. `T{}` would not compile.

`argv` `value`, `star`, and `error` are unchanged. The model binary aborts
with `CHECK CXX-EXPECTED-NULL` or `CHECK CXX-EXPECTED-ERROR` and does not
print `fake=`.

`tests/cpp/test_cxx_expected.cpp` is that comparison as a doctest. It checks
those lines. Without a usable clang++ it is `NOTRUN`, same as the other C++
model driver. `CMakeLists.txt` is unchanged; `_prism_test_sources` already
lists `tests/cpp/test_cxx_expected.cpp`. The conformance header array is
unchanged.
