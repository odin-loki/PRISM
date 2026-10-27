# variant slice

Branch `finish/cxx-variant`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.
`std::variant` is a header file, `src/prism/pir/models/cxx/variant`, the same
shape as `vector`: `_GLIBCXX_VARIANT`, `#pragma GCC system_header`,
`namespace std`. A directory of that name would make `#include <variant>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. PIR was not run. The yml files record the contract
each harness asks for.

## What the model covers

A discriminated union. One alternative is stored inline, in a union, inside
the variant object. There is no heap allocation. `index()` is that
alternative. `valueless_by_exception()` is always false: this model does not
represent a valueless state.

`emplace` destroys the previous alternative, then placement-news the new one
in the same storage. Assignment of a different alternative does the same.
Assignment of the same alternative assigns through the live object when that
alternative is assignable, and otherwise destroys and constructs. The
previous object is not leaked, and the two alternatives are not both alive.
A move leaves the source alternative alive (moved-from), not valueless.
Self-assign and self-move are no-ops.

`std::get<I>` and `std::get<T>` return the stored object when that
alternative is active. When it is not, `__prism_check` fires with class
`CXX-VARIANT-GET` and message `std::get on an inactive variant alternative`.
The function does not return a default-constructed `T` and does not return a
different alternative: after the check it names the requested alternative's
storage. `std::get_if` returns `nullptr` when the pointer is null or the
alternative is inactive, and a pointer to the live object when it is active.
`holds_alternative` is the index test. `variant_size` and
`variant_alternative` are present.

A converting constructor or assignment picks the alternative with an overload
set of `fn(T)` by value. A `T` that decays to the variant itself is not that
constructor. Duplicate alternatives (`variant<int, int>`) make that overload
set ill-formed if it is instantiated; `emplace<I>` still names an index.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and `lower_to_ir`
lowers the unit again with libstdc++. They are not stubbed:

- `visit`
- comparison (`operator==`, `operator<=>`, and the relational operators)
- `hash`, `monostate`, member `swap`
- `in_place` constructors and `initializer_list` emplace
- `bad_variant_access`

A constructor that throws after the previous alternative has been destroyed
is not modelled. The standard would make the variant valueless; this model
does not. The success path never leaves two alternatives alive. The
converting constructor does not reject list-initialization narrowing the way
the standard does.

Includes are `<cstddef>` and `<new>` only.

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `PRISM_PIR_MODELS`
gained one line, `src/prism/pir/models/cxx/variant`, so the embed name is
`cxx/variant`. `_prism_test_sources` gained `tests/cpp/test_cxx_variant.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"variant"` in the public-header
array. The skips for `iostream/prism_ostream.hpp` and `deque/deque` are
unchanged. No other model file was edited.

## Tests

`tests/cxx_models/variant_trace.cpp` includes `<variant>`. clang++ 18,
libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`,
compiled twice (`-isystem` the model directory, and without it). Happy-path
stdout matches, including construction, move-assignment, and destruction of
a user type. `std::get` of an inactive alternative, by type and by index,
aborts on the model binary with `CHECK CXX-VARIANT-GET` and does not print
a value. libstdc++ throws there, so that path is checked only on the model.

`tests/cpp/test_cxx_variant.cpp` is that comparison as a doctest. Without a
usable clang++ it is `NOTRUN`. The stored value is the one that was
emplaced, `get_if` on the inactive alternative is nullptr, and a wrong
`get` is a check.

`testdata/conformance/prism/cxx/cxx_variant_true.cpp` reads only the
alternative that was stored (`memsafety` true). `cxx_variant_false.cpp`
calls `std::get<char>` when that alternative is inactive (`false`, witness
`[1]`). The expected verdicts are YAML booleans.
`testdata/conformance/libc-models/variant_contracts.cpp` is the libc-contract
harness the header check requires: the active read is true, the inactive
`get` is `CXX-VARIANT-GET`. Those TUs compile against the model header.
PIR was not run.
