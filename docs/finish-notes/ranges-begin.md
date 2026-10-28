# ranges begin

Branch `finish/cxx-ranges-begin`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.

`std::ranges::begin` and `std::ranges::end` are declared on the existing
header `src/prism/pir/models/cxx/ranges`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_RANGES`. `size`, `ssize`,
and `empty` are unchanged. A value that does not fit in `ptrdiff_t` still
calls `_Prism_ssize_does_not_fit`: `__prism_check` with class
`FUNC-CONTRACT` and message `ranges::ssize size does not fit in ptrdiff_t`,
then `__builtin_trap`. That path does not return a wrapped negative.
There is still no pointer overload of `size`. `empty` of a `T(&)[N]` is
still true only when `N == 0`, and that body still does not load an element.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. This is not a proof that an iterator stays in range. This
header is not a proof that a range is sorted or that a pointer has a length.

## What begin and end do

`begin` and `end` take a `T(&)[N]` and nothing else.

For a bound other than 0 the parameter is `T (&)[N]`. `begin` returns
`__a`. That is array-to-pointer decay: a pointer to the first element, and
the body does not load it. `end` returns `__a + _Nm`, a pointer one past
the last element. The addition does not load an element. A const array uses
the same templates, because `_Tp` is deduced as `const U`. The return type
is `_Tp*`.

`N == 0` does not match that parameter. Clang rejects `T (&)[0]` while
substituting the bound ("zero-length arrays are not permitted in C++"), the
same way it rejects that parameter for `empty`. The empty array is a second
overload: a reference constrained with `sizeof(_Arr) == 0`. `begin` returns
the decayed pointer. `end` returns that pointer plus zero. The two pointers
compare equal. Neither body loads an element. When `T(&)[N]` is viable it
is the more specialized overload, so a bound other than 0 is not taken
there.

An array `int[4][0]` has size 0 and bound 4. `begin` compares equal to the
array. The trace does not call `end` on that array: the element type has
size 0, and g++ rejects arithmetic on a pointer to an empty aggregate.
`empty` of that array is still false. An array `int[0][4]` has bound 0, and
`begin` and `end` are the same pointer.

A class with a `begin()` member has no overload. The call fails to compile
and pir falls back.

## What falls back

A pointer argument, including a pointer to an array, has no overload.
`sizeof` of a pointer is not 0, so the zero-length overload does not take
it. The call fails to compile (`no matching function`), and pir falls back
to libstdc++. A pointer is not a range.

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`):

- `ranges::sort`, `ranges::find`
- views

The header does not include `<iterator>`, `<algorithm>`, `<memory>`,
`<vector>`, `<string>`, or `<span>`. A later `++` or a dereference of the
returned pointer is the caller's expression. This header does not decide
whether that use stays inside the array.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/ranges`. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_ranges.cpp`.
`tests/cpp/test_qa_conformance.cpp` was not edited. The `"ranges"` entry
in that header array stays. The skips for `iostream/prism_ostream.hpp`
and `deque/deque` are unchanged.

`testdata/conformance/prism/cxx/cxx_ranges_true.cpp`,
`cxx_ranges_false.cpp`, and
`testdata/conformance/libc-models/ranges_contracts.cpp` were not edited.
They still call `size`, `ssize`, and `empty` only. `begin` and `end` are
not called there.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. It used to require
`begin(` and `end(` to be absent, and it used to require an array `begin`
and `end` to fail to compile. Those checks now require the four overloads.
The array `begin` returns `__a`, and that body has no `*`, no subscript,
no member access, and no addition. The array `end` returns `__a + _Nm`,
and that body has no `*`, no subscript, and no member access. The
zero-length `begin` returns `__a`. The zero-length `end` returns
`__a + 0`. Those bodies do not load an element. `_Tp*` occurs twice, both
return types, and there is no pointer parameter.
`requires(sizeof(_Arr) == 0)` occurs three times: `empty`, `begin`, and
`end`. The ssize check and trap are still the only `__prism_check`, and
the conversion's only return is still `static_cast<std::ptrdiff_t>(__n)`.

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
bdist 4
bfirst 9
blast 6
zsame 1
cbdist 4
cbfirst 2
cblast 5
bpdist 2
bpsame 1
isame 1
osame 1
```

`bdist 4`, `bfirst 9`, and `blast 6` are an `int[4]` whose first element
is 9 and whose last is 6: `end - begin` is 4, and the pointers address
those elements. `zsame 1` is `int[0]`: `begin` and `end` compare equal, and
the run does not dereference them. `cbdist` / `cbfirst` / `cblast` are a
`const int[4]`. `bpdist 2` and `bpsame 1` are an array of two null
pointers: `begin` is that array, the distance is 2, and the run does not
dereference the pointers. `isame 1` is `int[4][0]`: `begin` compares equal
to the array. `osame 1` is `int[0][4]`: `begin` and `end` compare equal.
Constant `begin` of `int[4]{1, 2, 3, 4}` is the array, `end` is one past
the last, the first element is 1, and the last is 4. Constant `begin` and
`end` of `int[0]` compare equal, against both headers. A requires-expression
rejects `begin` and `end` on `int*`, `const int*`, `int (*)[4]`, and a
class with no array bound, against both headers. clang++ 18.1.3 and g++
13.3.0 compiled the same trace against this header with `-Wall -Wextra
-Werror`. The g++ binary printed the same lines. An array `begin` and
`end`, including a const array, compiles with `-Wall -Wextra -Wpedantic
-Werror`.

`argv` `huge` is unchanged. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line.

`std::ranges::begin(int*)`, `std::ranges::end(int*)`, and those calls on
`int (*)[4]` fail to compile against this header (`no matching function`).
So does an lvalue whose const `begin()` member is declared. `sort` and
`find` are not members. `std::ranges::size(int*)` and
`std::ranges::empty(int*)` still fail the same way.
