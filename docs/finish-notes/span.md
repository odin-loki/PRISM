# span slice

Branch `finish/cxx-span`. `std::span` is a header file,
`src/prism/pir/models/cxx/span`, the same shape as `vector`. A directory of
that name would make `#include <span>` fail and every such unit would fall
back to libstdc++.

The solver was not run. The yml files record the contract each task asks
for. Neither task is marked `PROVED`.

## What the model covers

A non-owning view: one pointer and one length. Constructors are the empty
view, a pointer plus a length, and a C array (the array is not copied).
Copy and copy-assignment copy the view. The destructor is defaulted and
does not release the viewed memory. There is no allocation.

`data()`, `size()`, and `empty()` read those two fields. `operator[]`
returns the element at that index only after `__glibcxx_assert(__n <
this->size())` (the same call `<vector>` uses; the condition text contains
`size()`, so PIR classifies a failure as `MEM-OOB-READ`). It does not
return a stand-in `T`. `front()` and `back()` on an empty span use
`__glibcxx_assert(!this->empty())` (`FUNC-CONTRACT`, the same precondition
on both) and do not return a fabricated `T`. `subspan(offset, count)` and
`subspan(offset)` (the tail) require the slice to sit inside `size()`; a
slice that would run past the end is that precondition, not a span of
invented elements. `first(count)` is the same check.

`begin()` / `end()` are a small random-access iterator over the viewed
pointer. It is a class type because libstdc++ `<format>` calls
`iterator::operator->`. `<format>` is included from `<memory>` and
`<ostream>`, so those members have to compile or every such translation
unit fails against the models and falls back to libstdc++, including a
unit whose own code only uses `<vector>`. An lvalue with `data()` and
`size()` (a `basic_string` in that `<format>` sink) becomes a view of
those elements. A temporary does not bind.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records
the fallback). They are not stubbed:

- `std::span<T, N>` and `std::dynamic_extent` (the template has one
  parameter; a static extent is not modelled)
- deduction guides
- `last`, `size_bytes`, and the template `first` / `subspan`
- a constructor from an iterator pair, or from an rvalue range
- `as_bytes` / `as_writable_bytes`
- a converting constructor from `span<U>`

`__cpp_lib_span` is not defined. The header does not include `<vector>`,
`<array>`, `<deque>`, or any other container. `src/prism/pir/stage.cpp` is
unchanged: it already puts the `cxx/` model directory on the include path.

## Wiring

`CMakeLists.txt` `PRISM_PIR_MODELS` lists `src/prism/pir/models/cxx/span`
next to the other `cxx` models. `_prism_test_sources` lists
`tests/cpp/test_cxx_span.cpp`. `tests/cpp/test_qa_conformance.cpp` names
`"span"` in the public-header array. `tests/cxx_models/span_trace.cpp` is
compiled by that doctest, not by CMake.

## Tests

`span_trace.cpp` includes `<span>` and prints `size`, `empty`, and an
in-range index (`at2` is `30` from `{10, 20, 30, 40}`). It also prints the
int after the span that viewed it is destroyed (`kept=5`).
`tests/cpp/test_cxx_span.cpp` reads the model header without running pir:
the destructor is defaulted, the file does not allocate or release, and
`front()` calls `__glibcxx_assert(!this->empty())`. With clang++ it
compiles the trace against the model (`-isystem` the `cxx` model
directory) and against libstdc++. Without `<bits/c++config.h>` that half
is `NOTRUN`, same as the other C++ model drivers.

`testdata/conformance/prism/cxx/cxx_span_true.cpp` indexes a 3-element
view only when the index is in range (`expected: true`, property
`no-oob`). `cxx_span_false.cpp` can take index 3 (`expected: false`,
witness `[3]`). The yml schema is the usual `expected` boolean. They are
not marked `PROVED`.
