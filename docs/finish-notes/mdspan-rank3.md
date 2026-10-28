# mdspan rank 3

Branch `finish/cxx-mdspan-rank3`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::mdspan` stays one extensionless file, `src/prism/pir/models/cxx/mdspan`.
A directory of that name would make `#include <mdspan>` fail with EISDIR.
The include guard is still `_GLIBCXX_MDSPAN`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof.

## What changed

Rank 1 (`mdspan<T>`, one extent) and rank 2 (`mdspan<T, 2>`, two extents) are
the same specializations as before. Rank 3 is `mdspan<T, 3>`: construct from
a non-owning `T*` and three extents. The destructor is defaulted. Nothing is
allocated, and destroying the view does not free the pointer.

A null data pointer with a non-zero extent is `__prism_check` with class
`PTR-NULL-DEREF` (`std::mdspan null data pointer with a non-zero extent`).
All three extents zero may be a null pointer.

`operator()(i, j, k)` with any index outside its extent is `__prism_check`
with class `MEM-OOB-READ` (`std::mdspan::operator() index is outside the
extents`). The in-range address is `i * extent(1) * extent(2) + j *
extent(2) + k` in the caller's array (contiguous, row-major). That is not a
`layout_right` object. The call returns the caller's element.

`extent(0)`, `extent(1)`, and `extent(2)` are the three stored extents.
`size()` is their product when that product fits in `size_t`. A zero extent
makes the product 0, including when the other two would overflow each other,
and that path does not fire the overflow check. When the three-extent
product does not fit, the check is `MEM-OOB-READ` (`std::mdspan extents
product overflows`) and `size()` stays 0. The store is `if (__fits) _M_size
= __prod`. A wrapped count is not stored.

Rank 2 still checks its product the same way (`__e1 == 0 || __e0 <=
SIZE_MAX / __e1`) and still assigns `_M_size = __e0 * __e1` only when that
check passes, so an overflowing rank-2 product stays 0.

`data()` is the pointer that was passed in. A deduction guide maps
`(T*, size_t, size_t, size_t)` to `mdspan<T, 3>`.

## What falls through

The primary template stays a declaration:

```cpp
template <typename _Tp, std::size_t _Rank = 1>
class mdspan;
```

Rank other than 1, 2, and 3 is that incomplete type, so the unit fails to
compile and the pir stage lowers it again with libstdc++
(`extra.cxx_models`). The standard template argument list (`Extents`,
`LayoutPolicy`, `AccessorPolicy`) is not `mdspan<T>` / `mdspan<T, 2>` /
`mdspan<T, 3>`.

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

`tests/cpp/test_cxx_mdspan.cpp` still reads the header. It now requires three
defaulted destructors, three quoted `PTR-NULL-DEREF` checks, and three
`operator()` bodies that call `__prism_check` (`MEM-OOB-READ`) before they
index `_M_ptr`. Rank 2's overflow lines are still present. Rank 3 stores
`_M_size` only from `__prod` after the fit check, and the header has no
`_M_size = __e0 * __e1 * __e2`. The primary `class mdspan;` remains, and
there is no `mdspan<_Tp, 4>` specialization.

`tests/cxx_models/mdspan_trace.cpp` adds the rank-3 cases. The in-range run
still prints the rank-1 and rank-2 lines, then:

```
r3 24 2 3 4 1 24
z3 0
e3 0 1
```

`r3` is extents `(2, 3, 4)`, `size()` 24, `(0, 0, 0)` equal to 1, and
`(1, 2, 3)` equal to 24 (the last element). `z3` is extents
`(SIZE_MAX, 2, 0)`: the product is 0, so `size()` is 0 and the overflow
check does not fire. `e3` is a null pointer with extents `(0, 0, 0)`.

`operator()(2, 0, 0)`, `operator()(0, 3, 0)`, and `operator()(0, 0, 4)` on
extents `(2, 3, 4)` abort with `MEM-OOB-READ`. A null pointer with extents
`(0, 0, 1)` aborts with `PTR-NULL-DEREF`. Extents `(SIZE_MAX, 2, 2)` abort
with `std::mdspan extents product overflows`, and so does rank 2 with
extents `(SIZE_MAX, 2)`. `mdspan<int, 4>` is an implicit instantiation of
the undefined primary template. `mdspan<int, E, L, A>` does not match the
`size_t` rank parameter.

Checked with Ubuntu clang++ 18.1.3 (ASan/UBSan), `-std=c++23 -O0 -g
-fsanitize=address,undefined -fno-sanitize-recover=all`. The success path
printed the lines above and exited 0. The failing modes aborted (`SIGABRT`),
including `operator()(0, 0, 4)` built at `-O1`. Those are the check, not an
ASan heap error. `prism_tests` was not run. cmake was not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not an
mdspan model" from the source text (`std::mdspan` or `mdspan<`) before the
header is inlined. `src/prism/bmc_encoder.inc` still throws `mdspan unencoded`
on the same spellings. Those files were not edited. Until that gate is gone,
a task that spells `std::mdspan` or `mdspan<` stays on that path.
`docs/PIR.md` was not edited.
