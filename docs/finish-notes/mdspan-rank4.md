# mdspan rank 4

Branch `finish/cxx-mdspan-rank4`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.
`std::mdspan` stays one extensionless file, `src/prism/pir/models/cxx/mdspan`.
A directory of that name would make `#include <mdspan>` fail with EISDIR.
The include guard is still `_GLIBCXX_MDSPAN`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof.

## What changed

Rank 1 (`mdspan<T>`, one extent), rank 2 (`mdspan<T, 2>`, two extents), and
rank 3 (`mdspan<T, 3>`, three extents) are the same specializations as before.
Rank 4 is `mdspan<T, 4>`: construct from a non-owning `T*` and four extents.
The destructor is defaulted. Nothing is allocated, and destroying the view
does not free the pointer.

A null data pointer with a non-zero extent is `__prism_check` with class
`PTR-NULL-DEREF` (`std::mdspan null data pointer with a non-zero extent`).
All four extents zero may be a null pointer.

`operator()(i, j, k, l)` with any index outside its extent is `__prism_check`
with class `MEM-OOB-READ` (`std::mdspan::operator() index is outside the
extents`). The in-range address is `i * extent(1) * extent(2) * extent(3) +
j * extent(2) * extent(3) + k * extent(3) + l` in the caller's array
(contiguous, row-major). That is not a `layout_right` object. The call
returns the caller's element.

`extent(0)`, `extent(1)`, `extent(2)`, and `extent(3)` are the four stored
extents. `size()` is their product when that product fits in `size_t`. A
zero extent makes the product 0, including when the other three would
overflow each other, and that path does not fire the overflow check. When
the four-extent product does not fit, the check is `MEM-OOB-READ`
(`std::mdspan extents product overflows`) and `size()` stays 0. The store is
`if (__fits) _M_size = __prod`. A wrapped count is not stored.

Rank 2 still checks its product the same way (`__e1 == 0 || __e0 <=
SIZE_MAX / __e1`) and still assigns `_M_size = __e0 * __e1` only when that
check passes, so an overflowing rank-2 product stays 0. Rank 3 still stores
`_M_size` only from `__prod` after its fit check, and the header still has
no `_M_size = __e0 * __e1 * __e2`.

`data()` is the pointer that was passed in. A deduction guide maps
`(T*, size_t, size_t, size_t, size_t)` to `mdspan<T, 4>`.

## What falls through

The primary template stays a declaration:

```cpp
template <typename _Tp, std::size_t _Rank = 1>
class mdspan;
```

Rank other than 1, 2, 3, and 4 is that incomplete type, so the unit fails to
compile and the pir stage lowers it again with libstdc++
(`extra.cxx_models`). The standard template argument list (`Extents`,
`LayoutPolicy`, `AccessorPolicy`) is not `mdspan<T>` / `mdspan<T, 2>` /
`mdspan<T, 3>` / `mdspan<T, 4>`.

Not declared, so they are not stubbed:

- `submdspan` and `mdarray`
- `layout_left`, `layout_right`, `layout_stride`
- `extents`, `dextents`, `default_accessor`
- `operator[]` (the standard spelling)
- iterators

The header does not include `<span>`, `<vector>`, or `<memory>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/mdspan` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_mdspan.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already
lists `mdspan` in the header-name array; that array was not edited.

## Tests

`tests/cpp/test_cxx_mdspan.cpp` still reads the header. It now requires four
defaulted destructors, four quoted `PTR-NULL-DEREF` checks, and four
`operator()` bodies that call `__prism_check` (`MEM-OOB-READ`) before they
index `_M_ptr`. Rank 2's overflow lines are still present. Rank 3 and rank 4
each store `_M_size` only from `__prod` after the fit check, and the header
has no `_M_size = __e0 * __e1 * __e2` and no
`_M_size = __e0 * __e1 * __e2 * __e3`. The primary `class mdspan;` remains.
There is an `mdspan<_Tp, 4>` specialization and no `mdspan<_Tp, 5>`.

`tests/cxx_models/mdspan_trace.cpp` adds the rank-4 cases. The in-range run
still prints the rank-1, rank-2, and rank-3 lines, then:

```
r4 120 2 3 4 5 1 120
z4 0
e4 0 1
```

`r4` is extents `(2, 3, 4, 5)`, `size()` 120, `(0, 0, 0, 0)` equal to 1, and
`(1, 2, 3, 4)` equal to 120 (the last element). `z4` is extents
`(SIZE_MAX, 2, 2, 0)`: the product is 0, so `size()` is 0 and the overflow
check does not fire. `e4` is a null pointer with extents `(0, 0, 0, 0)`.

`operator()(2, 0, 0, 0)`, `operator()(0, 3, 0, 0)`, `operator()(0, 0, 4, 0)`,
and `operator()(0, 0, 0, 5)` on extents `(2, 3, 4, 5)` abort with
`MEM-OOB-READ`. A null pointer with extents `(0, 0, 0, 1)` aborts with
`PTR-NULL-DEREF`. Extents `(SIZE_MAX, 2, 2, 2)` abort with
`std::mdspan extents product overflows`, and so do rank 3 with extents
`(SIZE_MAX, 2, 2)` and rank 2 with extents `(SIZE_MAX, 2)`. `mdspan<int, 5>`
is an implicit instantiation of the undefined primary template.
`mdspan<int, E, L, A>` does not match the `size_t` rank parameter.

Checked with Ubuntu clang++ 18.1.3 (ASan/UBSan), `-std=c++23 -O0 -g
-fsanitize=address,undefined -fno-sanitize-recover=all`. The success path
printed the lines above and exited 0. The failing modes aborted (`SIGABRT`),
including `operator()(0, 0, 0, 5)` built at `-O1`. Those are the check, not an
ASan heap error. A host probe of extents `(SIZE_MAX/2, 2, 1, 1)` stored
`size()` `18446744073709551614`. Extents `(2, 2, 2, SIZE_MAX)` aborted with
the product-overflow check. Extents `(SIZE_MAX, 0, SIZE_MAX, SIZE_MAX)`
printed `size()` 0 and did not fire that check. `prism_tests` was not run.
cmake was not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not an
mdspan model" from the source text (`std::mdspan` or `mdspan<`) before the
header is inlined. `src/prism/bmc_encoder.inc` still throws `mdspan unencoded`
on the same spellings. Those files were not edited. Until that gate is gone,
a task that spells `std::mdspan` or `mdspan<` stays on that path.
`docs/PIR.md` was not edited.
