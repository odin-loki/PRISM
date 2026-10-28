# ranges data

Branch `finish/cxx-ranges-data`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`std::ranges::data` is declared on the existing header
`src/prism/pir/models/cxx/ranges`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_RANGES`. `size`, `ssize`,
`empty`, `begin`, and `end` are unchanged. A value that does not fit in
`ptrdiff_t` still calls `_Prism_ssize_does_not_fit`: `__prism_check` with
class `FUNC-CONTRACT` and message
`ranges::ssize size does not fit in ptrdiff_t`, then `__builtin_trap`.
That path does not return a wrapped negative. There is still no pointer
overload of `size`. `empty` of a `T(&)[N]` is still true only when
`N == 0`, and that body still does not load an element. `begin` and `end`
are still the same pointers they were.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. This is not a proof that an iterator stays in range. This
header is not a proof that a range is sorted or that a pointer has a
length. `data` returns a pointer to the first element of an array. That
pointer is not a length.

## What data does

`data` takes a `T(&)[N]` and nothing else.

For a bound other than 0 the parameter is `T (&)[N]`. `data` returns
`__a`. That is array-to-pointer decay: a pointer to the first element, and
the body does not load it. A const array uses the same template, because
`_Tp` is deduced as `const U`. The return type is `_Tp*`.

`N == 0` does not match that parameter. Clang rejects `T (&)[0]` while
substituting the bound ("zero-length arrays are not permitted in C++"),
the same way it rejects that parameter for `begin` and `empty`. The empty
array is a second overload: a reference constrained with
`requires(sizeof(_Arr) == 0)`. `data` returns the decayed pointer. The
trailing return type is `decltype(__a + 0)`, which names the pointer type.
The body is `return __a` and does not load an element. When `T(&)[N]` is
viable it is the more specialized overload, so a bound other than 0 is not
taken there.

An array `int[4][0]` has size 0 and bound 4. `data` compares equal to the
array. The trace does not dereference it: the element type has size 0.
`empty` of that array is still false. An array `int[0]` and an array
`int[0][4]` have bound 0. `data` is the decayed pointer, and it compares
equal to `begin`. The run does not dereference them.

A class with a `data()` member has no overload. The call fails to compile
and pir falls back.

## What falls back

A pointer argument, including a pointer to an array, has no overload.
`sizeof` of a pointer is not 0, so the zero-length overload does not take
it. Clang reports that constraint as `sizeof(int *) == 0` evaluated to
false (`8 == 0`), and the same for `int (*)[4]`. The call fails to compile
(`no matching function`), and pir falls back to libstdc++. A pointer is
not a range. This is not a proof that the pointer has a length.

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`):

- `ranges::sort`, `ranges::find`
- views

The header does not include `<iterator>`, `<algorithm>`, `<memory>`,
`<vector>`, `<string>`, or `<span>`. A later dereference of the returned
pointer is the caller's expression. This header does not decide whether
that use stays inside the array, and it does not attach a length to the
pointer.

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
They still call `size`, `ssize`, and `empty` only. `data` is not called
there.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. The array `data` returns
`__a`, and that body has no `*`, no subscript, no member access, and no
addition. The zero-length `data` returns `__a`. That body does not load an
element. `_Tp*` occurs three times, the return types of `begin`, `end`,
and `data`, and there is no pointer parameter.
`requires(sizeof(_Arr) == 0)` occurs four times: `empty`, `begin`, `end`,
and `data`. `return __a;` occurs four times: the array and zero-length
overloads of `begin` and of `data`. The ssize check and trap are still the
only `__prism_check`, and the conversion's only return is still
`static_cast<std::ptrdiff_t>(__n)`. There is no `.data()` call.

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
dfirst 9
dsame 1
cdfirst 2
cdsame 1
zdata 1
dpsame 1
idsame 1
odsame 1
```

`dfirst 9` is an `int[4]` whose first element is 9. The `*` is the caller
reading through the pointer `data` returned; the model body does not load
it. `dsame 1` is that pointer comparing equal to the array and to `begin`.
`cdfirst 2` and `cdsame 1` are a `const int[4]` whose first element is 2.
`zdata 1` is `int[0]`: `data` compares equal to the array and to `begin`,
and the run does not dereference it. `dpsame 1` is an array of two null
pointers: `data` is that array, and the run does not dereference the
pointers. That is not a length of a pointer. `idsame 1` is `int[4][0]`:
`data` compares equal to the array. `odsame 1` is `int[0][4]`: `data`
compares equal to the array and to `begin`. Constant `data` of
`int[4]{1, 2, 3, 4}` is the array, equals `begin`, and the first element
is 1. Constant `data` of `int[0]` equals the array and equals `begin`,
against both headers. A requires-expression rejects `data` on `int*`,
`const int*`, `int (*)[4]`, and a class with no array bound, against both
headers. clang++ 18.1.3 and g++ 13.3.0 compiled the same trace against
this header with `-Wall -Wextra -Werror`. The g++ binary printed the same
lines. An array `data`, including a const array, compiles with
`-Wall -Wextra -Wpedantic -Werror` on both compilers.

`argv` `huge` is unchanged. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line.

`std::ranges::data(int*)` and `std::ranges::data(int (*)[4])` fail to
compile against this header (`no matching function`). So does an lvalue
whose const `data()` member is declared. `sort` and `find` are not
members. `std::ranges::size(int*)`, `std::ranges::empty(int*)`, and
`std::ranges::begin(int*)` still fail the same way.
