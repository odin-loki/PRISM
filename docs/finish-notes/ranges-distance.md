# ranges distance

Branch `finish/cxx-ranges-distance`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::ranges::distance` is declared on the existing header
`src/prism/pir/models/cxx/ranges`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_RANGES`. `size`, `ssize`,
`empty`, `begin`, `end`, `data`, `cbegin`, `cend`, and `cdata` are
unchanged. A value that does not fit in `ptrdiff_t` still calls
`_Prism_ssize_does_not_fit`: `__prism_check` with class `FUNC-CONTRACT`
and message `ranges::ssize size does not fit in ptrdiff_t`, then
`__builtin_trap`. That path does not return a wrapped negative. There is
still no pointer overload of `size`. `empty` of a `T(&)[N]` is still true
only when `N == 0`, and that body still does not load an element. `begin`,
`end`, `data`, `cbegin`, `cend`, and `cdata` are still the same pointers
they were.

This slice did not run cmake, ninja, `prism_tests`, or pir. This is not
PROVED and not CLEAN. This is not a proof that an iterator stays in range.
This header is not a proof that a range is sorted or that a pointer has a
length. `distance` of an array is the bound. That value is not a length of
a pointer.

## What distance does

`distance` takes a `T(&)[N]` and nothing else. There is no iterator-pair
overload and no pointer overload.

For a bound other than 0 the parameter is `T (&)[N]`. The parameter is
unnamed, so the body does not load an element. The result is
`std::ptrdiff_t`. The only return is `static_cast<std::ptrdiff_t>(_Nm)`.

When `sizeof(std::size_t) >= sizeof(std::ptrdiff_t)`, a bound greater than
`PTRDIFF_MAX` calls `_Prism_ranges_distance_does_not_fit`. That function is
`[[noreturn]]`. It calls `__prism_check` with class `FUNC-CONTRACT` and
message `std::ranges::distance does not fit in ptrdiff_t`, then
`__builtin_trap`. It has no return. The cast sits after that call, so the
function does not return a wrapped negative `ptrdiff_t`. When `size_t` is
narrower than `ptrdiff_t`, every bound fits and that check is not
instantiated. On LP64 and LLP64 the widths are equal, so the check is in
the function.

A const array uses the same template, because `_Tp` is deduced as
`const U`.

`N == 0` does not match that parameter. Clang rejects `T (&)[0]` while
substituting the bound ("zero-length arrays are not permitted in C++"),
the same way it rejects that parameter for `empty`. The empty array is a
second overload: a reference constrained with `requires(sizeof(_Arr) == 0)`.
The parameter is unnamed. The body is `return 0` and does not load an
element. When `T(&)[N]` is viable it is the more specialized overload, so a
bound other than 0 is not taken there.

An array `int[4][0]` has size 0 and bound 4. `distance` is 4. `empty` of
that array is still false. An array `int[0]` and an array `int[0][4]` have
bound 0. `distance` is 0. The run does not dereference them.

A class with a const `size()` member has no overload. The call fails to
compile and pir falls back. `distance` does not call `size()`.

## What falls back

A pointer argument, including a pointer to an array, has no overload.
`sizeof` of a pointer is not 0, so the zero-length overload does not take
it. Clang reports that constraint as `sizeof(int *) == 0` evaluated to
false (`8 == 0`), and `sizeof(int (*)[4]) == 0` evaluated to false
(`8 == 0`). The call fails to compile (`no matching function`), and pir
falls back to libstdc++. A pointer is not a range and is not given a
length.

Two pointers fail the same way. Each overload requires one argument. There
is no iterator-pair overload.

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`):

- `ranges::sort`, `ranges::find`
- views

The header does not include `<iterator>`, `<algorithm>`, `<memory>`,
`<vector>`, `<string>`, or `<span>`.

Clang 18.1.3 rejects an array type of `PTRDIFF_MAX` elements and of
`PTRDIFF_MAX + 1` elements (`array is too large`). g++ 13.3.0 rejects a
`char` array of `PTRDIFF_MAX + 1` elements because that object size exceeds
the maximum object size `9223372036854775807`. Those compilers do not
instantiate a bound that does not fit. The check stays in the function so
a bound that fails the comparison takes the trap, and the cast is not
returned.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/ranges`. `CMakeLists.txt` was not edited.
`tests/cxx_models/ranges_trace.cpp` is not listed there.
`_prism_test_sources` already lists `tests/cpp/test_cxx_ranges.cpp`. No
other test source line was added or deleted.
`tests/cpp/test_qa_conformance.cpp` was not edited. The `"ranges"` entry
in that header array stays. The skips for `iostream/prism_ostream.hpp`
and `deque/deque` are unchanged.

`testdata/conformance/prism/cxx/cxx_ranges_true.cpp`,
`cxx_ranges_false.cpp`, and
`testdata/conformance/libc-models/ranges_contracts.cpp` were not edited.
They still call `size`, `ssize`, and `empty` only. `distance` is not
called there.

## Tests

`tests/cpp/test_cxx_ranges.cpp` reads the header. The array `distance`
calls `_Prism_ranges_distance_does_not_fit` before its only return,
`static_cast<std::ptrdiff_t>(_Nm)`. That body has no `*`, no subscript, no
member access, no addition, and no named parameter. The zero-length
`distance` returns `0`. That body does not load an element.
`requires(sizeof(_Arr) == 0)` occurs eight times: `empty`, `begin`, `end`,
`data`, `cbegin`, `cend`, `cdata`, and `distance`. `_Tp*` still occurs six
times, and there is still no pointer parameter. The ssize check and trap
are unchanged, and the conversion's only return is still
`static_cast<std::ptrdiff_t>(__n)`. The distance check is the one
`__prism_check` with message
`std::ranges::distance does not fit in ptrdiff_t`, followed by
`__builtin_trap` in a function that does not return. The header says
`This is not PROVED and not CLEAN.` once. There is no `.distance()` call.
The `size`, `ssize`, `empty`, `begin`, `end`, `data`, `cbegin`, `cend`,
and `cdata` signature locks are unchanged.

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
dist 4
zdist 0
cdist 4
idist 4
odist 0
pdist 2
```

`dist 4` is an `int[4]` whose bound is 4. The model body does not load the
first element. `zdist 0` is `int[0]`: the zero-length overload returns 0,
and the run does not dereference it. `cdist 4` is a `const int[4]`.
`idist 4` is `int[4][0]`: the bound is 4, so the array overload is the one
that runs, and the run does not dereference the zero-size elements.
`odist 0` is `int[0][4]`. `pdist 2` is an array of two null pointers. The
distance is that array's bound. The run does not dereference the pointers.
That is not a length of a pointer.

Constant `distance` of `int[4]{1, 2, 3, 4}` is 4 and equals `ssize` of that
array. Constant `distance` of `int[0]` is 0. The return type of `distance`
on `int[4]`, `const int[4]`, and `int[0]` is `ptrdiff_t`, against both
headers. A requires-expression accepts `distance` on those arrays and
rejects it on `int*`, `const int*`, `int (*)[4]`, and a class with no array
bound, against both headers. clang++ 18.1.3 and g++ 13.3.0 compiled the
same trace against this header with `-Wall -Wextra -Werror`. The g++ binary
printed the same lines. An array `distance`, including a const array and an
array of pointers, compiles with `-Wall -Wextra -Wpedantic -Werror` on both
compilers.

`argv` `huge` is unchanged. The model aborts with
`CHECK FUNC-CONTRACT: ranges::ssize size does not fit in ptrdiff_t` and
prints no `wrapped=` line.

`std::ranges::distance(int*)`, `std::ranges::distance(const int*)`, and
`std::ranges::distance(int (*)[4])` fail to compile against this header
(`no matching function`). So does `std::ranges::distance` on two `int*`,
and an lvalue whose const `size()` member is declared. `sort` and `find`
are not members. `std::ranges::size(int*)`, `std::ranges::empty(int*)`,
`std::ranges::begin(int*)`, `std::ranges::end(int*)`,
`std::ranges::data(int*)`, `std::ranges::cbegin(int*)`,
`std::ranges::cend(int*)`, and `std::ranges::cdata(int*)` still fail the
same way.
