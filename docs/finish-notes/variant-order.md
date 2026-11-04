# variant ordering

Same-type `operator<`, `operator>`, `operator<=`, and `operator>=` live on
the existing header `src/prism/pir/models/cxx/variant`. The header is still
a file, not a directory. A directory of that name would make
`#include <variant>` fail with EISDIR and every such unit would fall back
to libstdc++.

Nothing here is `PROVED`. Nothing here is `CLEAN`. Ordering two variants
is not a proof of a program, and it is not a fuzzer `CLEAN` result.

`operator==` and `operator!=` are unchanged. `std::monostate`'s six
operators are unchanged. Those monostate operators compare `monostate`
values. They do not compare two `variant` objects. `visit` of one or more
lvalue variants stays modelled. `valueless_by_exception()` is still always
false. One alternative is still stored inline.

## What the four operators do

Each takes two `const variant<_Types...>&` arguments, so both objects are
the same type. A `variant<int>` compared with a `variant<char>` is not a
candidate. Each operator is constexpr. Each requires every alternative to
be comparable with `<`. None of them constructs `T{}` or a `monostate`
stand-in for a value.

`operator<` returns before it reads storage:

- Both `valueless_by_exception()`: false. That path does not read storage.
- A valueless left side and a valued right side: true. That path does not
  read the stored alternative.
- A valued left side and a valueless right side: false. That path does not
  read storage.
- `index()` differs: the indexes are compared. That path does not read the
  inactive alternative.

The same index calls `_Prism_var_lt_at`. That helper returns
`_M_get<I>() < _M_get<I>()` only when both indexes are `I`. Any other
index recurses without reading. Past the last alternative it returns
false.

`operator>` is `return __b < __a`. It is `operator<` with the arguments
swapped. It does not read an inactive alternative itself. Both valueless
compare not-greater, because `operator<` returns false before it reads
storage.

`operator<=` is `return !(__b < __a)`. That is the negation of `operator>`
of the swapped arguments. It uses the existing `operator<`. Both valueless
compare less-or-equal, because that `operator<` returns false.

`operator>=` is `return !(__a < __b)`. It uses the existing `operator<`.
Both valueless compare greater-or-equal, because that `operator<` returns
false.

This model never produces a valueless variant. The valueless arms stay in
`operator<`, ahead of any `_M_get`. `operator>`, `operator<=`, and
`operator>=` reach those arms only through that call.

## What stayed omitted

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++:

- `swap`
- `operator<=>` on `variant` and on `monostate`
- `hash`, including `std::hash<std::monostate>`
- `in_place` constructors
- `bad_variant_access`

An alternative that provides `>` and does not provide `<` is not a
candidate for `operator>`. libstdc++ accepts that comparison because its
`operator>` applies `>` to the stored values. This header drops the
candidate because `__x < __y` is not satisfied. The same split holds for
`<=` and `>=`: libstdc++ requires the matching operator on each
alternative, and this header requires `<`.

An alternative that provides `<` and does not provide `<=` is a candidate
for `operator<=` and `operator>=` here. libstdc++ rejects those
comparisons.

Includes stay `<cstddef>` and `<new>`. The header does not include
`<memory>` or `<compare>`.

## Tests

`tests/cpp/test_cxx_variant.cpp` reads the header without clang++. The
monostate operators are still the same lines. `operator<`, `operator>`,
`operator<=`, and `operator>=` for `const variant<_Types...>&` are present
once each. `operator>`'s body is `return __b < __a`. `operator<=`'s body
is `return !(__b < __a)`. `operator>=`'s body is `return !(__a < __b)`.
Those three bodies do not name `_M_get`. The less-than helper compares
with `<` only inside `index() == _I` for both objects. After comment
stripping, `swap`, `bad_variant_access`, `hash`, `in_place`, and
`operator<=>` are absent, `visit` is still present, and the words `PROVED`
and `CLEAN` are absent. The header text still says this is not PROVED and
not CLEAN.

`tests/cxx_models/variant_trace.cpp` does not call the new operators, so
its stdout compared with libstdc++ is unchanged.

With clang++, same-type ordering of `int`, `char`, duplicate `int`
alternatives, and `monostate` prints the same lines as libstdc++. A
different variant type fails to compile against both. Without a usable
clang++ those cases are `NOTRUN`. `CMakeLists.txt` was not edited.
`prism_tests` was not run.
