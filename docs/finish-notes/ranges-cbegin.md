# ranges cbegin

Branch `finish/cxx-ranges-cbegin`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::ranges::cbegin`, `std::ranges::cend`, and `std::ranges::cdata` are
declared on the existing header `src/prism/pir/models/cxx/ranges`. The
header is still a file, not a directory. The include guard is still
`_GLIBCXX_RANGES`. `size`, `ssize`, `empty`, `begin`, `end`, and `data`
are unchanged. A value that does not fit in `ptrdiff_t` still calls
`_Prism_ssize_does_not_fit`: `__prism_check` with class `FUNC-CONTRACT`
and message `ranges::ssize size does not fit in ptrdiff_t`, then
`__builtin_trap`. That path does not return a wrapped negative. There is
still no pointer overload of `size`. `empty` of a `T(&)[N]` is still true
only when `N == 0`, and that body still does not load an element. `begin`,
`end`, and `data` are still the same pointers they were.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. Nothing here is `CLEAN`. This is not a proof that an iterator
stays in range. This header is not a proof that a range is sorted or that
a pointer has a length. `cbegin`, `cend`, and `cdata` return a const
pointer into an array. That pointer is not a length.

## What cbegin, cend, and cdata do

`cbegin`, `cend`, and `cdata` take a `T(&)[N]` and nothing else.

For a bound other than 0 the parameter is `T (&)[N]`. The return type is
`const _Tp*`. `cbegin` returns `__a`. That is array-to-pointer decay: a
const pointer to the first element, and the body does not load it. `cend`
returns `__a + _Nm`, a const pointer one past the last element. The
addition does not load an element. `cdata` returns `__a`, that same
decayed const pointer, and the body does not load it. A const array uses
the same templates, because `_Tp` is deduced as `const U`. `const _Tp*`
then names `const U*`.

`N == 0` does not match that parameter. Clang rejects `T (&)[0]` while
substituting the bound ("zero-length arrays are not permitted in C++"),
the same way it rejects that parameter for `begin` and `empty`. The empty
array is a second overload: a reference constrained with
`requires(sizeof(_Arr) == 0)`. The trailing return type is
`decltype(static_cast<const _Arr&>(__a) + 0)`, which names the const
pointer. The body is `return __a` and does not load an element. `cbegin`,
`cend`, and `cdata` each return that decayed pointer, so on an empty array
the three compare equal. When `T(&)[N]` is viable it is the more
specialized overload, so a bound other than 0 is not taken there.

An array `int[4][0]` has size 0 and bound 4. `cbegin` and `cdata` compare
equal to the array. The trace does not call `cend` on that array: the
element type has size 0, and g++ rejects arithmetic on a pointer to an
empty aggregate. `empty` of that array is still false. An array `int[0]`
and an array `int[0][4]` have bound 0. `cbegin`, `cend`, and `cdata` are
the decayed const pointer. The run does not dereference them.

A class with a `begin()` or `data()` member has no overload. The call
fails to compile and pir falls back.

## What falls back

A pointer argument, including a pointer to an array, has no overload.
`sizeof` of a pointer is not 0, so the zero-length overload does not take
it. Clang reports that constraint as `sizeof(int *) == 0` evaluated to
false (`8 == 0`), and `sizeof(int (*)[4]) == 0` evaluated to false
(`8 == 0`). The call fails to compile (`no matching function`), and pir
falls back to libstdc++. A pointer is not a range. This is not a proof
that the pointer has a length.

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
They still call `size`, `ssize`, and `empty` only. `cbegin`, `cend`, and
`cdata` are not called there.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. The array `cbegin` and
`cdata` return `__a`, and those bodies have no `*`, no subscript, no
member access, and no addition. The array `cend` returns `__a + _Nm`, and
that body has no `*`, no subscript, and no member access. The zero-length
`cbegin`, `cend`, and `cdata` return `__a`. Those bodies do not load an
element. `_Tp*` occurs six times: the return types of `begin`, `end`, and
`data`, and the `const _Tp*` return types of `cbegin`, `cend`, and
`cdata`. `const _Tp*` occurs three times. There is no pointer parameter.
`requires(sizeof(_Arr) == 0)` occurs seven times: `empty`, `begin`, `end`,
`data`, `cbegin`, `cend`, and `cdata`. `return __a;` occurs nine times:
the array and zero-length overloads of `begin` and of `data`, the array
and zero-length overloads of `cbegin` and of `cdata`, and the zero-length
overload of `cend`. `return __a + _Nm;` occurs twice, `end` and `cend`.
`return __a + 0;` still occurs once, the zero-length `end`. The ssize
check and trap are still the only `__prism_check`, and the conversion's
only return is still `static_cast<std::ptrdiff_t>(__n)`. There is no
`.data()`, `.cbegin()`, `.cend()`, or `.cdata()` call. The `begin`,
`end`, and `data` signature locks are unchanged.

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
kbdist 4
kbfirst 9
kblast 6
kbsame 1
kzsame 1
kcbdist 4
kcbfirst 2
kcblast 5
kbpdist 2
kbpsame 1
kisame 1
kosame 1
kdfirst 9
kdsame 1
kcdfirst 2
kcdsame 1
kzdata 1
kdpsame 1
kidsame 1
kodsame 1
```

`kbdist 4`, `kbfirst 9`, and `kblast 6` are an `int[4]` whose first element
is 9 and whose last is 6: `cend - cbegin` is 4. The `*` is the caller
reading through the pointer `cbegin` returned; the model body does not
load it. `kbsame 1` is that pointer comparing equal to `begin`. `kzsame 1`
is `int[0]`: `cbegin` and `cend` compare equal, and the run does not
dereference them. `kcbdist` / `kcbfirst` / `kcblast` are a `const int[4]`.
`kbpdist 2` and `kbpsame 1` are an array of two null pointers: `cbegin` is
that array, the distance is 2, and the run does not dereference the
pointers. That is not a length of a pointer. `kisame 1` is `int[4][0]`:
`cbegin` compares equal to the array. `kosame 1` is `int[0][4]`: `cbegin`
and `cend` compare equal. `kdfirst 9` is the caller reading through the
pointer `cdata` returned. `kdsame 1` is that pointer comparing equal to the
array and to `cbegin`. `kcdfirst 2` and `kcdsame 1` are a `const int[4]`
whose first element is 2. `kzdata 1` is `int[0]`: `cdata` compares equal to
the array and to `cbegin`, and the run does not dereference it. `kdpsame 1`
is the array of two null pointers: `cdata` is that array. `kidsame 1` is
`int[4][0]`. `kodsame 1` is `int[0][4]`: `cdata` compares equal to the array
and to `cbegin`.

Constant `cbegin` of `int[4]{1, 2, 3, 4}` is the array, equals `begin`,
`cend` is one past the last, the first element is 1, and the last is 4.
Constant `cdata` equals `cbegin`, and the first element is 1. Constant
`cbegin`, `cend`, and `cdata` of `int[0]` compare equal, against both
headers. The return type of `cbegin`, `cend`, and `cdata` on `int[4]`,
`const int[4]`, and `int[0]` is `const int*`, against both headers. A
requires-expression rejects `cbegin`, `cend`, and `cdata` on `int*`,
`const int*`, `int (*)[4]`, and a class with no array bound, against both
headers. clang++ 18.1.3 and g++ 13.3.0 compiled the same trace against this
header with `-Wall -Wextra -Werror`. The g++ binary printed the same lines.
An array `cbegin`, `cend`, and `cdata`, including a const array, compiles
with `-Wall -Wextra -Wpedantic -Werror` on both compilers. Assigning any of
the three on `int[4]` to `int*` fails: the result is `const int*`.

`argv` `huge` is unchanged. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line.

`std::ranges::cbegin(int*)`, `std::ranges::cend(int*)`,
`std::ranges::cdata(int*)`, and those calls on `int (*)[4]` fail to compile
against this header (`no matching function`). So does an lvalue whose const
`begin()` or `data()` member is declared. `sort` and `find` are not
members. `std::ranges::size(int*)`, `std::ranges::empty(int*)`,
`std::ranges::begin(int*)`, `std::ranges::end(int*)`, and
`std::ranges::data(int*)` still fail the same way.
