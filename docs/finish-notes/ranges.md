# ranges slice

Branch `finish/cxx-ranges`, base `1278b9dc714ceaee54277a298c50212376942489`.

`std::ranges` is the extensionless file `src/prism/pir/models/cxx/ranges`.
A directory of that name would make `#include <ranges>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_RANGES`, so libstdc++'s own header is not also included. The file
is a `#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`. This header is not a proof that a range is sorted
or that a pointer has a length.

## What the model covers

`std::ranges::size` and `std::ranges::ssize` only.

`size` of a `T(&)[N]` returns `N` (`std::size_t`). `size` of an lvalue
whose const `size()` member is callable returns that `size()`. The call is
`static_cast<const T&>(t).size()`, so the const member is the one that
runs. `ssize` is that same value converted to `ptrdiff_t`.

A value that does not fit in `ptrdiff_t` calls `__prism_check` with class
`FUNC-CONTRACT` and message `ranges::ssize size does not fit in ptrdiff_t`,
then `__builtin_trap`. That path does not return. A large unsigned size is
not cast to a negative `ptrdiff_t`. Unsigned sizes narrower than
`ptrdiff_t` fit, so they are cast. A signed size wider than `ptrdiff_t` is
checked against both `PTRDIFF_MAX` and `PTRDIFF_MIN`.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What falls back

A pointer argument, including a pointer to an array, has no overload. The
call fails to compile (`no matching function`), so pir falls back to
libstdc++. An rvalue does not bind. A type whose `size()` is not const
fails the const call and falls back the same way. They are not stubbed.

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`):

- `ranges::begin`, `ranges::end`, `ranges::sort`, `ranges::find`
- views

A type that is not an array and has no const `size()` member fails to
compile here. This header does not compute a length from iterators. It
does not include `<memory>`, `<vector>`, `<string>`, `<iterator>`,
`<algorithm>`, or `<span>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `PRISM_PIR_MODELS`
gained

```
src/prism/pir/models/cxx/ranges
```

so the embedded name is `cxx/ranges`. `_prism_test_sources` gained
`tests/cpp/test_cxx_ranges.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`"ranges"` next to the other public header names. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged. `iostream`
and `ostream` stay absent.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They still stop a unit that
names `std::ranges::views` (`C++ ranges views unencoded`) or `ranges::to`
(`C++ ranges::to unencoded`) before a header is inlined. The tasks in this
slice call `size` and `ssize` only, so those gates do not match them.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. It is a regular file.
The array overload returns `_Nm`. The member overload returns
`static_cast<const _Tp&>(__t).size()` and takes an lvalue. The only
`__prism_check` call is the `FUNC-CONTRACT` above, and it is followed by
`__builtin_trap` in a function that does not return. The conversion's only
return is `static_cast<std::ptrdiff_t>(__n)`, after that call. The header
text has no pointer parameter, and it does not declare `begin(`, `end(`,
`sort(`, or `find(`.

`tests/cxx_models/ranges_trace.cpp` is `#include <ranges>`. Checked with
clang++ 18.1.3 and libstdc++ 13, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched:

```
arr 4
sarr 4
box 7
sbox 7
cbox 3
scbox 3
```

Constant `size` and `ssize` of `int[4]` are 4, and of a constexpr object
whose const `size()` is 3 are 3, against both headers. `argv` `huge` is an
`unsigned long long` size of `PTRDIFF_MAX + 1`. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line. libstdc++ returns
`-9223372036854775808` (`PTRDIFF_MIN`).

`std::ranges::size(int*)` and `std::ranges::size(int (*)[4])` fail to
compile against this header. So do an rvalue, a non-const `size()`, and
`begin`, `end`, `sort`, and `find`.

`testdata/conformance/prism/cxx/cxx_ranges_true.cpp` checks an array of
bound 4 and a const `size()` member, including `ssize` of a value that
fits (`no-overflow`, `expected: true`). `cxx_ranges_false.cpp` calls
`ssize` on a size of `PTRDIFF_MAX + 1` when the input is non-zero
(`expected: false`, witness `[1]`). Both `expected` values are YAML
booleans. The false task is `sanitizer_blind`: a UBSan build of that
function against libstdc++ returns 0. The `ptrdiff_t` result is
`PTRDIFF_MIN`, and the conversion to `int` is 0, with no sanitizer
diagnostic. The model takes the check and the trap instead, and does not
return that value.

`testdata/conformance/libc-models/ranges_contracts.cpp` is the
`#include <ranges>` the header case searches for.
