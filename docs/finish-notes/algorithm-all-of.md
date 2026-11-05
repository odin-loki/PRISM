# algorithm all_of / any_of / none_of

Branch `finish/cxx-algorithm-all-of`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`. Nothing here is `CLEAN`. After a failed check the
path traps. It does not continue into a read, a predicate call, or a
fabricated success.

## What this slice adds

`std::min`, `std::max`, `std::clamp`, `std::min_element`, `std::max_element`,
`std::find`, `std::find_if`, and `std::count` are unchanged.

`clamp(v, lo, hi)` when `hi < lo` still calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`.

`std::all_of`, `std::any_of`, and `std::none_of` are one overload each:

`bool all_of(_Tp* first, _Tp* last, bool (*pred)(const _Tp&))`

`bool any_of(_Tp* first, _Tp* last, bool (*pred)(const _Tp&))`

`bool none_of(_Tp* first, _Tp* last, bool (*pred)(const _Tp&))`

There is no second overload of any of those names. There is no generic
predicate, no execution policy, and no second template parameter. The
predicate is a function pointer. A by-value `bool(int)` does not match. A
lambda `[](int)` does not match. The element type is not required to be
equality-comparable or less-than-comparable. A type with a deleted default
constructor is a valid element type. A type with no `operator==` is a valid
element type.

A null predicate calls `__prism_check` with class `FUNC-CONTRACT` and the
message `all_of: null predicate` (the same shape for `any_of` and
`none_of`), then `__builtin_trap`. That path does not call the predicate.
The null check is first, so an empty range with a null predicate takes this
path and does not return the empty-range result.

When `last < first`, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message `all_of: last < first` (the same shape for
the others), then `__builtin_trap`. That path does not read an element,
does not invent a `T`, and does not call the predicate. In a constant
expression the call is ill-formed: `__prism_check` is not a constant
expression.

When `first == last` and the predicate is non-null, the function does not
read and does not call. `all_of` returns true. `any_of` returns false.
`none_of` returns true. An empty range of null pointers follows that rule.
ASan reported no read, and the call count stayed 0.

Otherwise `all_of` returns false at the first element for which the
predicate is false, else true. `any_of` returns true at the first element
for which it is true, else false. `none_of` returns false at the first
element for which it is true, else true. The deciding return is before
`++__first`, so the predicate is not called on a later element. The
functions do not allocate and do not write the range.

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
- every other algorithm

No `<memory>`, `<functional>`, `<string>`, `<iterator>`, `<ranges>`,
`<vector>`, or `<compare>` header is included.

## Wiring

`src/prism/pir/stage.cpp`, `CMakeLists.txt`, and
`tests/cpp/test_qa_conformance.cpp` were not edited. The model path and
`tests/cpp/test_cxx_algorithm.cpp` were already on those lists. No new
`tests/cpp` file was added, so `_prism_test_sources` was not edited and no
line was dropped. The conformance header-name array was not edited.

## Tests

`tests/cxx_models/algorithm_trace.cpp` includes `<algorithm>`. It is compiled
twice (model directory as `-isystem`, and without it). Constant checks cover
the previous `min` / `max` / `clamp` / element / `find` / `find_if` /
`count` cases, plus `all_of`, `any_of`, and `none_of`. Those constant
checks use a non-const local array, because a `constexpr` array decays to
`const T*`. They cover a hit, a front decision, a miss, an empty range, a
tie, a null empty range, a type with only `operator==`, a type with no
default constructor, and a type with no `operator==`.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The earlier algorithms
are unchanged. Each new function checks a null predicate, then
`last < first`, then the empty return, all before any call or dereference.
The deciding return is before `++__first`. Each name occurs once.
`sort(`, `copy(`, `fill(`, and `for_each(` stay absent.

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
all_of_all=1 all_of_all_calls=4
all_of_front=0 all_of_front_calls=1
all_of_mid=0 all_of_mid_calls=2
all_of_empty=1 all_of_empty_calls=0
all_of_one=1
all_of_null=1 all_of_null_calls=0
all_of_keep=1
all_of_eq=0 all_of_eq_empty=1
all_of_nodef=0 all_of_nodef_empty=1
all_of_pred=0 all_of_pred_empty=1
any_of_hit=1 any_of_hit_calls=3
any_of_front=1 any_of_front_calls=1
any_of_miss=0 any_of_miss_calls=4
any_of_empty=0 any_of_empty_calls=0
any_of_tie=1 any_of_tie_calls=1
any_of_one=1
any_of_null=0 any_of_null_calls=0
any_of_keep=1
any_of_eq=1 any_of_eq_empty=0
any_of_nodef=1 any_of_nodef_empty=0
any_of_pred=1 any_of_pred_empty=0
none_of_miss=1 none_of_miss_calls=4
none_of_front=0 none_of_front_calls=1
none_of_mid=0 none_of_mid_calls=3
none_of_empty=1 none_of_empty_calls=0
none_of_one=1
none_of_null=1 none_of_null_calls=0
none_of_keep=1
none_of_eq=0 none_of_eq_empty=1
none_of_nodef=0 none_of_nodef_empty=1
none_of_pred=0 none_of_pred_empty=1
```

`all_of_all_calls=4` is every element of `{9, 4, 1, 4}` true.
`all_of_front_calls=1` is false on the leading element.
`all_of_mid_calls=2` is true on `9` and false on the next `4`.
`all_of_empty_calls=0` returns true and does not call.
`any_of_hit_calls=3` is the `1` in `{9, 4, 1, 4}`.
`any_of_front_calls=1` is the leading `9`.
`any_of_miss_calls=4` is no `7`.
`any_of_empty_calls=0` returns false.
`none_of_miss_calls=4` is no `7`, so the result is true.
`none_of_front_calls=1` is the leading `9`, so the result is false.
`none_of_mid_calls=3` stops on the `1`.
`none_of_empty_calls=0` returns true.
`all_of_null`, `any_of_null`, and `none_of_null` are an empty null range
with a live predicate. `all_of_keep`, `any_of_keep`, and `none_of_keep` are
the range still `{9, 4, 1, 4}`. `all_of_eq`, `any_of_eq`, and `none_of_eq`
are `EqOnly`. `*_nodef` is a type with a deleted default constructor.
`*_pred` is a type with no `operator==`.

`argv` `invert` aborts with `CHECK FUNC-CONTRACT: clamp: hi < lo`.
`invert-min` and `invert-max` abort with `min_element: last < first` and
`max_element: last < first`. `invert-find` aborts with
`CHECK FUNC-CONTRACT: find: last < first`. `invert-find-if` aborts with
`CHECK FUNC-CONTRACT: find_if: last < first` and does not print `called`.
`null-find-if` and `null-find-if-empty` abort with
`CHECK FUNC-CONTRACT: find_if: null predicate`. `invert-count` aborts with
`CHECK FUNC-CONTRACT: count: last < first`. `invert-all-of`,
`invert-any-of`, and `invert-none-of` abort with
`all_of: last < first`, `any_of: last < first`, and
`none_of: last < first`, and do not print `called`. `null-all-of`,
`null-any-of`, `null-none-of`, and the `-empty` forms abort with
`all_of: null predicate`, `any_of: null predicate`, and
`none_of: null predicate`. None of those runs prints `value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`static_assert(std::min_element(a + 2, a) == a)` is not a constant
expression. A `constexpr` function that calls `std::all_of`, `std::any_of`,
or `std::none_of` on a non-const local array with `last < first` is not a
constant expression: the note from clang++ is that `__prism_check` cannot
be used in a constant expression. The same note is on a null predicate,
including an empty range with a null predicate (`all_of<int>(&a[0], &a[0],
nullptr)`). `static_assert` of an empty range is true for `all_of`, false
for `any_of`, and true for `none_of`, against this header and against
libstdc++.

`std::all_of`, `std::any_of`, and `std::none_of` of `int*` and
`bool (*)(const int&)` compile against libstdc++ and against this header.
A `[](int)` lambda, and `bool (*)(int)`, compile against libstdc++ and fail
against this header (`no matching function`). `std::sort`, `std::copy`,
`std::fill`, and `std::for_each` stay omitted: the header text does not
declare them.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.

## Port note (onto main)

Main already declared all_of, any_of, and none_of with any callable
predicate (`_Pred`). The port keeps that single overload per name and adds
this commit's traps to it: a null predicate traps when `_Pred` is a pointer
(`__is_pointer`), before the range check; then the inverted-range trap, the
empty-range return, and the scan, in the order the doctest checks. Lambdas
and `bool (*)(int)` predicates stay accepted, so the doctest expects them to
compile under the model.
