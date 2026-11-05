# array not-equal

Branch `finish/cxx-array-ne`, base `12a7a141fc4bf5bc9094cbff46da82bc98ad1370`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/array`. That path is a file. A directory of the
same name would make `#include <array>` fail with EISDIR. The include guard
is still `_PRISM_MODEL_ARRAY`, and `_GLIBCXX_ARRAY` is still defined.
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two arrays for inequality is not a
fuzzer `CLEAN` result. The header says this is not PROVED and not CLEAN.

## What not-equal does

One function template, the same `T` and the same `N`:

```
constexpr bool operator!=(const array<_Tp, _Nm>&, const array<_Tp, _Nm>&)
```

The body is `return !(__x == __y)`. It calls the existing `operator==`.
It does not walk elements itself, and it does not construct a `T`.

`array<T, 0>` compared with another `array<T, 0>` is false. `operator==`
returns true before any element access, so the negation is false and no
element is read. A type with no default constructor is a valid element
type for `N == 0`; the comparison does not construct a `T`.

For `N > 0` the result is the negation of element-wise equality. The walk
stays inside `operator==`, in index order. A mismatch returns false from
`operator==` and does not read a later index, so `operator!=` is true and
stops at that same index. Equal arrays make `operator==` true, so
`operator!=` is false. The element expression inside `operator==` is
`__x._M_elems[__i] == __y._M_elems[__i]`, so a member `operator==` and a
free `operator==` both apply. No stand-in element is created.

A different `N` is not an overload. `array<int, 1>` and `array<int, 2>`
deduce conflicting `_Nm` and the comparison is ill-formed. There is no
second template that accepts two sizes.

`operator<=>` is still not declared. `std::get` and `tuple_size` are still
not declared.

## What stayed

`at()`, `operator[]`, `front`, `back`, `fill`, `swap`, `data()`,
`operator==`, `operator<`, `operator>`, `operator<=`, and `operator>=` are
unchanged, including on the `N == 0` specialization. That specialization
is still a type with no element. `empty()` is still true, `size()` and
`max_size()` are still 0, `fill` and `swap` still do nothing, and `data()`
is still a null pointer. `front`, `back`, and `operator[]` on `N == 0` are
still the precondition and then not a value. `at()` still throws
`out_of_range` for every index.

The header still does not include `<algorithm>`, `<vector>`, or `<memory>`.
Not-equal does not call `std::equal` and it does not use iterators.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- iterators and reverse iterators
- `operator<=>`
- `std::get`, `tuple_size`, `tuple_element`
- `std::to_array`

## Wiring

`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped. `src/prism/pir/models/cxx/array`
was already in `PRISM_PIR_MODELS`. There is still no
`tests/cpp/test_cxx_array.cpp`. `tests/cpp/test_qa_conformance.cpp` was not
edited. `array` stays in the header-name array.
`testdata/conformance/libc-models/array_contracts.cpp` and its yml were not
edited.

## Tests

`tests/cxx_models/array_trace.cpp` includes `<array>`. The previous
constant checks for `operator==`, `operator<`, `operator>`, `operator>=`,
and `operator<=` stay. New constant checks cover two empty arrays
(not-equal is false), an equal triple (not-equal is false), mismatches at
the first, middle, and last index, and a one-element array in both
directions.

The runtime lines cover those cases, a const array, an array compared with
itself, and a type with no default constructor. `MemEq::operator==` counts
calls and counts a read of a negative value. Those counts are the reads
inside `operator==`. A negative value sits past a mismatch and must not be
read.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<array>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched, including the previous construction,
equality, less, greater, greater-or-equal, and less-or-equal lines.
Not-equal lines:

```
ne same=0 mid=1 head=1 tail=1 one=0 oned=1 self=0 csame=0 cdiff=1 zero=0
nem same=0 mid=1 head=1 tail=1 zero=0 poison=1 calls same=3 mid=2 head=1 tail=3 zero=0 poison=2 poison_reads=0
```

`zero=0` on the first line is two `array<int, 0>` values: not-equal is
false. `nem` `zero=0` with `calls zero=0` is two `array<MemEq, 0>` values:
`MemEq` has no default constructor, and `operator==` was not called on an
element. `calls same=3` is two equal arrays: three element comparisons and
a false result. `calls head=1` is a mismatch at index 0. `calls mid=2` is
a mismatch at index 1. `calls tail=3` is a mismatch at the last index.
`poison=1` with `calls poison=2` and `poison_reads=0` is
`{1, 2, -1} != {1, 9, -1}`: true after two comparisons, and the negative
element is not read. Both binaries exited 0. `argv` `oob` still aborts in
`operator[]` on `__n < this->size()`. `array<int, 1> != array<int, 2>` is
ill-formed against this header (`deduced conflicting values for parameter
'_Nm'`) and against libstdc++. libstdc++ `<array>` in this mode has no
separate `operator!=`; `!=` is the rewritten negation of its `operator==`.
`prism_tests` was not run.
