# algorithm find

Branch `finish/cxx-algorithm-find`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`.

## What this slice adds

`std::min`, `std::max`, `std::clamp`, `std::min_element`, and
`std::max_element` are unchanged.

`clamp(v, lo, hi)` when `hi < lo` still calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`.

`std::find` is one overload: `_Tp* find(_Tp* first, _Tp* last, const _Tp& value)`.
There is no predicate, no execution policy, and no second template
parameter. The type must be comparable with `operator==`.

When `last < first`, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message `find: last < first`, then `__builtin_trap`.
That path does not read an element and does not invent a `T`. In a constant
expression the call is ill-formed: `__prism_check` is not a constant
expression.

When `first == last`, the result is `last`. That path does not read an
element and does not use the value. An empty range of null pointers returns
null. A type with a deleted default constructor is a valid element type.

Otherwise the result is a pointer to the first element in `[first, last)`
that compares equal with `operator==`, or `last` if none. The match returns
before the pointer advances, so the earlier of two equals is the result.
The function does not allocate and does not write the range.

`T` is deduced from the pointers and from the value. A `const int*` and an
`int` value deduce different types (`const int` and `int`), so that call
does not match. A `long` value against `int*` does not match either. Those
calls fail to compile against this header, and the pir stage falls back to
libstdc++.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
- `copy`
- `fill`
- `for_each`
- `find_if`
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
the previous `min` / `max` / `clamp` / element cases. `find` constant checks
use a non-const local array, because a `constexpr` array decays to
`const T*`. They cover a hit, the first of two equals, a front hit, a miss,
an empty range, a tie, a null empty range, a type with only `operator==`,
and a type with no default constructor.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The inverted-clamp path
and both element algorithms are unchanged. `find` checks `last < first`
before the empty return, that return is before any dereference, and the
empty slice does not mention the value. The comparison is `*__first ==
__value`, the hit returns before `++__first`, and the body has no `new` or
`malloc`. `sort(`, `copy(`, `fill(`, and `for_each(` stay absent.
`find_if` stays absent.

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
```

`find_hit_i=2` is the `1` in `{9, 4, 1, 4}`. `find_first_i=1` is the first
`4`, not the second. `find_front_i=0` is the leading `9`. `find_miss=1` is
no `7`. `find_empty=1` returns the start pointer. `find_one=1` finds `8`
and misses `0` in a one-element range. `find_null=1` is an empty null
range; ASan reported no read. `find_keep=1` is the range still
`{9, 4, 1, 4}` after the searches. `find_eq_id=1` is a pointer to the
element, not to the needle. `find_nodef` is a type with a deleted default
constructor: the match is `1` at index 1, and the empty range returns the
start pointer.

`argv` `invert` aborts with `CHECK FUNC-CONTRACT: clamp: hi < lo`.
`invert-min` and `invert-max` abort with `min_element: last < first` and
`max_element: last < first`. `invert-find` aborts with
`CHECK FUNC-CONTRACT: find: last < first`. None of those runs prints
`value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`static_assert(std::min_element(a + 2, a) == a)` is not a constant
expression. A `constexpr` function that calls `std::find` on a non-const
local array with `last < first` is not a constant expression: the note from
clang++ is that `__prism_check` cannot be used in a constant expression.
`std::find` of `int*` and `const int&` compiles against libstdc++ and
against this header. `std::find` of `int*` and `long`, and of `const int*`
and `int`, compile against libstdc++ and fail against this header
(`deduced conflicting types`). `std::find_if` compiles against libstdc++
and fails against this header (`no member named 'find_if'`). `std::sort`,
`std::copy`, `std::fill`, and `std::for_each` compile against libstdc++ and
fail against this header.

`#include <algorithm>` with `-isystem` on the model directory resolved to
`cxx/algorithm`. cmake, ninja, `prism_tests`, and pir were not run.
