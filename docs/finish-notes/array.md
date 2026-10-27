# array slice

Branch `finish/cxx-array`, base `66d1b3373ddf27f0a3187c35d2d19bd533a599cc`.
`std::array` is a header file, `src/prism/pir/models/cxx/array`. A directory
of that name would make `#include <array>` fail with EISDIR and every such
unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. This header is not a proof.

## What the model covers

An aggregate. For `N > 0` the only data member is `T _M_elems[N]`, the same
shape as libstdc++'s member, so `std::array<T, N>{e0, e1, ...}` initialises
the elements. Nothing is allocated. `data()` is that member; `fill` and
`swap` do not move the pointer. `fill` assigns in index order. `swap`
exchanges elements with `std::swap` (argument-dependent lookup, as
libstdc++'s `iter_swap` does). `max_size()` is `N`. The deduction guide and
the non-member `swap` are defined.

`operator[]` is the vector model's `_GLIBCXX_ASSERTIONS` check, same
condition text: `__glibcxx_assert(__n < this->size())`. That string contains
`size()`, so PIR classifies it `MEM-OOB-READ`. `at()` does not assert. It
throws `std::out_of_range` with libstdc++'s `array::at` message
(`__n >= _Nm`). `front` / `back` are `__glibcxx_assert(!this->empty())`
(`FUNC-CONTRACT`).

`N == 0` is a specialization with no element and no stand-in `T`. `empty()`
is true, `size()` and `max_size()` are 0, `fill` and `swap` do nothing, and
`data()` is a null pointer (libstdc++ converts its empty `_Type` to null;
this model does not store that `_Type`). `front` and `back` on `N == 0` are
the `!empty()` precondition, then not a value. `operator[]` is the same
`__n < this->size()` precondition. `at()` throws `out_of_range` for every
index, because every index is `>= 0`.

## What is not modelled

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers the whole translation unit with libstdc++
(`extra.cxx_models`). They are not stubbed:

- iterators and reverse iterators (`begin` / `end` / `rbegin` / `rend` and
  the const forms), so range-for does not compile here
- `operator==` and `operator<=>`
- the tuple interface: `std::get`, `tuple_size`, `tuple_element`
  (structured bindings). `get` is omitted with the rest of that interface
- `std::to_array`

`front` / `back` on a zero-length array are the precondition above. There is
no fake element to read.

`src/prism/bmc.cpp` still returns "not an array model" from the source text
before any header is inlined. This slice does not change that, and it does
not edit `stage.cpp`. `docs/PIR.md` still describes `std::array` as
libstdc++'s own header under `_GLIBCXX_ASSERTIONS`. That page is not edited
here.

## Tests

`tests/cpp/test_cxx_list.cpp` compiles `list_trace.cpp` as its own program.
It does not `#include` the trace, so there is no `tests/cpp/test_cxx_array.cpp`.

Checked with clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -O1`. `tests/cxx_models/array_trace.cpp` compiled
twice (`-isystem` the model directory, and without it). Stdout matched,
including construction and assignment order. Both binaries, run with `oob`,
abort in `operator[]` on `__n < this->size()`. `array<int, 0>::front()`
aborts on `!this->empty()` both ways. The solver was not run.

`testdata/conformance/prism/cxx/cxx_array_true.cpp` is an in-bounds
`operator[]` and `data()` (`no-oob`, true). `cxx_array_false.cpp` is
`operator[]` at `size()` (`false`, witness `[4]`).
`testdata/conformance/libc-models/array_contracts.cpp` is the libc-contract
harness: access, fill, swap, and `N == 0` true; `operator[]` past the end
`MEM-OOB-READ`; `at` never throwing, `front` / `back` on `N == 0`, and a
wrong `front()` assert `FUNC-CONTRACT`.

`CMakeLists.txt` lists `src/prism/pir/models/cxx/array` next to `list`.
`tests/cpp/test_qa_conformance.cpp` lists `array` with the other public
headers. The iostream and deque skips in that test are unchanged.
