# mdspan slice

Branch `finish/cxx-mdspan`, base `b58ed8da4042896363fbbe540d29692207d1a1ea`.
`std::mdspan` is a header file, `src/prism/pir/models/cxx/mdspan`. A directory
of that name would make `#include <mdspan>` fail with EISDIR and every such
unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. `expected:` is an unquoted YAML boolean (`false`, not the string
`"false"`). This header is not a proof.

## What the model covers

A non-owning view of a contiguous `T*`. Nothing is allocated. The destructor
is defaulted and does not free the pointer. There is no element object in
the view.

`mdspan<T>` is rank 1: construct from a pointer and one extent.
`mdspan<T, 2>` is rank 2: construct from a pointer and two extents, and
`operator()(i, j)` checks both. The address is `i * extent(1) + j` in the
caller's array (contiguous, row-major). That is not a `layout_right` object.

A null data pointer with a non-zero extent is `__prism_check` with class
`PTR-NULL-DEREF` (`std::mdspan null data pointer with a non-zero extent`).
An empty extent may be a null pointer. No stand-in element is created.

`operator()` with an index outside those extents is `__prism_check` with
class `MEM-OOB-READ`. The failure is the precondition. It is not a default
`T` and not a padding element. An in-range call returns the caller's
element. `size()` and `extent(0)` are the stored extent. On rank 2,
`extent(0)` is the first extent, `extent(1)` the second, and `size()` is
their product when that product fits in `size_t`. A product that does not
fit is a check, and `size()` stays 0 on that path rather than a wrapped
count.

`data()` is the pointer that was passed in.

## What is not modelled

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers the whole translation unit with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `submdspan` and `mdarray`
- `layout_left`, `layout_right`, `layout_stride`
- `extents`, `dextents`, `default_accessor`
- `operator[]` (the standard spelling)
- iterators
- any rank other than 1 or 2, and the standard template argument list
  (`Extents`, `LayoutPolicy`, `AccessorPolicy`)

The header does not include `<span>`, `<vector>`, or `<memory>`.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not an
mdspan model" from the source text (`std::mdspan` or `mdspan<`) before the
header is inlined. `src/prism/bmc_encoder.inc` still throws `mdspan unencoded`
on the same spellings. This slice does not change those exits, and it does
not edit `stage.cpp`. `write_cxx_models` already copies every embedded
`cxx/` header and the stage already passes that directory as `-isystem`.
Until the early exits go, a task that spells `std::mdspan` or `mdspan<`
stays on that path. `docs/PIR.md` still lists `std::mdspan` with the
unmodelled library surface. That page is not edited here.

## Tests

`tests/cpp/test_cxx_mdspan.cpp` reads the header: both `operator()` bodies
call `__prism_check` (`MEM-OOB-READ`) before they index `_M_ptr`, both
destructors are defaulted, and there is no `delete[]` or `operator new`.
It then compiles `tests/cxx_models/mdspan_trace.cpp` with `-isystem` on the
model directory (`-std=c++23 -O0`, ASan/UBSan). The host stub aborts in
`__prism_check`. The in-range call writes through the view into the
caller's array (`11` at index 0). After the view dies, the stack element
and the heap element are still there, and `delete[]` of that heap block
runs once. `operator()(4)` on an extent of 4 aborts with `MEM-OOB-READ`.
Rank-2 `operator()(1, 3)` on extents `(2, 3)` does the same. A null
pointer with extent 1 aborts with `PTR-NULL-DEREF`. Without a usable
clang++ the run is `NOTRUN`; the header text is still checked.

Checked with Ubuntu clang++ 18.1.3 (ASan/UBSan). The trace printed those
lines and exited 0. `oob`, `oob2`, and a null pointer with extent 1 aborted
in the check (`SIGABRT`), including `operator()(4)` built at `-O1`. Not an
ASan heap error. `prism_tests` was not run.

`testdata/conformance/prism/cxx/cxx_mdspan_true.cpp` reads an in-range
element (`no-oob`, true). `cxx_mdspan_false.cpp` calls `operator()` at
`size()` (`false`, witness `[4]`).
`testdata/conformance/libc-models/mdspan_contracts.cpp` is the libc-contract
harness the conformance test requires for a public model header: in-range
read, empty null, and a view that does not keep the array; `operator()`
past the extent is `MEM-OOB-READ`; a null pointer with a non-zero extent
is `PTR-NULL-DEREF`.

`CMakeLists.txt` lists `src/prism/pir/models/cxx/mdspan` in
`PRISM_PIR_MODELS` and `tests/cpp/test_cxx_mdspan.cpp` in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` lists `mdspan`
with the other public headers. The iostream and deque skips in that test
are unchanged. No engine configure, no `prism_tests`, no pir run.
