# vector not-equal

Branch `finish/cxx-vector-ne`, base `df8f09a5e6011a990d96ee440ae59f35a01dfefd`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/vector`. That path is a file. A directory of the
same name would make `#include <vector>` fail with EISDIR. The include guard
is still `_PRISM_MODEL_VECTOR`. The file still defines `_GLIBCXX_VECTOR` and
is still a `#pragma GCC system_header`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two vectors for inequality is not a
fuzzer `CLEAN` result. The header says this is not PROVED and not CLEAN.

## What not-equal does

One function template, the same `T` and the same `Alloc`:

```
constexpr bool operator!=(const vector<_Tp, _Alloc>& __x,
                          const vector<_Tp, _Alloc>& __y)
```

The body is `return !(__x == __y)`. It calls the existing container
`operator==`. It does not walk elements, it does not call `begin()` or
`end()`, and it does not return `T{}`.

Two empty vectors compare not-equal as false. `operator==` is still

```
return __x.size() == __y.size() && std::equal(__x.begin(), __x.end(), __y.begin());
```

Sizes are compared first. When both sizes are 0 the `&&` still calls
`std::equal`. That call is libstdc++'s `std::equal` from
`<bits/stl_algobase.h>`, which this header already includes. An empty range
does not load an element. For a pointer range libstdc++ can compare with
`__memcmp`, and that path returns true when the distance is 0 without
calling `__memcmp`. For a class type the loop is `for (; __first1 != __last1; ...)`,
so an empty range does not enter the loop and does not evaluate `*__first1 == *__first2`.
`begin()` and `end()` on an empty vector are the same pointer (`_M_start` and
`_M_finish` are both null, or both the start pointer after `reserve` or
`clear`). `empty()` is still `_M_start == _M_finish`.

A size mismatch makes `operator==` false before `std::equal` is evaluated, so
no element is loaded. A mismatch of equal length stops at the first difference
inside that `std::equal`. `operator!=` is the negation of that result and does
not start a second walk. Equal vectors make `operator==` true, so
`operator!=` is false.

A different `T` or a different `Alloc` is not an overload. There is no
second template.

`operator<=>` is still the lexicographical three-way compare. Iterator
`operator!=` is not declared. Iterators stay
`__gnu_cxx::__normal_iterator`.

## What stayed

`operator==`, `operator<=>`, `swap`, `clear`, and `vector<bool>` are
unchanged. `vector<bool>` is still the incomplete declaration

```
template <typename _Alloc>
class vector<bool, _Alloc>;
```

Member `swap` still swaps the three pointers. `clear` still destroys
`[_M_start, _M_finish)` and sets `_M_finish = _M_start`. The non-member
`swap` still calls the member. Not-equal does not call `__prism_check` or
`__builtin_trap`.

## What still falls through to libstdc++

Not declared as a definition, so a unit that names one fails to compile
against this header and the pir stage lowers it again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `vector<bool>` (the specialization is still only declared)
- `std::pmr::vector` (the alias is declared and the class `static_assert`s
  on `polymorphic_allocator`)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/vector`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_vector_ne.cpp
```

No existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not
edited. `src/prism/pir/models/cxx/algorithm` was not edited. `std::equal` is
the one already included from `<bits/stl_algobase.h>`.
`testdata/conformance/libc-models/vector_contracts.cpp` and its yml were not
edited.

## Tests

`tests/cpp/test_cxx_vector_ne.cpp` reads the header. `operator!=` is
declared once, after `operator==` and before `operator<=>`, as
`return !(__x == __y)`. That body does not contain `!=`, a loop, `begin`,
`end`, `std::equal`, a dereference, `_M_`, `T{}`, `__prism_check`, or
`__builtin_trap`. After comments are stripped, `operator!=` occurs once and
iterator `operator!=` does not. `operator==` is still the previous return:
`size()` is compared before `std::equal`. `operator<=>`, member `swap`,
member `clear`, and the non-member `swap` are still the previous bodies.
`vector<bool>` is still an incomplete declaration. The comment above
`operator!=` says this is not PROVED and not CLEAN.

`tests/cxx_models/vector_trace.cpp` includes `<vector>`. The previous lines
stay. New lines cover two empty vectors, two equal vectors, a mismatch at
the first, middle, and last element, a shorter vector, a longer vector, a
vector compared with itself, two vectors compared after `clear`, two empty
vectors with different `reserve` capacities, a const vector, and a `NoDef`
element type with no default constructor. A `Load` element counts
`operator==` calls, counts a read of a negative value, and counts
constructors. Those counts are the reads inside `operator==`. A negative
value sits past a mismatch and must not be read. A size mismatch must not
call `operator==` on an element.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<vector>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched, 44 lines. Not-equal lines:

```
ne empty=0 same=0 head=1 mid=1 tail=1 short=1 long=1 self=0 clear=0 reserve=0 csame=0 nodef=0
nel empty=0/0 same=0/3 head=1/1 mid=1/2 tail=1/3 len=1/0 lreads=0 poison=1/2 preads=0 eqneg=0/2/1 ctors=0
```

`empty=0` is two empty `vector<int>` values: not-equal is false.
`nel` `empty=0/0` is two empty `vector<Load>` values: not-equal is false
and `Load::operator==` was not called. `ctors=0` is the sum of `Load`
constructors during those comparisons. `same=0/3` is two equal vectors of
three elements: three element comparisons and a false result. `head=1/1`
is a mismatch at the first element. `mid=1/2` is a mismatch at the second
element. `tail=1/3` is a mismatch at the last element. `len=1/0` with
`lreads=0` is `{1, -1} != {1}`: true because the sizes differ, with no
element comparison and no read of the negative value. `poison=1/2` with
`preads=0` is `{1, 2, -1} != {1, 9, -1}`: true after two comparisons, and
the negative element is not read. `eqneg=0/2/1` is `{1, -1} != {1, -1}`:
false after two comparisons, and the matching negative element is read.
`nodef=0` is two empty `vector<NoDef>` values. `NoDef` has no default
constructor. Both binaries exited 0.

The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and run. The not-equal case passed
(64 assertions). `prism_tests` was not run.

libstdc++ 14 `<vector>` in this mode has no separate container
`operator!=`. That overload is compiled only when
`__cpp_lib_three_way_comparison` is not set. In C++23, `!=` is the rewritten
negation of `operator==`. This header declares the container overload and
does not declare iterator `operator!=`.
