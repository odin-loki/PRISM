# mdspan rank 7

Branch `finish/cxx-mdspan-rank7`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.
`std::mdspan` stays one extensionless file, `src/prism/pir/models/cxx/mdspan`.
A directory of that name would make `#include <mdspan>` fail with EISDIR.
The include guard is still `_GLIBCXX_MDSPAN`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof.

## What changed

Rank 1 (`mdspan<T>`, one extent), rank 2 (`mdspan<T, 2>`, two extents), rank 3
(`mdspan<T, 3>`, three extents), rank 4 (`mdspan<T, 4>`, four extents), rank 5
(`mdspan<T, 5>`, five extents), and rank 6 (`mdspan<T, 6>`, six extents) are
the same specializations as before. Rank 7 is `mdspan<T, 7>`: construct from
a non-owning `T*` and seven extents. The destructor is defaulted. Nothing is
allocated, and destroying the view does not free the pointer.

A null data pointer with any non-zero extent is `__prism_check` with class
`PTR-NULL-DEREF` (`std::mdspan null data pointer with a non-zero extent`).
All seven extents zero may be a null pointer.

`operator()(i, j, k, l, m, n, p)` with any index outside its extent is
`__prism_check` with class `MEM-OOB-READ`
(`std::mdspan::operator() index is outside the extents`). The call still
indexes after that check, the same way ranks 1 through 6 do. The in-range
address is `i * extent(1) * extent(2) * extent(3) * extent(4) * extent(5) *
extent(6) + j * extent(2) * extent(3) * extent(4) * extent(5) * extent(6) +
k * extent(3) * extent(4) * extent(5) * extent(6) + l * extent(4) *
extent(5) * extent(6) + m * extent(5) * extent(6) + n * extent(6) + p` in
the caller's array (contiguous, row-major). That is not a `layout_right`
object. The call returns the caller's element.

`extent(0)`, `extent(1)`, `extent(2)`, `extent(3)`, `extent(4)`,
`extent(5)`, and `extent(6)` are the seven stored extents. An index past
the rank is `__prism_check` with class `FUNC-CONTRACT`
(`std::mdspan::extent index is outside the rank`), and the function still
returns the last stored extent. `size()` is the product of the seven
extents when that product fits in `size_t`. A zero extent makes the product
0 and skips the divisions, including when the other six would overflow each
other, and that path does not fire the overflow check and does not divide
by zero. When the seven-extent product does not fit, the check is
`MEM-OOB-READ` (`std::mdspan extents product overflows`) and `size()` stays
0. The store is `if (__fits) _M_size = __prod`. A wrapped count is not
stored.

Rank 2 still checks its product the same way (`__e1 == 0 || __e0 <=
SIZE_MAX / __e1`) and still assigns `_M_size = __e0 * __e1` only when that
check passes, so an overflowing rank-2 product stays 0. Rank 3, rank 4,
rank 5, and rank 6 still store `_M_size` only from `__prod` after their fit
checks, and the header still has no `_M_size = __e0 * __e1 * __e2`, no
`_M_size = __e0 * __e1 * __e2 * __e3`, no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4`, no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5`, and no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5 * __e6`.

`data()` is the pointer that was passed in. A deduction guide maps
`(T*, size_t, size_t, size_t, size_t, size_t, size_t, size_t)` to
`mdspan<T, 7>`.

## What falls through

The primary template stays a declaration:

```cpp
template <typename _Tp, std::size_t _Rank = 1>
class mdspan;
```

Rank other than 1, 2, 3, 4, 5, 6, and 7 is that incomplete type, so the
unit fails to compile and the pir stage lowers it again with libstdc++
(`extra.cxx_models`). The standard template argument list (`Extents`,
`LayoutPolicy`, `AccessorPolicy`) is not `mdspan<T>` / `mdspan<T, 2>` /
`mdspan<T, 3>` / `mdspan<T, 4>` / `mdspan<T, 5>` / `mdspan<T, 6>` /
`mdspan<T, 7>`.

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
lists `mdspan` in the header-name array; that array was not edited. The
`iostream/prism_ostream.hpp` and `deque/deque` skips in that test were not
removed. `src/prism/pir/stage.cpp` was not edited.

`testdata/conformance/prism/cxx/cxx_mdspan_true.cpp` and
`cxx_mdspan_false.cpp`, and `testdata/conformance/libc-models/mdspan_contracts.cpp`,
were not edited. Rank 6 left those rank-1 tasks in place. Every existing
trace harness in `tests/cxx_models/mdspan_trace.cpp` is still there.

## Tests

`tests/cpp/test_cxx_mdspan.cpp` still reads the header. It now requires seven
defaulted destructors, seven quoted `PTR-NULL-DEREF` checks, and seven
`operator()` bodies that call `__prism_check` (`MEM-OOB-READ`) before they
index `_M_ptr`. Rank 2's overflow lines are still present. Rank 3, rank 4,
rank 5, rank 6, and rank 7 each store `_M_size` only from `__prod` after the
fit check, and the header has no `_M_size = __e0 * __e1 * __e2`, no
`_M_size = __e0 * __e1 * __e2 * __e3`, no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4`, no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5`, and no
`_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5 * __e6`. The primary
`class mdspan;` remains. There is an `mdspan<_Tp, 7>` specialization and no
`mdspan<_Tp, 8>`.

`tests/cxx_models/mdspan_trace.cpp` adds the rank-7 cases. The in-range run
still prints the rank-1, rank-2, rank-3, rank-4, rank-5, and rank-6 lines,
then:

```
r7 40320 2 3 4 5 6 7 8 1 40320
z7 0
e7 0 1
```

`r7` is extents `(2, 3, 4, 5, 6, 7, 8)`, `size()` 40320,
`(0, 0, 0, 0, 0, 0, 0)` equal to 1, and `(1, 2, 3, 4, 5, 6, 7)` equal to
40320 (the last element). `z7` is extents `(SIZE_MAX, 2, 2, 2, 2, 2, 0)`:
the product is 0, so `size()` is 0 and the overflow check does not fire.
`e7` is a null pointer with extents `(0, 0, 0, 0, 0, 0, 0)`.

`operator()(2, 0, 0, 0, 0, 0, 0)`, `operator()(0, 3, 0, 0, 0, 0, 0)`,
`operator()(0, 0, 4, 0, 0, 0, 0)`, `operator()(0, 0, 0, 5, 0, 0, 0)`,
`operator()(0, 0, 0, 0, 6, 0, 0)`, `operator()(0, 0, 0, 0, 0, 7, 0)`, and
`operator()(0, 0, 0, 0, 0, 0, 8)` on extents `(2, 3, 4, 5, 6, 7, 8)` abort
with `MEM-OOB-READ`. A null pointer with extents `(0, 0, 0, 0, 0, 0, 1)`
aborts with `PTR-NULL-DEREF`. Extents `(SIZE_MAX, 2, 2, 2, 2, 2, 2)` abort
with `std::mdspan extents product overflows`, and so do rank 6 with extents
`(SIZE_MAX, 2, 2, 2, 2, 2)`, rank 5 with extents `(SIZE_MAX, 2, 2, 2, 2)`,
rank 4 with extents `(SIZE_MAX, 2, 2, 2)`, rank 3 with extents
`(SIZE_MAX, 2, 2)`, and rank 2 with extents `(SIZE_MAX, 2)`.
`mdspan<int, 8>` is an implicit instantiation of the undefined primary
template. `mdspan<int, E, L, A>` does not match the `size_t` rank parameter.

Checked with Ubuntu clang++ 18.1.3 (ASan/UBSan), `-std=c++23 -O0 -g
-fsanitize=address,undefined -fno-sanitize-recover=all`. The success path
printed the lines above and exited 0. The failing modes aborted (`SIGABRT`),
including `operator()(0, 0, 0, 0, 0, 0, 8)` built at `-O1`. Those are the
check, not an ASan heap error. A host probe of extents
`(SIZE_MAX/2, 2, 1, 1, 1, 1, 1)` stored `size()` `18446744073709551614`.
Extents `(SIZE_MAX/3, 3, 1, 1, 1, 1, 1)` stored `size()`
`18446744073709551615`. Extents `(SIZE_MAX/3 + 1, 3, 1, 1, 1, 1, 1)`,
`(2, 2, 2, 2, 2, 2, SIZE_MAX)`, and `(SIZE_MAX, 2, 2, 2, 2, 2, 2)` fired the
product-overflow check and left `size()` 0. Extents
`(SIZE_MAX, 0, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX)` printed
`size()` 0 and did not fire that check (the zero extent skips the
divisions, including a division by that zero). Extents
`(2, 2, 2, 2, 0, SIZE_MAX, SIZE_MAX)` printed `size()` 0 and did not fire
that check (the zero is a later divisor, and that path does not divide by
it). A null pointer with extents `(1, 0, 0, 0, 0, 0, 0)` and with extents
`(0, 1, 0, 0, 0, 0, 0)` fired `PTR-NULL-DEREF`. A rank-7 view of a
one-element heap array read 42; after the view died the element was still
42 under ASan, and `delete[]` freed it. With a stub that records the check
and returns, `operator()(2, 0, 0, 0, 0, 0, 0)` on extents
`(2, 3, 4, 5, 6, 7, 8)` still read the next element of a 40321-element
array (40321) after `MEM-OOB-READ`, and `extent(7)` returned 8 after
`FUNC-CONTRACT`. A deduction guide with seven `size_t` arguments compiled
as `mdspan<int, 7>` and read the element. `prism_tests` was not run. cmake
was not run.

## Still in front of the model

`src/prism/bmc_unenc.inc` still returns "is not an mdspan model" from the
source text (`std::mdspan` or `mdspan<`) before the header is inlined.
`src/prism/bmc_encoder.inc` still throws `mdspan unencoded` on the same
spellings. `src/prism/bmc.cpp` still returns that sentence when the engine
text contains `mdspan unencoded` or `std::mdspan`. Those files were not
edited. Until that gate is gone, a task that spells `std::mdspan` or
`mdspan<` stays on that path. `docs/PIR.md` was not edited.
