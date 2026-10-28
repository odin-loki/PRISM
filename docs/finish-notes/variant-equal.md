# variant equality

Branch `finish/cxx-variant-equal`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.
`operator==` and `operator!=` are on the existing header
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
`operator!=(monostate, monostate)` still returns false. The four relational
operators on `monostate` are unchanged. Those operators compare `monostate`
values. They do not compare two `variant` objects.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. Nothing here is `CLEAN`. Comparing two variants is not a proof
of a program, and it is not a fuzzer `CLEAN` result.

## What the two operators do

Both take two `const variant<_Types...>&` arguments, so both objects are
the same type. A `variant<int>` compared with a `variant<char>` is not a
candidate, and it does not return true. Each operator is constexpr. Each
requires every alternative to be comparable with `==`. Neither constructs
`T{}`.

`operator==` checks three paths before it reads storage:

- Both `valueless_by_exception()`: return true. That path does not read a
  stored alternative.
- One `valueless_by_exception()` and one that holds a value: return false.
  A valueless variant is not equal to one that holds a value. That path
  does not read a stored alternative.
- `index()` differs: return false. That path does not read the inactive
  alternative.

The same index calls `_Prism_var_eq_at`. That helper returns
`_M_get<I>() == _M_get<I>()` only when both indexes are `I`. Any other
index recurses without reading. Past the last alternative it returns
false, not true. The stored values are compared with `==`.

`operator!=` is `!(__a == __b)`. It does not read storage itself.

This model never produces a valueless variant, because
`valueless_by_exception()` returns false and `index()` is never
`variant_npos`. The valueless arms are still in `operator==`, ahead of
any `_M_get`.

## What stayed

`std::monostate` is still empty. Its six operators are the same functions
as before. `visit`, `swap`, `hash`, `in_place`, and `bad_variant_access`
are still not declared. `operator<`, `operator>`, `operator<=`,
`operator>=`, and `operator<=>` are still not declared for `variant`.
`operator<=>` is still not declared for `monostate`.

Includes stay `<cstddef>` and `<new>`. The header does not include
`<memory>` or `<compare>`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `visit`
- `swap`
- relational comparison of variant (`operator<`, `operator>`,
  `operator<=`, `operator>=`, and `operator<=>`)
- `hash`, including `std::hash<std::monostate>`
- `in_place` constructors
- `bad_variant_access`
- `operator<=>` on `monostate`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_variant.cpp`.
`tests/cpp/test_qa_conformance.cpp` already names `"variant"` in the
header array. That array was not edited. No other model file was edited.

## Tests

`tests/cpp/test_cxx_variant.cpp` reads the header without clang++. The
monostate operators are still the same lines. `operator==` and
`operator!=` for `const variant<_Types...>&` are present once each.
The valueless `return true` is before either-valueless `return false`,
which is before the different-index `return false`, which is before
`_Prism_var_eq_at`. That `operator==` body does not name `_M_get`,
`_M_ptr`, or `_M_storage`. The helper compares with `==` only inside
`index() == _I` for both objects, and its other return is false.
`operator!=` is the negation and does not name `_M_get`. After comment
stripping, `visit`, `swap`, `bad_variant_access`, `hash`, `in_place`,
and `operator<=>` are absent, and the words `PROVED` and `CLEAN` are
absent. The header text still says this is not PROVED and not CLEAN.

With clang++, the same file compiles `variant<int>` against
`variant<char>`, and `variant<int>` against `variant<long>`. Both `==`
and `!=` fail against libstdc++ and against this header. A
`variant<NoDef, char>` whose alternative has no default constructor
compiles for `==` and `!=` against both. A small program compiled twice
(`-isystem` the model directory, and without it), with
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`, prints
the same lines: equal ints, unequal ints, a different index, `!=` as the
negation, two `variant<int, int>` holding `1` at different indexes
unequal, the same index equal, two `monostate` alternatives equal, a
`monostate` alternative unequal to an `int`, and `NoDef` comparisons
`call 3 3` then `call 3 4` with no third call for the inactive
alternative. Without a usable clang++ those cases are `NOTRUN`.
`CMakeLists.txt` is unchanged. `prism_tests` was not run.
