# array equality

Branch `finish/cxx-array-equal`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`operator==` is a non-member on the existing header
`src/prism/pir/models/cxx/array`. That path is a file. A directory of the
same name would make `#include <array>` fail with EISDIR. The include guard
is still `_PRISM_MODEL_ARRAY`, and `_GLIBCXX_ARRAY` is still defined.
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two arrays is not a fuzzer `CLEAN`
result. The header says this is not PROVED and not CLEAN.

## What equality does

One function template, the same `T` and the same `N`:

```
constexpr bool operator==(const array<_Tp, _Nm>&, const array<_Tp, _Nm>&)
```

`array<T, 0>` compared with another `array<T, 0>` is true. That arm returns
before any element access. It does not read an element and it does not
invent one. A type with no default constructor is a valid element type for
`N == 0`; the comparison does not construct a `T`.

For `N > 0` the result is true only when every corresponding element
compares equal with `operator==`. The walk is index order. A mismatch
returns false and does not read a later index. The element expression is
`__x._M_elems[__i] == __y._M_elems[__i]`, so a member `operator==` and a
free `operator==` both apply. No stand-in element is created.

A different `N` is not an overload. `array<int, 1>` and `array<int, 2>`
deduce conflicting `_Nm` and the comparison is ill-formed. There is no
second template that accepts two sizes.

`operator!=` is not declared. `operator<=>` is still not declared.

## What stayed

`at()`, `operator[]`, `front`, `back`, `fill`, `swap`, and `data()` are
unchanged, including on the `N == 0` specialization. That specialization
is still a type with no element. `empty()` is still true, `size()` and
`max_size()` are still 0, `fill` and `swap` still do nothing, and `data()`
is still a null pointer. `front`, `back`, and `operator[]` on `N == 0` are
still the precondition and then not a value. `at()` still throws
`out_of_range` for every index.

The header still does not include `<algorithm>`, `<vector>`, or `<memory>`.
Equality does not call `std::equal` and it does not use iterators.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- iterators and reverse iterators
- `operator<=>`
- `std::get`, `tuple_size`, `tuple_element`
- `std::to_array`

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/array` was
already in `PRISM_PIR_MODELS`. There is still no
`tests/cpp/test_cxx_array.cpp`. `tests/cpp/test_qa_conformance.cpp` was not
edited. `array` stays in the header-name array. `src/prism/bmc.cpp`,
`src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp` were not edited.

## Tests

`tests/cxx_models/array_trace.cpp` includes `<array>`. Constant checks cover
two empty arrays, an equal triple, mismatches at the first, middle, and
last index, and a one-element array. The runtime lines cover those cases,
a const array, an array compared with itself, and a type with no default
constructor. That type's `operator==` counts calls. A negative value sits
past a mismatch and must not be read.

`array_equal_true` in `testdata/conformance/libc-models/array_contracts.cpp`
requires equal triples, a first-element mismatch, a middle mismatch, a last
element that matches only when the parameter is 7, and a one-element
mismatch. `array_equal_empty_true` requires two `array<int, 0>` values to
compare equal. `array_equal_mismatch_false` asserts that a first-element
mismatch compares equal. That assert fails. Its class is `FUNC-CONTRACT`.
The yml `expected` values are booleans. They are not `PROVED` and not
`CLEAN`.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<array>`, and once against libstdc++.
Sibling model headers were not on that include path. Stdout matched,
including the previous construction lines. Equality lines:

```
eq same=1 mid=0 head=0 tail=0 one=1 oned=0 self=1 csame=1 cdiff=0 zero=1
eqm same=1 mid=0 head=0 tail=0 zero=1 poison=0 calls same=3 mid=2 head=1 tail=3 zero=0 poison=2 poison_reads=0
```

`zero=1` is two `array<int, 0>` values. `calls zero=0` is two
`array<MemEq, 0>` values: `MemEq` has no default constructor, and
`operator==` was not called. `calls head=1` is a mismatch at index 0.
`calls mid=2` is a mismatch at index 1. `poison=0` with `poison_reads=0`
is `{1, 2, -1}` against `{1, 9, -1}`: the result is false after two
comparisons, and the negative element is not read. Both binaries exited 0.
`argv` `oob` still aborts in `operator[]` on `__n < this->size()`.
`array<int, 1> == array<int, 2>` is ill-formed against this header and
against libstdc++. The contract translation unit compiled both ways.
`prism_tests` was not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::array unencoded` (`is not an array model`) from the source text,
before a header is inlined. Those files were not edited. Until that gate
is gone, these tasks do not reach this header. `docs/PIR.md` was not edited.
