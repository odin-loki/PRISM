# expected compare and emplace

Branch `port/expected`, base `22398045cd30557e508abb238e07a2535576373c`.
Same-type `operator==`, `operator!=`, `operator<`, and one-argument
`emplace` are on the existing header `src/prism/pir/models/cxx/expected`.
The header is still a file, not a directory. A directory of that name would
make `#include <expected>` fail with EISDIR and every such unit would fall
back to libstdc++.

This re-implements the October models (equality `1a244d246`, emplace
`c2c514bd2`, less `f3e3ea07c`) on the current header. Those patches do not
apply. The union is still `{ _Tp _M_val; _Er _M_unex; }` with `bool _M_has`.

`value()`, `operator*`, `operator->`, and `error()` are unchanged. The null
check is still `__prism_check` with class `CXX-EXPECTED-NULL`. The error
check is still `CXX-EXPECTED-ERROR`. `and_then`, `or_else`, `value_or`,
`transform`, and `transform_error` are unchanged.

The header includes `<new>` for the placement new. It does not include
`<memory>` or `<compare>`.

This is not PROVED and not CLEAN. Comparing two expected objects, or
storing a `T` with `emplace`, is not a proof and it is not a fuzzer CLEAN
result. PIR was not run. The solver was not run. cmake, ninja, and
`prism_tests` were not run.

## What the new operations do

`operator==` and `operator<` are friend function templates, defined after
the class, so a `T` or an `E` with no such operator can still be stored.
Both take two `const expected<T, E>&` of the same `T` and the same `E`.

`operator==` returns false when the flags differ, before either alternative
is named. Both values compare the stored `T` with `==` and do not read `E`.
Both errors compare the stored `E` with `==` and do not read `T`. It does
not call `value()` or `error()` and it does not construct `T{}` or `E{}`.

`operator!=` is `!(__x == __y)`. That body does not name either alternative.

`operator<` returns false for a value on the left and an error on the right,
and true for an error on the left and a value on the right, both before
storage is named. Otherwise it compares the active alternative with `<`.

`emplace(_Up&&)` destroys the active alternative, placement-news `T` into
`_M_val`, sets the flag on the error path, and returns that stored `T`. The
value arm does not name `_M_unex`. The error arm does not name `_M_val`.
It does not call `value()` or `error()`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `swap`
- `operator>`, `operator<=`, `operator>=`, and `operator<=>`
- comparison of `expected<T, E>` with `expected<U, G>`
- comparison with a bare `T`
- comparison with `unexpected`
- `emplace` with no argument, with more than one argument, or with an
  `initializer_list`
- `expected<void, E>`

C++23 `<expected>` has `operator==` and does not have `operator<`. Same-type
`operator<` compiles against this header and fails against libstdc++. That
is not a differential result.

## Tests

`tests/cpp/test_cxx_expected.cpp` reads the header without a compiler. The
friend declarations are inside the class and the definitions are after it.
`operator!=` is not a friend. The unequal arm of `operator==`, and both
mixed arms of `operator<`, return before `_M_storage` is named. `emplace`
destroys `_M_val` or `_M_unex`, then placement-news `T`. After comments are
stripped, `swap`, `operator>`, and `expected<void` are absent, and
`emplace(` appears once.

`tests/cxx_models/expected_trace.cpp` still matches libstdc++ on the
previous lines. Equality prints `eq_val 1 0 1` through `ne_cross 1`. A
value `EqVal(8)` against an error `EqErr(2)` prints `eq_cross 0 0` and does
not print `eqv 8` or `eqe 2`. `operator!=` calls `operator==` once more, so
`eqv 3 3` and `eqe 5 6` each appear twice. `emplace` prints `one 1 2 1`,
`from_err 1 5 1`, `lv 3 3 1`, `rv -1 3 1`, `zero 1 0 1`, `box 1 42 1`, and
`wasnow 3 3`. The trace does not call `operator<`.

Checked with g++ 13.3.0 and libstdc++ (`__cpp_lib_expected` present) and
with clang++ 18.1.3 against this header. Flags were `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1 -w`. The differential trace matched.
`operator<` was run only against this header. `prism_tests` was not run.
