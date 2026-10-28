# algorithm copy

Branch `finish/cxx-algorithm-copy`, base `72a1cc5a5402874a154e9266b9807cf1d70f0db6`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`. Nothing here is `CLEAN`. After a failed check the
path traps. It does not continue into a read, a write, or a fabricated
success.

## What this slice adds

`std::min`, `std::max`, `std::clamp`, `std::min_element`, `std::max_element`,
`std::find`, `std::find_if`, `std::count`, `std::all_of`, `std::any_of`,
`std::none_of`, and `std::fill` are unchanged.

`std::copy` is one overload:

`_Tp* copy(_Tp* first, _Tp* last, _Tp* dest)`

There is no second overload. There is no execution policy. `T` is deduced
from all three pointers. A `const int*` source and an `int*` destination
do not match. That call fails to compile against this header
(`deduced conflicting types for parameter '_Tp'`), and the pir stage falls
back to libstdc++. The element type is not required to be
equality-comparable or less-than-comparable. A type with a deleted default
constructor and no `operator==` is a valid element type.

When `last < first`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::copy last is before first")`

and then `__builtin_trap`. That path does not read an element and does not
write. It does not look at `dest`. In a constant expression the call is
ill-formed: `__prism_check` is not a constant expression. The note from
clang++ is that `__prism_check` cannot be used in a constant expression.

When `first == last`, the function returns `dest`. That path does not read
and does not write, even when `dest` is null. An empty range of null
pointers returns null. ASan reported no read and no write.
`static_assert` of that empty null call is a constant expression, against
this header and against libstdc++.

A null `dest` on a non-empty range calls

`__prism_check(0, "PTR-NULL-DEREF", "std::copy null destination")`

and then `__builtin_trap`. That path does not write. The null check is
after the empty return, so a null empty range does not take it. In a
constant expression with a real source array and a null `dest`, evaluation
reaches `__prism_check`. The note from clang++ is that `__prism_check`
cannot be used in a constant expression. The call in that note is
`copy<int>(&a[0], &a[3], nullptr)`.

When `dest` lies in `[first, last)`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::copy dest lies in [first, last)")`

and then `__builtin_trap`. That path does not write. The scan is a walk
from `first` to `last` that compares each pointer with `dest` using `==`.
It does not dereference an element. `dest == last` is outside that
half-open range, so an adjacent copy that starts at `last` is not this
check. In a constant expression the overlap call is ill-formed at
`__prism_check`. The note from clang++ shows the call
`copy<int>(&a[0], &a[3], &a[1])`.

Otherwise each element in `[first, last)` is assigned from the source to
the destination: `*__out = *__first`, then `++__first`, then `++__out`.
The loop stops when `first == last`, so the element at `last` is not read
and is not written. The result is the destination pointer one past the
last element written. An empty range returns `dest` itself. The function
does not allocate.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. The header includes nothing.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
- `copy_n`
- `copy_backward`
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
`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. The existing `std::clamp`
unencoded gate is unchanged.

## Tests

`tests/cxx_models/algorithm_trace.cpp` includes `<algorithm>`. It is compiled
twice (model directory as `-isystem`, and without it). Constant checks cover
the previous `min` / `max` / `clamp` / element / `find` / `find_if` /
`count` / `all_of` / `any_of` / `none_of` / `fill` cases, plus `copy`. Those
constant checks use a non-const local array, because a `constexpr` array
decays to `const T*`. They cover a full range into a separate buffer, a
prefix that leaves the source element at `last` and the destination element
past the copied prefix unchanged, an empty range, an empty null range, one
element, a type with no `operator==` and no default constructor, and an
adjacent copy whose destination is `last`.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The earlier algorithms
are unchanged. `copy` checks `last < first`, then the empty return of
`dest`, then a null `dest`, then the equality scan, all before any
`*__out = *__first`. The scan body has no `*__` and no `<`. The store is
`*__out = *__first`, followed by `++__first` and `++__out`. The name occurs
once. `sort(`, `for_each(`, `fill_n`, `copy_n(`, and `copy_backward(` stay
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
copy_all=9,4,1,4 end=1 src=9,4,1,4
copy_span=9,4,1,8 end=1
copy_empty=8,7,6,5 end=1
copy_one=8 end=1
copy_null=1
copy_pred=3,1,3,5 copy_pred_empty=4,5
copy_adj=1,2,3,1,2,3 end=1
```

`copy_all=9,4,1,4 end=1 src=9,4,1,4` copies `{9, 4, 1, 4}` into a separate
buffer and leaves the source unchanged. The returned pointer is one past
the last written element. `copy_span=9,4,1,8 end=1` copies the first three
elements and leaves the destination element at the end equal to `8`.
`copy_empty=8,7,6,5 end=1` is an empty range: the destination is unchanged
and the returned pointer is `dest`. `copy_one=8 end=1` is a one-element
range. `copy_null=1` is an empty null range; ASan reported no read and no
write. `copy_pred=3,1,3,5` is `PredOnly`, a type with a deleted default
constructor and no `operator==`, copied into a longer buffer whose last
element stays `5`. `copy_pred_empty=4,5` is that type on an empty range,
left unchanged. `copy_adj=1,2,3,1,2,3 end=1` copies `[first, last)` onto
`last` in a longer array. That destination is not in `[first, last)`.

`argv` `invert` still aborts with `CHECK FUNC-CONTRACT: clamp: hi < lo`.
The earlier `invert-*`, `null-*`, `invert-fill`, and `null-fill` runs are
unchanged. `invert-copy` aborts with
`CHECK FUNC-CONTRACT: std::copy last is before first` and does not print
`wrote` or `read`. `null-copy` aborts with
`CHECK PTR-NULL-DEREF: std::copy null destination` and does not print
`wrote` or `read`. `overlap-copy` (destination at the last element of the
source range) and `overlap-copy-first` (destination equal to `first`) abort
with `CHECK FUNC-CONTRACT: std::copy dest lies in [first, last)` and do not
print `wrote` or `read`. The element type used there assigns with a
`wrote` trace and compares elements with a `read` trace. Pointer equality
does not call that comparison. None of those runs prints `value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
A `constexpr` function that calls `std::copy` with `last < first`, with a
null `dest` on a non-empty range, or with `dest` inside `[first, last)` is
not a constant expression: the note from clang++ is that `__prism_check`
cannot be used in a constant expression. `static_assert` of an empty range,
including an empty null range, returns `dest` and keeps the original
elements, against this header and against libstdc++.

`std::copy` of three `int*` pointers compiles against libstdc++ and against
this header. `std::copy` of `const int*` to `int*` compiles against
libstdc++ and fails against this header. `std::copy_n`,
`std::copy_backward`, `std::sort`, and `std::for_each` compile against
libstdc++ and fail against this header.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.
