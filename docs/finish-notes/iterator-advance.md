# iterator advance slice

Branch `finish/cxx-iterator-advance`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::advance` for a pointer lives in the existing extensionless file
`src/prism/pir/models/cxx/iterator`. A directory of that name would make
`#include <iterator>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_ITERATOR`. The file is
still a `#pragma GCC system_header`, and the functions stay in namespace
`std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. Nothing here is `CLEAN`. This is not a proof that an iterator
stays in range, and it is not a proof that the pointer addresses an array.

## What the model returns

`begin`, `end`, `distance`, `next`, and `prev` are unchanged, including
both `N == 0` overloads and the negative-offset traps. For `N > 0` the
parameter is `T (&)[N]`. `begin` returns `__arr` (array-to-pointer decay,
not a load). `end` returns `__arr + _Nm`. `N == 0` is still the reference
constrained with `sizeof(_Arr) == 0`. Those two pointers compare equal. A
pointer argument still does not match `begin` or `end`. `distance` is
still the two-`T*` overload. A count that does not fit in `ptrdiff_t` is
still `__prism_check` with class `FUNC-CONTRACT` and message
`std::distance subtraction does not fit in ptrdiff_t`, then
`__builtin_trap`. `next(p)` still returns `p + 1`. `next(p, 0)` still
returns `p` and does not add. `next(p, n)` for `n > 0` still returns
`p + n`. `n < 0` is still `_Prism_next_negative`. `prev(p)` still returns
`p - 1`. `prev(p, 0)` still returns `p` and does not subtract. `prev(p, n)`
for `n > 0` still returns `p - n`. `n < 0` is still `_Prism_prev_negative`.

`std::advance` is one overload, `void advance(T*& p, ptrdiff_t n)`. There
is no default argument. A class iterator does not match. The parameter is
a reference, so the call writes the caller's pointer.

`advance(p, 0)` returns and leaves `p` unchanged. That branch is
`if (__n == 0) return;`. The text of that branch does not add and does
not subtract.

`advance(p, n)` for `n > 0` sets `p` to `p + n`. The assignment is
`__p = __p + __n` in the `__n > 0` branch.

`advance(p, n)` for `n < 0` sets `p` to `p + n`. That is a backward step
because the offset is negative. The assignment is the same
`__p = __p + __n`, in the `__n < 0` branch. That branch does not trap,
does not call `__prism_check`, does not call `__builtin_trap`, and does
not call `next` or `prev`. The function body contains no `-`. It does not
form `p - n`.

The same sentences are comments on the header: this is not a proof that
the pointer addresses an array, and this is not PROVED and not CLEAN.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is
not defined in the header. This slice does not declare it again.

`advance(p, 0)`, `advance(p, n)` for an in-range positive `n`, and
`advance` from `end` by a negative `n` that lands on an element or on
`begin` are constant expressions. A negative `n` is not ill-formed merely
because it is negative. `next` and `prev` still are, for a negative
offset, because those paths call a function that is not `constexpr`.

## What falls back

Not declared, or not a match, so a unit that names one fails to compile
against this header and pir lowers that translation unit again with
libstdc++ (`extra.cxx_models`). They are not stubbed:

- `begin` and `end` for a pointer
- `std::distance` for anything other than two `T*`
- `std::next` and `std::prev` for anything other than `T*`
- `std::advance` for anything other than `T*&`
- `reverse_iterator`
- iterator traits, tags, insert iterators, stream iterators, and the
  container overloads of `begin` and `end`

The header includes `<cstddef>` and `<cstdint>`. It does not include
`<memory>`, `<vector>`, `<string>`, `<algorithm>`, `<ranges>`, or
`<cmath>`. A later `++` or a dereference of the pointer is the caller's
expression. This header does not decide whether that use stays inside an
array.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/iterator`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_iterator.cpp`. No new `tests/cpp` file was added, so
that list was not given another line and no existing line was removed.
`tests/cxx_models/iterator_trace.cpp` was not added to `CMakeLists.txt`.
`tests/cpp/test_qa_conformance.cpp` already lists `"iterator"` in the
public header-name array. That array was not edited. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged. `iostream`
and `ostream` stay absent. `CMakeLists.txt` was not edited.

## Tests

`tests/cpp/test_cxx_iterator.cpp` reads the header as text. It still checks
that `begin` and `end` are the array-reference overload and the
`sizeof == 0` overload, that those bodies do not load the element, and
that a pointer is not a `begin` parameter. The `distance`, `next`, and
`prev` locks stay, including the zero returns and the negative-offset
traps. The star count is now seventeen: the previous sixteen, plus the
`_Tp*&` parameter of `advance`. The body does not dereference. With line
comments removed, the code names `advance` once and does not name
`reverse_iterator`. The source says `This is not PROVED and not CLEAN.`
twice, and `a backward step` twice. In `advance`, `__n == 0` and
`return;` come before either `+`. Each of `__n > 0` and `__n < 0` contains
one `__p = __p + __n` and no `-`. The negative slice contains no
`__prism_check` and no `__builtin_trap`. The body does not name `next` or
`prev`. The translation unit was syntax-only. `prism_tests` was not run,
so those `CHECK`s were not executed by the test binary. A reader using the
same comment strip and brace match accepted the header.

`tests/cxx_models/iterator_trace.cpp` is `#include <iterator>`. Checked
with clang++ 18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`). The model trace printed

```
len 3 first 4 last 6
neg -3
empty 1
const 2 8
ptrs 2 1
next 5 1 6 1
next0 1
nextc 9
nextrt 1 6
prev 6 1 5 1
prev0 1
prevc 9
prevrt 1 5
adv 1 6 6 1
adv0 1
advc 9
advrt 1 6 5
```

and exited 0. `adv 1 6 6 1` is `advance(begin, 0) == begin`,
`*advance(begin, 2)`, `*advance(end, -1)`, and `advance(end, -3) == begin`.
`adv0` is `advance` of an empty array's `begin` by zero. `advc` is
`*advance` of a const array's `begin` by one. `advrt` is the zero step,
the `+ 2` step, and the `- 2` step from `end`, with volatile offsets, so
those branches ran. `argv` `aneg` is `advance` by `-1` from the middle
element. It prints `moved=4` and exits 0, with no `CHECK` line. `argv`
`neg` still aborts with `CHECK FUNC-CONTRACT: std::next offset is negative`
and prints no `moved=` line. `argv` `pneg` still aborts with
`CHECK FUNC-CONTRACT: std::prev offset is negative` and prints no
`moved=` line. `argv` `huge` still aborts with
`CHECK FUNC-CONTRACT: std::distance subtraction does not fit in ptrdiff_t`
and prints no `wrapped=` line.

A driver whose `__prism_check` returns, then calls `advance` by `-1`,
prints `moved=4` and exits 0. The same driver calling `next` by `-1`
dies with SIGILL from `__builtin_trap` and prints no `moved=` line.

An `N > 0` program (distance `3` and `-3` on `int[3]`, distance `2` on a
const array and on an array of pointers, the same `next` and `prev`
steps, and the same `advance` steps) printed the same lines against
libstdc++ and against this header. `static_assert` of those constexpr
`advance` results holds both ways. `std::begin` on `int*` fails here
(`no matching function for call to 'begin'`). `std::advance` on a class
type fails here (`no matching function for call to 'advance'`, candidate
`could not match '_Tp *' against 'NotAPointer'`). libstdc++ accepts that
call at the signature and then fails in the body (`no type named
'difference_type'`). `std::reverse_iterator` fails here (`no member named
'reverse_iterator'`). An empty array still does not compile against
libstdc++ (`std::begin` has no match; with `-Wpedantic -Werror`, clang
also reports `zero size arrays are an extension`). It compiles against
this header.

libstdc++ `advance(p, -1)` from the middle of `int[3]` prints `moved=4`
and exits 0, with no sanitizer diagnostic. `advance(p, 0)` leaves the
pointer equal to `p`.

`testdata/conformance/prism/cxx/cxx_iterator_true.cpp` still reads `*begin`
of a three-element array and compares `begin` and `end` of an empty array
(`no-oob`, `expected: true`). It also requires the same in-range `next`
and `prev` calls as before. It does not call `advance`. Those calls do
not load the element except the existing `*begin` read.
`cxx_iterator_false.cpp` is unchanged: `distance` of two `char*` separated
by `PTRDIFF_MAX + 1` bytes (`expected: false`, witness `[1]`, property
`no-overflow`). Both `expected` values are YAML booleans. The yml files
were not edited.

`testdata/conformance/libc-models/iterator_contracts.cpp` includes
`<iterator>` and calls the same in-range `distance`, `next`, and `prev`.
It does not call `advance`. It is the contract harness the header case
searches for. It is not a scored task.

cmake, ninja, `prism_tests`, and pir were not run.
