# algorithm find_if / count

Branch `finish/cxx-algorithm-count`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`. Nothing here is `CLEAN`. After a failed check the
path traps. It does not continue into a read or a fabricated success.

## What this slice adds

`std::min`, `std::max`, `std::clamp`, `std::min_element`, `std::max_element`,
and `std::find` are unchanged.

`clamp(v, lo, hi)` when `hi < lo` still calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`.

`std::find_if` is one overload:
`_Tp* find_if(_Tp* first, _Tp* last, bool (*pred)(const _Tp&))`.
There is no generic predicate, no execution policy, and no second template
parameter. The predicate is a function pointer. A by-value `bool(int)` does
not match. A lambda `[](int)` does not match.

A null predicate calls `__prism_check` with class `FUNC-CONTRACT` and the
message `find_if: null predicate`, then `__builtin_trap`. That path does
not call the predicate. The null check is first, so an empty range with a
null predicate takes this path and does not return `last`.

When `last < first`, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message `find_if: last < first`, then
`__builtin_trap`. That path does not read an element, does not invent a
`T`, and does not call the predicate. In a constant expression the call is
ill-formed: `__prism_check` is not a constant expression.

When `first == last` and the predicate is non-null, the result is `last`.
That path does not read an element and does not call the predicate. An
empty range of null pointers returns null. A type with a deleted default
constructor is a valid element type. A type with no `operator==` is a valid
element type.

Otherwise the result is a pointer to the first element for which the
predicate returns true, or `last` if none. The true result returns before
the pointer advances. The function does not allocate and does not write
the range.

`std::count` is one overload:
`__PTRDIFF_TYPE__ count(_Tp* first, _Tp* last, const _Tp& value)`.
The result is a `ptrdiff_t`: how many elements in `[first, last)` compare
equal with `operator==`. The type must be equality-comparable, the same
constraint as `find`. `T` is deduced from the pointers and from the value.
A `const int*` and an `int` value do not match. A `long` value against
`int*` does not match. Those calls fail to compile against this header,
and the pir stage falls back to libstdc++.

When `last < first`, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message `count: last < first`, then
`__builtin_trap`. That path does not read an element. In a constant
expression the call is ill-formed.

When `first == last`, the result is 0. That path does not read an element.
An empty range of null pointers returns 0.

Otherwise the function counts matches. The tally starts at 0 after the
empty return. When the tally is already `PTRDIFF_MAX` and another element
compares equal, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message
`count: result does not fit in ptrdiff_t`, then `__builtin_trap`. That
path does not increment the tally, does not read the next element, and
does not return a wrapped count. A count of exactly `PTRDIFF_MAX` matches
still fits and is the return value. The trace does not execute a live
range of `PTRDIFF_MAX + 1` matches. The header order is the lock.

The function does not allocate and does not write the range.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. The header includes nothing.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
- `copy`
- `fill`
- `for_each`
- `count_if`
- `find_if_not`
- every other algorithm

No `<memory>`, `<functional>`, `<string>`, `<iterator>`, `<ranges>`,
`<vector>`, or `<compare>` header is included.

## Wiring

`src/prism/pir/stage.cpp`, `CMakeLists.txt`, and
`tests/cpp/test_qa_conformance.cpp` were not edited. The model path and
`tests/cpp/test_cxx_algorithm.cpp` were already on those lists. The
conformance header-name array was not edited. `src/prism/bmc.cpp`,
`src/prism/bmc_unenc.inc`, and `src/prism/bmc_encoder.inc` were not edited.
The existing `std::clamp` unencoded gate is unchanged.

## Tests

`tests/cxx_models/algorithm_trace.cpp` includes `<algorithm>`. It is compiled
twice (model directory as `-isystem`, and without it). Constant checks cover
the previous `min` / `max` / `clamp` / element / `find` cases. `find_if` and
`count` constant checks use a non-const local array, because a `constexpr`
array decays to `const T*`. They cover a hit, the first of two equals, a
front hit, a miss, an empty range, a tie, a null empty range, a type with
only `operator==`, a type with no default constructor, and a type with no
`operator==` (`find_if` only).

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The earlier algorithms
are unchanged. `return 0` stays absent before `count`. `find_if` checks a
null predicate, then `last < first`, then the empty return, all before any
call or dereference. The hit returns before `++__first`. `count` checks
`last < first` before the empty `return 0`, and that return is before any
dereference and before the tally. The overflow check is
`__prism_check` then `__builtin_trap` before `++__n` and before the next
element, with no `return` on that path. `sort(`, `copy(`, `fill(`, and
`for_each(` stay absent. `count_if` and `find_if_not` stay absent.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Stdout of the model binary matched libstdc++:

```
min_eq=1
max_eq=1
min_lo=2
max_hi=9
clamp_mid=5 id=1
clamp_lo=2 id=1
clamp_hi=9 id=1
wide_min_is0=0 wide_min_is_min=1
wide_max_is0=0 wide_max_is_max=1
clamp_inside_zero=1
ord_eq_first=1 ord_max=2
ptr_eq_first=1
el_min=1 el_min_i=2
el_max=9 el_max_i=0
el_empty=1
el_tie=1
el_one=1
el_null=1
el_ord_min=1 el_ord_max_i=0
el_nodef_min=1 el_nodef_max_i=0 el_nodef_empty=1
find_hit=1 find_hit_i=2
find_first_i=1
find_front_i=0
find_miss=1
find_empty=1
find_one=1
find_null=1
find_keep=1
find_eq_i=1 find_eq_id=1 find_eq_miss=1 find_eq_empty=1
find_nodef=1 find_nodef_i=1 find_nodef_empty=1
find_if_hit=1 find_if_hit_i=2 find_if_hit_calls=3
find_if_first_i=1 find_if_first_calls=2
find_if_front_i=0 find_if_front_calls=1
find_if_miss=1 find_if_miss_calls=4
find_if_empty=1 find_if_empty_calls=0
find_if_tie=1 find_if_tie_calls=1
find_if_one=1
find_if_null=1
find_if_keep=1
find_if_eq_i=1 find_if_eq_empty=1
find_if_nodef_i=1 find_if_nodef_empty=1
find_if_pred_i=1 find_if_pred_empty=1
count_four=2
count_one=1
count_nine=1
count_miss=0
count_empty=0
count_tie=2
count_one_hit=1
count_one_miss=0
count_null=0
count_eq=2 count_eq_miss=0 count_eq_empty=0
count_nodef=1 count_nodef_three=2 count_nodef_empty=0
count_keep=1
```

`find_if_hit_i=2` is the `1` in `{9, 4, 1, 4}`, after three predicate calls.
`find_if_first_calls=2` stops on the first `4`. `find_if_front_calls=1` is
the leading `9`. `find_if_miss_calls=4` is no `7`. `find_if_empty_calls=0`
returns the start pointer and does not call. `find_if_null=1` is an empty
null range with a live predicate; ASan reported no read and the call count
stayed 0. `find_if_keep=1` and `count_keep=1` are the range still
`{9, 4, 1, 4}`. `count_four=2` is the two `4`s. `count_empty=0` and
`count_null=0` read nothing. `count_eq=2` is two `EqOnly` matches.
`count_nodef` is a type with a deleted default constructor: one `1`, two
`3`s, and the empty range returns 0. `find_if_pred` is a type with no
`operator==`.

`argv` `invert` aborts with `CHECK FUNC-CONTRACT: clamp: hi < lo`.
`invert-min` and `invert-max` abort with `min_element: last < first` and
`max_element: last < first`. `invert-find` aborts with
`CHECK FUNC-CONTRACT: find: last < first`. `invert-find-if` aborts with
`CHECK FUNC-CONTRACT: find_if: last < first` and does not print `called`.
`null-find-if` and `null-find-if-empty` abort with
`CHECK FUNC-CONTRACT: find_if: null predicate`. `invert-count` aborts with
`CHECK FUNC-CONTRACT: count: last < first`. None of those runs prints
`value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`static_assert(std::min_element(a + 2, a) == a)` is not a constant
expression. A `constexpr` function that calls `std::find`, `std::find_if`,
or `std::count` on a non-const local array with `last < first` is not a
constant expression: the note from clang++ is that `__prism_check` cannot
be used in a constant expression. The same note is on a null `find_if`
predicate. `static_assert` of `count` on an empty range is 0, against this
header and against libstdc++.

`std::find_if` of `int*` and `bool (*)(const int&)` compiles against
libstdc++ and against this header. `std::find_if` of a `[](int)` lambda,
and of `bool (*)(int)`, compiles against libstdc++ and fails against this
header. `std::count` of `int*` and `const int&` compiles against both.
`std::count` of `int*` and `long`, and of `const int*` and `int`, compiles
against libstdc++ and fails against this header (`deduced conflicting
types`). `std::count_if`, `std::sort`, `std::copy`, `std::fill`, and
`std::for_each` compile against libstdc++ and fail against this header.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.
