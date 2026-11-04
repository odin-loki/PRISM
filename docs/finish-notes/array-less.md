# array less

Branch `finish/cxx-array-less`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`operator<` is a non-member on the existing header
`src/prism/pir/models/cxx/array`. That path is a file. A directory of the
same name would make `#include <array>` fail with EISDIR. The include guard
is still `_PRISM_MODEL_ARRAY`, and `_GLIBCXX_ARRAY` is still defined.
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Ordering two arrays is not a fuzzer `CLEAN`
result. The header says this is not PROVED and not CLEAN.

## What less does

One function template, the same `T` and the same `N`:

```
constexpr bool operator<(const array<_Tp, _Nm>&, const array<_Tp, _Nm>&)
```

`array<T, 0>` compared with another `array<T, 0>` is false. That arm is
`if constexpr (_Nm == 0) return false` and it returns before any element
access. It does not read an element and it does not invent one. A type
with no default constructor is a valid element type for `N == 0`; the
comparison does not construct a `T`.

For `N > 0` the walk is index order, `0 <= i < N`. Each index compares the
elements with `operator<`:

- `__x._M_elems[i] < __y._M_elems[i]` returns true. A later index is not
  read.
- Otherwise `__y._M_elems[i] < __x._M_elems[i]` returns false. The left
  element is greater, and a later index is not read.
- Otherwise the two elements are equivalent and the walk continues.

When every element through `N` is equivalent, the result is false. The
loop condition is `__i < _Nm`, so an index past `N` is not read. The
element expression names `_M_elems`, so a member `operator<` and a free
`operator<` both apply. `operator==` is not used. No stand-in element is
created.

A different `N` is not an overload. `array<int, 1>` and `array<int, 2>`
deduce conflicting `_Nm` and the comparison is ill-formed. There is no
second template that accepts two sizes.

`operator>`, `operator<=`, `operator>=`, and `operator!=` are not
declared. `operator<=>` is still not declared.

## What stayed

`at()`, `operator[]`, `front`, `back`, `fill`, `swap`, `data()`, and
`operator==` are unchanged, including on the `N == 0` specialization.
That specialization is still a type with no element. `empty()` is still
true, `size()` and `max_size()` are still 0, `fill` and `swap` still do
nothing, and `data()` is still a null pointer. `front`, `back`, and
`operator[]` on `N == 0` are still the precondition and then not a value.
`at()` still throws `out_of_range` for every index.

The header still does not include `<algorithm>`, `<vector>`, or
`<memory>`. Less does not call `std::lexicographical_compare` and it does
not use iterators.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- iterators and reverse iterators
- `operator<=>`
- `operator>`, `operator<=`, `operator>=`, and `operator!=`
- `std::get`, `tuple_size`, `tuple_element`
- `std::to_array`

## Wiring

`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped. `src/prism/pir/models/cxx/array`
was already in `PRISM_PIR_MODELS`. There is still no
`tests/cpp/test_cxx_array.cpp`. `tests/cpp/test_qa_conformance.cpp` was not
edited. `array` stays in the header-name array. `src/prism/bmc.cpp`,
`src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp` were not edited.
`testdata/conformance/libc-models/array_contracts.cpp` and its yml were not
edited.

## Tests

`tests/cxx_models/array_trace.cpp` includes `<array>`. The previous
constant checks for `operator==` stay. New constant checks cover two empty
arrays (not less), an equivalent triple, a middle element that is less, a
first element that is less, a last element that is less, a last element
that is greater, a first element that is greater while a later element
would be less, and a one-element array in both directions and when the
elements are equivalent.

The runtime lines cover those cases, a const array, an array compared with
itself, and two types with no default constructor. `MemLt::operator<`
counts calls and counts a read of a negative value. `NeverLt::operator<`
always returns false, so the elements are equivalent through `N` even when
the stored ints differ. A negative value sits past the deciding element
and must not be read.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<array>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched, including the previous construction and
equality lines. Less lines:

```
lt same=0 mid=1 head=1 tail=1 gt=0 early=0 one=1 oned=0 oneeq=0 self=0 csame=0 cdiff=1 zero=0
ltm same=0 mid=1 head=1 tail=1 gt=0 early=0 zero=0 poison=1 pgt=0 never=0 nzero=0 calls same=6 mid=3 head=1 tail=5 gt=6 early=2 zero=0 poison=3 pread=0 pgt=2 pgread=0 never=6 nzero=0
```

`zero=0` on the first line is two `array<int, 0>` values. `ltm` `zero=0`
with `calls zero=0` is two `array<MemLt, 0>` values: `MemLt` has no
default constructor, and `operator<` was not called. `calls head=1` is a
less element at index 0, so the later elements are not read. `calls mid=3`
is equivalent at index 0 (two calls) and less at index 1 (one call).
`calls tail=5` is less only at the last index. `calls gt=6` is greater at
the last index: both directions of `operator<` run there, and the result
is false. `calls early=2` is greater at index 0, so the later elements,
which would be less, are not read. `poison=1` is the result of
`{1, 2, -1} < {1, 9, -1}`: true after three comparisons (`calls poison=3`)
and `pread=0` means the negative element is not read. `pgt=0` with
`calls pgt=2` and `pgread=0` is `{2, -1, -1} < {1, 0, 0}`: false at index
0, and the negative elements are not read. `never=0` with `calls never=6`
is two `array<NeverLt, 3>` values whose ints differ: six `operator<` calls
and a false result, because neither element is less. `nzero=0` with
`calls nzero=0` is two `array<NeverLt, 0>` values. Both binaries exited 0.
`argv` `oob` still aborts in `operator[]` on `__n < this->size()`.
`array<int, 1> < array<int, 2>` is ill-formed against this header
(`deduced conflicting values for parameter '_Nm'`) and against libstdc++.
`prism_tests` was not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::array unencoded` (`is not an array model`) from the source text,
before a header is inlined. Those files were not edited. Until that gate
is gone, these tasks do not reach this header. `docs/PIR.md` was not edited.
