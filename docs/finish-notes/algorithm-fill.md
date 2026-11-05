# algorithm fill

Branch `finish/cxx-algorithm-fill`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`. Nothing here is `CLEAN`. After a failed check the
path traps. It does not continue into a read, a write, or a fabricated
success.

## What this slice adds

`std::min`, `std::max`, `std::clamp`, `std::min_element`, `std::max_element`,
`std::find`, `std::find_if`, `std::count`, `std::all_of`, `std::any_of`, and
`std::none_of` are unchanged.

`clamp(v, lo, hi)` when `hi < lo` still calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`.

`std::fill` is one overload:

`void fill(_Tp* first, _Tp* last, const _Tp& value)`

There is no second overload. There is no execution policy and no second
template parameter. `T` is deduced from the pointers and from the value. A
`long` value against `int*` does not match. That call fails to compile
against this header, and the pir stage falls back to libstdc++. The element
type is not required to be equality-comparable or less-than-comparable. A
type with a deleted default constructor and no `operator==` is a valid
element type.

When `last < first`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::fill last is before first")`

and then `__builtin_trap`. That path does not write. In a constant
expression the call is ill-formed: `__prism_check` is not a constant
expression. The note from clang++ is that `__prism_check` cannot be used in
a constant expression.

When `first == last`, the function returns. That path does not write, even
when the pointer is null. An empty range of null pointers returns. ASan
reported no read and no write. `static_assert` of that empty null call is a
constant expression, against this header and against libstdc++.

A null `first` on a non-empty range calls

`__prism_check(0, "PTR-NULL-DEREF", "std::fill null destination")`

and then `__builtin_trap`. That path does not write. The null check is
after the empty return, so a null empty range does not take it.

Otherwise each element in `[first, last)` is assigned the value. The
assignment is `*__first = __value`, then `++__first`. The loop stops when
`first == last`, so the element at `last` is not written. The function does
not allocate.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. The header includes nothing.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
- `copy`
- `for_each`
- `fill_n`
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
`count` / `all_of` / `any_of` / `none_of` cases, plus `fill`. Those constant
checks use a non-const local array, because a `constexpr` array decays to
`const T*`. They cover a full range, a range that stops before the last
element, an empty range, an empty null range, one element, and a type with
no `operator==` and no default constructor.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The earlier algorithms
are unchanged. `fill` checks `last < first`, then the empty return, then a
null `first`, all before any store of `__value`. The store is
`*__first = __value` and the following `++__first` is the only advance.
The name occurs once. `sort(`, `copy(`, `for_each(`, and `fill_n` stay
absent.

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
fill_all=7,7,7,7
fill_span=7,7,7,4
fill_empty=9,4,1,4
fill_one=3
fill_null=1
fill_pred=9,9,9 fill_pred_empty=4,5
```

`fill_all=7,7,7,7` is `{9, 4, 1, 4}` assigned `7` through `last`.
`fill_span=7,7,7,4` assigns the first three elements and leaves the
element at `last` equal to `4`. `fill_empty=9,4,1,4` is an empty range
left unchanged. `fill_one=3` is a one-element range. `fill_null=1` is an
empty null range; ASan reported no read and no write. `fill_pred=9,9,9`
is `PredOnly`, a type with a deleted default constructor and no
`operator==`. `fill_pred_empty=4,5` is that type on an empty range, left
unchanged.

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
`none_of: null predicate`. `invert-fill` aborts with
`CHECK FUNC-CONTRACT: std::fill last is before first` and does not print
`wrote`. `null-fill` aborts with
`CHECK PTR-NULL-DEREF: std::fill null destination` and does not print
`wrote`. None of those runs prints `value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`static_assert(std::min_element(a + 2, a) == a)` is not a constant
expression. A `constexpr` function that calls `std::fill` on a non-const
local array with `last < first` is not a constant expression: the note
from clang++ is that `__prism_check` cannot be used in a constant
expression. A `constexpr` call with a null `first` and `last` equal to
one past an array element is not a constant expression either: the note
from clang++ is that the comparison between that element and `nullptr`
has an unspecified value, so constant evaluation stops on `last < first`
and does not reach `__prism_check`. The runtime `null-fill` path is the
one that reports `PTR-NULL-DEREF`. `static_assert` of an empty range,
including an empty null range, keeps the original elements, against this
header and against libstdc++.

`std::fill` of `int*` and `const int&` compiles against libstdc++ and
against this header. `std::fill` of `int*` and `long` compiles against
libstdc++ and fails against this header (`deduced conflicting types`).
`std::fill_n`, `std::sort`, `std::copy`, and `std::for_each` compile
against libstdc++ and fail against this header.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.
