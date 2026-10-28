# variant monostate

Branch `finish/cxx-variant-monostate`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::monostate` is declared on the existing header
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

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. Two `monostate` values comparing equal is not a proof of a
program.

## What monostate is

An empty type in namespace `std`. It has no members and no bases.
`operator==(monostate, monostate)` is constexpr and noexcept and returns
true. `operator!=(monostate, monostate)` is constexpr and noexcept and
returns false. Both take the arguments by value. Those operators compare
`monostate` values. They do not compare two `variant` objects.

A `variant` whose first alternative is `monostate` still uses the existing
default constructor. That alternative is placement-newed in the same inline
union. `index()` is 0, `holds_alternative<monostate>` is true, and
`valueless_by_exception()` is false.

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
- `operator<=>` and the relational operators on `monostate` (`<`, `>`,
  `<=`, `>=`)

Includes stay `<cstddef>` and `<new>`. The header does not include
`<memory>` or `<compare>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_variant.cpp`.
`tests/cpp/test_qa_conformance.cpp` was not edited. No other model file
was edited.

## Tests

`tests/cxx_models/variant_trace.cpp` includes `<variant>`. clang++ 18,
libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`,
compiled twice (`-isystem` the model directory, and without it). Happy-path
stdout matches. Two `monostate` objects print `mono eq=1 ne=0`. A default
`variant<monostate, int>` prints
`mono_var index=0 holds=1 geteq=1 getne=0 valueless=0`. `std::get` of an
inactive alternative aborts on the model binary with `CHECK CXX-VARIANT-GET`
and does not print a value.

`tests/cpp/test_cxx_variant.cpp` is that comparison as a doctest, plus a
header read that does not need clang++: `monostate` is declared,
`operator==` returns true, `operator!=` returns false, and after comment
stripping `visit`, `swap`, `bad_variant_access`, `hash`, and `in_place`
are absent. A separate case compiles small units that name variant
comparison, `swap`, `visit`, `bad_variant_access`, `hash<monostate>`, or
`monostate`'s `operator<`. Each one compiles against libstdc++ and fails
against this header. Without a usable clang++ that case is `NOTRUN`.
`CMakeLists.txt` is unchanged.
