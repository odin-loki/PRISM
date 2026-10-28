# ranges empty

Branch `finish/cxx-ranges-empty`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::ranges::empty` is declared on the existing header
`src/prism/pir/models/cxx/ranges`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_RANGES`. `size` and
`ssize` are unchanged. A value that does not fit in `ptrdiff_t` still
calls `_Prism_ssize_does_not_fit`: `__prism_check` with class
`FUNC-CONTRACT` and message `ranges::ssize size does not fit in ptrdiff_t`,
then `__builtin_trap`. That path does not return a wrapped negative.
There is still no pointer overload of `size`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. This header is not a proof that a range is sorted or that a
pointer has a length.

## What empty does

`empty` of a `T(&)[N]` returns `_Nm == 0` (`bool`). The parameter is
unnamed, so the body does not load an element.

Clang rejects a parameter written `T(&)[0]` ("zero-length arrays are not
permitted in C++"). `N == 0` is a second overload: a reference constrained
with `sizeof(_Arr) == 0`, returning `true`. That parameter is unnamed too.
It does not load an element. When `T(&)[N]` is viable it is the more
specialized overload, so a bound other than 0 is not taken there. An
array `int[4][0]` has size 0 and bound 4, and `empty` is false. An array
`int[0][4]` has bound 0, and `empty` is true.

`empty` of an lvalue with a const `empty()` member returns that `empty()`.
The call is `static_cast<const _Tp&>(__t).empty()`, so the const member
is the one that runs.

## What falls back

A pointer argument, including a pointer to an array, has no overload.
`sizeof` of a pointer is not 0, so the zero-length overload does not take
it. The call fails to compile (`no matching function`), and pir falls
back to libstdc++. A pointer is not a range.

An rvalue does not bind. A type whose `empty()` is not const fails the
const call and falls back the same way. They are not stubbed. libstdc++
accepts both of those; this header does not.

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`):

- `ranges::begin`, `ranges::end`, `ranges::sort`, `ranges::find`
- views

A type that is not an array and has no const `empty()` member fails to
compile here. This header does not decide emptiness from `size() == 0`
on a class, and it does not compare iterators. It does not include
`<memory>`, `<vector>`, `<string>`, `<iterator>`, `<algorithm>`, or
`<span>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/ranges`. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_ranges.cpp`.
`tests/cpp/test_qa_conformance.cpp` was not edited. The `"ranges"` entry
in that header array stays. The skips for `iostream/prism_ostream.hpp`
and `deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. The array `empty`
returns `_Nm == 0` and that body has no `*`, no subscript, and no
member access. The zero-length overload returns `true` under
`requires(sizeof(_Arr) == 0)`, and that body does not name an element.
The member overload returns `static_cast<const _Tp&>(__t).empty()` and
takes an lvalue. There is no `empty` overload on a pointer. The ssize
check and trap are still the only `__prism_check`, and the conversion's
only return is still `static_cast<std::ptrdiff_t>(__n)`.

`tests/cxx_models/ranges_trace.cpp` is `#include <ranges>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched:

```
arr 4
sarr 4
box 7
sbox 7
cbox 3
scbox 3
earr 0
zempty 1
cearr 0
inner 0
outer 1
eflag 1
enflag 0
ecflag 1
eprefer 1
eptrs 0
```

`earr 0` and `cearr 0` are `int[4]` and `const int[4]`: not empty.
`zempty 1` is `int[0]`. `inner 0` is `int[4][0]`. `outer 1` is
`int[0][4]`. `eflag` / `enflag` / `ecflag` are a const `empty()` member
on a true lvalue, a false lvalue, and a const object. `eprefer 1` is a
type whose const `empty()` is true and whose const `size()` is 4, so
`empty` calls `empty()`. `eptrs 0` is an array of two null pointers: not
empty, and the run does not dereference them. Constant `empty` of
`int[4]` is false, of `int[0]` is true, and of a constexpr object whose
const `empty()` is true is true, against both headers. g++ 13 compiled
the same trace against this header.

`argv` `huge` is unchanged. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line.

`std::ranges::empty(int*)` and `std::ranges::empty(int (*)[4])` fail to
compile against this header. So do an rvalue, a non-const `empty()`, and
`begin`, `end`, `sort`, and `find`. `std::ranges::size(int*)` still
fails the same way.

`testdata/conformance/prism/cxx/cxx_ranges_true.cpp` still checks an
array of bound 4 and a const `size()` member, including `ssize` of a
value that fits. It also checks that the array is not empty, that an
array of bound 0 is empty, and that a const `empty()` member is that
member (`no-overflow`, `expected: true`, YAML boolean). The false task
is unchanged: `ssize` of `PTRDIFF_MAX + 1` (`expected: false`, witness
`[1]`). `testdata/conformance/libc-models/ranges_contracts.cpp` calls
those same `empty` cases. It is still the `#include <ranges>` the header
case searches for.
