# mdspan ranks 8 to 11

Branch `port/mdspan-ranks`, base `03cf1640eeeef249ce33077ac98004ca94425708`
(main). `std::mdspan` stays one extensionless file,
`src/prism/pir/models/cxx/mdspan`. A directory of that name would make
`#include <mdspan>` fail with EISDIR. The include guard is still
`_GLIBCXX_MDSPAN`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. This header is not a proof.

## Where this comes from

`finish/cxx-mdspan-rank8` .. `rank11` (`a28be6bc9`, `e487eaca8`, `fd6c642ff`,
`cfe50b3df` on `local-main-2026-10-29`) added ranks 8 to 11 as
`mdspan<T, 8>` .. `mdspan<T, 11>` specializations. Main has since moved the
rank classes to `__prism_mdspan<T, N>` behind the standard spelling
`mdspan<T, extents<I, E...>, layout_right, default_accessor<T>>`, so those
commits do not apply. This change keeps their intent (null data pointer,
overflowing product, an index past any extent) and their test ideas, written
the way main writes ranks 5 to 7.

## What changed

`__prism_mdspan<T, 8>`, `<T, 9>`, `<T, 10>` and `<T, 11>` are new. Each is
the rank-7 class with more extents: construct from a non-owning `T*` and N
extents, defaulted destructor, nothing allocated, destroying the view does
not free the pointer. `std::mdspan<T, dextents<I, N>>` and
`std::mdspan<T, extents<I, E...>>` with N entries reach them through the
existing `: public __prism_mdspan<_Tp, sizeof...(_Exts)>`. Four deduction
guides map `T*` and 8, 9, 10 or 11 `size_t` extents to
`mdspan<T, dextents<size_t, N>>`.

- A null data pointer with any non-zero extent is `__prism_check` with class
  `PTR-NULL-DEREF`. All N extents zero may be a null pointer.
- `operator()(i, j, k, l, m, n, p, q[, r[, s[, t]]])`, and `operator[]` with
  N indices (it forwards to `operator()`), check every index against its
  extent first: `MEM-OOB-READ`, `std::mdspan::operator() index is outside
  the extents`. The address is formed after the check, row-major: index a
  times the product of extents a+1 .. N-1, summed.
- `size()` is the product of the N extents when it fits in `size_t`. A zero
  extent makes the product 0 and skips the divisions. Otherwise each step is
  multiplied only after `prefix <= SIZE_MAX / next`. A product that does not
  fit is `MEM-OOB-READ` (`std::mdspan extents product overflows`) and
  `size()` stays 0; the store is `if (__fits) _M_size = __prod`.
- `extent(r)` returns the r-th stored extent. `r >= N` is `FUNC-CONTRACT`
  (`std::mdspan::extent index is outside the rank`), then the last stored
  extent comes back.

Rank 12 is still the incomplete primary `__prism_mdspan<T, 12>`, and there is
no guide with twelve extents. Rank 0 is also not declared.

The header comment was rewritten. It still described the old `mdspan<T, N>`
spelling and listed `extents`, `dextents`, `layout_right`,
`default_accessor` and `operator[]` as not declared, which stopped being true
in `e4bc5a5a6`. It now says what is declared, that `operator()` and `data()`
are the model's own spellings (the standard has `operator[]` and
`data_handle()`), that the null-pointer check is the model's and not the
standard's, and that a static extent is not compared with the constructor
argument.

## Tests

`tests/cpp/test_cxx_mdspan.cpp` reads the header: eleven rank classes, no
`__prism_mdspan<_Tp, 12>` or `<_Tp, 0>`, eleven defaulted destructors,
eleven `PTR-NULL-DEREF` and eleven `FUNC-CONTRACT` checks, nine
`if (__fits) _M_size = __prod;`, ten overflow checks, eleven `operator()`
bodies that call `__prism_check` before `return _M_ptr[`, the zero tests and
index conditions of ranks 8 to 11, the four new guides and none for rank 12.

`tests/cxx_models/mdspan_trace.cpp` adds, for each N in 8..11: `rankN`,
`zeroN`, `emptyN`, `deduceN`, `oobN(which)`, `nullN`, `ovfN`. Extents
alternate 2 and 3, so the arrays are 1296, 2592, 7776 and 15552 ints and
every stride is still a product of several extents. The success run prints

```
r8 8 1296 2 3 2 3 2 3 2 3 1 1296 778
z8 0
e8 0 1
d8 1 778 1
r9 9 2592 2 3 2 3 2 3 2 3 2 1 2592 1556
z9 0
e9 0 1
d9 1 1556 1
r10 10 7776 2 3 2 3 2 3 2 3 2 3 1 7776 4666
z10 0
e10 0 1
d10 1 4666 1
r11 11 15552 2 3 2 3 2 3 2 3 2 3 2 1 15552 9332
z11 0
e11 0 1
d11 1 9332 1
```

`rN` is rank(), size(), the N extents, the first element, the last element,
and the element at `(1, 0, 1, 0, ...)` (offset 777, 1555, 4665, 9331).
`zN` is extents `(SIZE_MAX, 2, ..., 2, 0)`: the product is 0 and the overflow
check does not fire. `eN` is a null pointer with all-zero extents. `dN` is
`std::mdspan m(h, size_t(2), ...)`: the deduced type is
`mdspan<int, dextents<size_t, N>>`, `m[1, 0, 1, 0, ...]` reads the same
element as `rN`, and `&m[last] == &m(last)`.

The doctest runs each failing mode: one past each of the N extents through
`operator()`, and the last one through `operator[]` (`oob8` .. `oob8q`,
`oob8b`, through `oob11` .. `oob11t`, `oob11b`) abort with `MEM-OOB-READ`;
`nullN` (extents `(0, ..., 0, 1)`) aborts with `PTR-NULL-DEREF`; `ovfN`
(extents `(SIZE_MAX, 2, ..., 2)`) aborts with `extents product overflows`.

Rank 12 is checked by compiling: `dextents<size_t, 12>` with twelve extents,
`extents<size_t, 1 x12>`, and CTAD with twelve `size_t` arguments each fail
to compile against the model; the same three units with rank 11 compile. The
old negative test (`std::mdspan<int, 8>`) is gone: it failed because 8 is not
a type, not because of the rank.

## Model against a real <mdspan>

libstdc++ 14 (`/usr/include/c++/14`, the newest libstdc++ on the box) has no
`<mdspan>` header; it arrived in GCC 15. A real-library build under
`-D_GLIBCXX_ASSERTIONS` is therefore not possible here (`fatal error:
'mdspan' file not found` with clang 23, clang++-18 and g++-14). The real
library used is libc++ from the clang 23 install (`-stdlib=libc++
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG`). clang++-18 cannot
parse libc++ 23's `<mdspan>` (`#pragma clang attribute` visibility error),
so clang++-18 was only run against the model.

The comparison program uses only standard spelling (`dextents`,
`operator[]`, `extent`, `size`, `rank`, CTAD) for ranks 1 to 11, all under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all`. Model with clang 23, model with clang++-18, and
libc++ with clang 23 printed byte-identical output (22 lines: every element
walked in row-major order through `operator[]`, first/last/mixed element,
size, extents, CTAD type, a zero-first extents list, an all-zero null view).
Failing modes, ranks 1 to 11:

| Mode | Model | libc++ (debug hardening) |
|---|---|---|
| one past the last extent / the first extent | `MEM-OOB-READ` abort | `__is_multidimensional_index_in` abort |
| `(SIZE_MAX, 2, ..., 2)` | `extents product overflows` abort | `__required_span_size_is_representable` abort |
| `extent(N)` | `FUNC-CONTRACT` abort | `extents access: index must be less than rank` abort |
| null pointer, one extent 1 | `PTR-NULL-DEREF` abort | runs, size() 1 |
| `(SIZE_MAX, 2, ..., 2, 0)`, rank 3+ | size() 0, no check | `__required_span_size_is_representable` abort |

The null pointer row is the model's own check, not the standard's. In the
last row libc++ multiplies left to right and stops at the first overflowing
partial product, before it reaches the 0. The standard's precondition is on
the size of the index space, which is 0, so the model follows the standard
there. Ranks 3 to 7 already behaved this way on main.

## Not changed

`testdata/conformance/` tasks were not added or edited; the rank-1 tasks are
unchanged. `CMakeLists.txt`, `src/prism/pir/stage.cpp`,
`tests/cpp/test_qa_conformance.cpp` and `docs/PIR.md` were not edited.

What is still open: the model accepts `operator()` and `data()`, which the
standard mdspan does not declare, and it does not compare a static extent
with the constructor argument (`mdspan<int, extents<size_t, 2>> m(a, 5)`
stores 5). Both predate ranks 8 to 11.
