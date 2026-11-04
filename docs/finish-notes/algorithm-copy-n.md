# algorithm copy_n

Branch `finish/cxx-algorithm-copy-n`, base `c164ba71c67bf51330687627679f49368a361928`.

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
`std::none_of`, `std::fill`, and `std::copy` are unchanged.

`std::copy_n` is one overload:

`_Tp* copy_n(_Tp* first, std::size_t n, _Tp* dest)`

There is no second overload. There is no execution policy. `T` is deduced
from the two pointers. The count is `std::size_t`, not a second template
parameter. A `const int*` source and an `int*` destination do not match.
That call fails to compile against this header. The note from clang++ is
`deduced conflicting types for parameter '_Tp' ('const int' vs. 'int')`,
and the pir stage falls back to libstdc++. The element type is not required
to be equality-comparable or less-than-comparable. A type with a deleted
default constructor and no `operator==` is a valid element type.

`std::size_t` in this header is

`typedef __SIZE_TYPE__ size_t;`

inside namespace `std`, the same typedef `bits/c++config.h` uses. The
header still includes nothing. A later standard header may repeat that
typedef for the same type.

When `n == 0`, the function returns `dest`. That path does not read and
does not write, even when `dest` or the source is null. An empty count of
two null pointers returns null. A null source with a real destination
returns that destination and leaves it unchanged. A real source with a null
destination returns null and leaves the source unchanged. ASan reported no
read and no write. `static_assert` of those empty calls is a constant
expression, against this header and against libstdc++.

A null source with `n > 0` calls

`__prism_check(0, "PTR-NULL-DEREF", "std::copy_n null source")`

and then `__builtin_trap`. That path does not read. The null-source check
is after the empty return, so a null source with `n == 0` does not take it.
When both pointers are null and `n > 0`, this is the check that runs. In a
constant expression the call is ill-formed: `__prism_check` is not a
constant expression. The note from clang++ is that `__prism_check` cannot
be used in a constant expression. The call in that note is
`copy_n<int>(nullptr, 3, &b[0])`.

A null `dest` with `n > 0` calls

`__prism_check(0, "PTR-NULL-DEREF", "std::copy_n null destination")`

and then `__builtin_trap`. That path does not write. The destination check
is after the source check, so a null source is reported first. In a
constant expression the call is ill-formed at `__prism_check`. The note
from clang++ shows the call `copy_n<int>(&a[0], 3, nullptr)`.

Otherwise each element is assigned from the source to the destination:
`*__out = *__first`, then `++__first`, then `++__out`, then `--__n`. The
loop stops when `n == 0`, so exactly `n` elements are written. The element
one past the last element written is not written. The result is the
destination pointer one past the last element written. An empty count
returns `dest` itself. The function does not allocate. It does not scan
for a destination that lies in the `n` elements being read.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. The header includes nothing.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
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
`count` / `all_of` / `any_of` / `none_of` / `fill` / `copy` cases, plus
`copy_n`. Those constant checks use a non-const local array, because a
`constexpr` array decays to `const T*`. They cover a full count into a
separate buffer, a count that leaves the destination element one past the
last written element unchanged, an empty count, an empty null count, a null
source with an empty count, a null destination with an empty count, one
element, a type with no `operator==` and no default constructor, and an
adjacent copy of `n` elements onto the pointer `n` past the source.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The earlier algorithms
are unchanged. `copy_n` checks `n == 0`, then a null source, then a null
destination, all before any `*__out = *__first`. The store is
`*__out = *__first`, followed by `++__first`, `++__out`, and `--__n`. The
name occurs once. `sort(`, `for_each(`, `fill_n`, and `copy_backward(` stay
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
copy_n_all=9,4,1,4 end=1 src=9,4,1,4
copy_n_span=9,4,1,8 end=1
copy_n_empty=8,7,6,5 end=1
copy_n_one=8 end=1
copy_n_null=1
copy_n_null_src=8,7,6,5 end=1
copy_n_null_dst=1 src=9,4,1,4
copy_n_pred=3,1,3,5 copy_n_pred_empty=4,5
copy_n_adj=1,2,3,1,2,3 end=1
```

`copy_n_all=9,4,1,4 end=1 src=9,4,1,4` copies four `int`s into a separate
buffer and leaves the source unchanged. The returned pointer is one past
the last written element. `copy_n_span=9,4,1,8 end=1` copies three elements
and leaves the destination element one past those three equal to `8`.
`copy_n_empty=8,7,6,5 end=1` is a zero count: the destination is unchanged
and the returned pointer is `dest`. `copy_n_one=8 end=1` is one element.
`copy_n_null=1` is a zero count of two null pointers. `copy_n_null_src`
is a null source and a zero count; the destination stays `8,7,6,5`.
`copy_n_null_dst=1 src=9,4,1,4` is a null destination and a zero count; the
source stays unchanged and the result is null. ASan reported no read and
no write on those empty paths. `copy_n_pred=3,1,3,5` is `PredOnly`, a type
with a deleted default constructor and no `operator==`, copied into a
longer buffer whose last element stays `5`. `copy_n_pred_empty=4,5` is
that type with a zero count, left unchanged. `copy_n_adj=1,2,3,1,2,3 end=1`
copies three elements onto the pointer three past the source.

`argv` `null-copy-n-source` aborts with
`CHECK PTR-NULL-DEREF: std::copy_n null source` and does not print `wrote`,
`read`, or `value=`. `null-copy-n-dest` aborts with
`CHECK PTR-NULL-DEREF: std::copy_n null destination` and does not print
`wrote`, `read`, or `value=`. `null-copy-n-both` aborts with the null-source
message. The earlier `invert-*`, `null-*`, `invert-copy`, `null-copy`, and
`overlap-copy*` runs are unchanged.

`std::copy_n` of two `int*` pointers and a `std::size_t` compiles against
libstdc++ and against this header. An `int` count converts to `std::size_t`
and compiles against both. `std::copy_n` of `const int*` to `int*` compiles
against libstdc++ and fails against this header. `std::copy_backward`,
`std::sort`, and `std::for_each` compile against libstdc++ and fail against
this header.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.
