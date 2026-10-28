# monostate order

Branch `finish/cxx-monostate-order`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.
The four relational operators are on the existing header
`src/prism/pir/models/cxx/variant`. The header is still a file, not a
directory. A directory of that name would make `#include <variant>` fail
with EISDIR and every such unit would fall back to libstdc++.

`get`, `get_if`, and `holds_alternative` are unchanged. A wrong `std::get`
is still `__prism_check` with class `CXX-VARIANT-GET` and message
`std::get on an inactive variant alternative`. The function does not return
a default-constructed `T` and does not return a different alternative:
after the check it names the requested alternative's storage.
`valueless_by_exception()` is still always false. One alternative is still
stored inline, in a union, inside the variant object. There is no heap
allocation.

`operator==(monostate, monostate)` still returns true.
`operator!=(monostate, monostate)` still returns false.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. Ordering two `monostate` values is not a proof of a program.

## What the ordering is

`operator<`, `operator>`, `operator<=`, and `operator>=` each take two
`monostate` arguments by value. Each is constexpr and noexcept. Every
`monostate` compares equal, so `<` returns false, `>` returns false, `<=`
returns true, and `>=` returns true. Those operators compare `monostate`
values. They do not compare two `variant` objects.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `visit`
- `swap`
- comparison of variant (`operator==`, `operator<=>`, and the relational
  operators)
- `hash`, including `std::hash<std::monostate>`
- `in_place` constructors
- `bad_variant_access`
- `operator<=>` on `monostate`

Includes stay `<cstddef>` and `<new>`. The header does not include
`<memory>` or `<compare>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_variant.cpp`.
`tests/cpp/test_qa_conformance.cpp` already names `"variant"` in the
header array. That array was not edited. No other model file was edited.

## Tests

`tests/cxx_models/variant_trace.cpp` includes `<variant>`. clang++ 18.1.3
and libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, compiled twice (`-isystem` the model
directory, and without it). Happy-path stdout matches. Two `monostate`
objects print `mono eq=1 ne=0 lt=0 gt=0 le=1 ge=1`. A default
`variant<monostate, int>` prints
`mono_var index=0 holds=1 geteq=1 getne=0 valueless=0`. `static_assert`
requires `monostate` to be empty, `==` true, `!=` false, `<` and `>`
false, and `<=` and `>=` true. `std::get` of an inactive alternative
aborts on the model binary with `CHECK CXX-VARIANT-GET` and does not print
a value.

`tests/cpp/test_cxx_variant.cpp` is that comparison as a doctest, plus a
header read that does not need clang++: the four relational operators are
declared with those return values, and after comment stripping `visit`,
`swap`, `bad_variant_access`, `hash`, `in_place`, `operator<=>`, and
relational operators on `variant` are absent. A separate case compiles
small units that name variant comparison, `swap`, `visit`,
`bad_variant_access`, `hash<monostate>`, or `monostate`'s `operator<=>`.
Each one compiles against libstdc++ and fails against this header.
`monostate` `<`, `>`, `<=`, and `>=` compile against both. Without a
usable clang++ those compile cases are `NOTRUN`. `CMakeLists.txt` is
unchanged. `prism_tests` was not run, so the doctest `TEST_CASE` bodies
were not executed. Compiling the trace did evaluate the `static_assert`s.

clang++ 18.1.3 also syntax-checked the ordering translation unit and the
same `static_assert`s with `-std=c++23 -Wall -Wextra -Werror`, once against
libstdc++ and once with this header ahead of it. Both accepted them.
`operator<=>` on `monostate`, and `operator<` on `variant<int, char>`,
compiled against libstdc++ and failed against this header.
