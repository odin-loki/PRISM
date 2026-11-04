# charconv result not-equal

Branch `finish/cxx-charconv-ne`, base `22a53ea9dede93802c6de074d41853ed937f8b4f`.

`operator!=` for `std::from_chars_result` and `std::to_chars_result` is on
the existing header file `src/prism/pir/models/cxx/charconv`. The header is
still a file, not a directory. The include guard is still
`_PRISM_MODEL_CHARCONV` and `_GLIBCXX_CHARCONV`. `#pragma GCC system_header`.
Namespace `std`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run. This is not PROVED and not CLEAN. Comparing two charconv
results for inequality is not a proof, and it is not a fuzzer `CLEAN`
result.

Port note: on main, `from_chars` and `to_chars` for `bool` are both
`= delete`. The bool `from_chars` slice (`c164ba71c`) was not ported: the
standard library has no such overload, so a model body for it would accept
code that libstdc++ rejects. The header contains no `PROVED`, no `CLEAN`,
and no `__builtin_trap()`, and the new functions and their comments add
none. The honesty note is this file. Statements below that mention bool
`from_chars` or `tests/cpp/test_cxx_charconv_bool.cpp` describe the
original branch; `tests/cpp/test_cxx_charconv_ne.cpp` locks the main state.

## What not-equal does

Two friends, one on each result type, each `constexpr` and `noexcept`:

```
friend constexpr bool operator!=(const from_chars_result& __a, const from_chars_result& __b) noexcept {
    return !(__a == __b);
}
```

```
friend constexpr bool operator!=(const to_chars_result& __a, const to_chars_result& __b) noexcept {
    return !(__a == __b);
}
```

Each body calls the existing `operator==` and does not compare `ptr` or `ec`
again. It does not dereference `ptr`. A null ptr compares by address only.
The return type is `bool`, not `from_chars_result` and not `to_chars_result`.

`operator==` is still `return __a.ptr == __b.ptr && __a.ec == __b.ec;`.
Pointer equality is the address. Two null pointers are the same address, so
they compare equal when `ec` also matches, and `operator!=` is false. A null
pointer and a non-null pointer compare equal only if that address check
fails, so `operator!=` is true. A different `ec` makes `operator==` false
even when both pointers are null, so `operator!=` is true. The null pointer
is not loaded. A one-past-the-end `char*` is compared as an address and is
not loaded.

Equal results make `operator==` true, so `operator!=` is false. A result
compared with itself is not unequal. There is no floating-point overload.

## What stayed

`operator==`, integer `from_chars`, and integer `to_chars` are unchanged.
`from_chars` and `to_chars` for `bool` are still

```
from_chars_result from_chars(const char*, const char*, bool&, int = 10) = delete;
to_chars_result to_chars(char*, char*, bool, int = 10) = delete;
```

Those are the only two `= delete`. There is no `__builtin_trap()` in the
header. Integer `from_chars` and `to_chars` still do not trap.

`std::chars_format` and the floating-point overloads of `from_chars` and
`to_chars` are not declared. There is no `float`, `double`, or `long double`
overload. A use still fails to compile against this header and pir falls
back to libstdc++. They are omitted so this model does not invent a rounded
value.

Not-equal does not call `__prism_check` or `__builtin_trap`.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`). They
are not stubbed:

- `std::chars_format`
- floating-point `from_chars`
- floating-point `to_chars`
- `from_chars` and `to_chars` for `bool` (still `= delete`)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/charconv`. `CMakeLists.txt` gained one
`_prism_test_sources` line, immediately after
`tests/cpp/test_cxx_charconv.cpp`:

```text
tests/cpp/test_cxx_charconv_ne.cpp
```

No existing line was removed. `tests/cpp/test_cxx_charconv.cpp` is still
listed once.
`tests/cpp/test_qa_conformance.cpp` was not edited. `src/prism/bmc.cpp` and
`src/prism/bmc_unenc.inc` were not edited.
`testdata/conformance/libc-models/charconv_contracts.cpp` and its yml were
not edited. The trace `tests/cxx_models/charconv_ne_trace.cpp` is compiled
by the new driver, not by CMake.

## Tests

`tests/cpp/test_cxx_charconv_ne.cpp` reads the header. Each `operator!=` is
declared once, immediately after that type's `operator==`, as
`return !(__a == __b)`. That body does not contain `!=`, `ptr`, `ec`, a
dereference, either result type, `__prism_check`, or `__builtin_trap`. The
comment above each operator says the body does not compare `ptr` or `ec`
again, does not dereference `ptr`, and that a null ptr compares by address
only. Those comments do not contain `PROVED`, `CLEAN`, or `__builtin_trap`.
The header has no `PROVED`, `CLEAN`, or `__builtin_trap()`, and both bool
overloads are still deleted, after both result types. After comments are
stripped, `operator!=` occurs twice, and `chars_format`, `float`, `double`,
and `long double` are absent. `operator==` is still the previous return, and
that field comparison occurs twice in the header, not four times. The trace
does not call `from_chars` or `to_chars`.

`tests/cxx_models/charconv_ne_trace.cpp` includes `<charconv>`. It builds
results directly. `constexpr` checks cover two equal `from_chars_result`
values, a result compared with itself, a different pointer, a different
`errc`, both fields different, two null pointers with the same `errc`, two
null pointers with a different `errc`, and a null pointer against a non-null
pointer. The same null cases are `constexpr` for `to_chars_result`. The
return type of each `!=` is `bool`. A null load is not a constant
expression, so those `static_assert`s do not compile if `ptr` is
dereferenced. Runtime lines repeat those cases and, for `to_chars_result`,
compare a pointer to one `char` with one past that `char`.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<charconv>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched. ASan stayed quiet. Both binaries exited
0. Not-equal lines:

```
from same ne=0 eq=1 neg=1
from self ne=0 eq=1 neg=1
from ptr ne=1 eq=0 neg=1
from ec ne=1 eq=0 neg=1
from both ne=1 eq=0 neg=1
from null ne=0 eq=1 neg=1
from nullec ne=1 eq=0 neg=1
from nullp ne=1 eq=0 neg=1
to same ne=0 eq=1 neg=1
to self ne=0 eq=1 neg=1
to ptr ne=1 eq=0 neg=1
to ec ne=1 eq=0 neg=1
to both ne=1 eq=0 neg=1
to null ne=0 eq=1 neg=1
to nullec ne=1 eq=0 neg=1
to nullp ne=1 eq=0 neg=1
to cell=Q
```

`ne=0` with `eq=1` is not unequal. `ne=1` with `eq=0` is unequal. `neg=1`
is `ne == !eq` for that pair. `from null` is two null `from_chars_result`
values with `errc{}`: not-equal is false, and the pointer was not loaded.
`from nullec` is two null pointers with different `errc` values: not-equal
is true. `from nullp` is a null pointer against a non-null pointer.
`to ptr` compares a `char*` with one past that character. `to cell=Q` is
the byte at the pointer: it was not written. `from ec` is the same pointer
with `errc::result_out_of_range`. `to ec` is the same pointer with
`errc::value_too_large`.

libstdc++ 14 `<charconv>` in this mode has no separate `operator!=`.
`operator==` is defaulted when `__cplusplus > 201703L` and
`__cpp_impl_three_way_comparison >= 201907L`. In C++23, `!=` is the rewritten
negation of that `operator==`. This header declares the two friends.

The doctest driver uses the same sanitizer flags as
`tests/cpp/test_cxx_charconv.cpp`, including `-w` in place of
`-Wall -Wextra -Werror`. The manual trace compile above is the `-Werror`
run. The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`, `-Wall -Wextra -Werror`, and run.
Three cases passed (137 assertions). `prism_tests` was not run.
