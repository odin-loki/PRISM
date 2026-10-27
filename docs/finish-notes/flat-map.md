# flat_map slice

Branch `finish/cxx-flat-map`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.
`std::flat_map` is a header file, `src/prism/pir/models/cxx/flat_map`. A
directory of that name would make `#include <flat_map>` fail with EISDIR
and every such unit would fall back to libstdc++. The include guard defines
`_GLIBCXX_FLAT_MAP`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml files record the contract each harness asks
for. `expected` values are YAML booleans. Nothing here is a proof.

## What the model covers

Two template parameters, `Key` and `T`. Each pair is one heap node from
`::operator new` / `::operator delete`. The header does not include
`<map>`, `<vector>`, `<memory>`, or `<initializer_list>`. `size()` is the
number of nodes.

`operator[]` on a missing key inserts a node (the mapped object is
value-initialised in that node) and returns a reference to it. A key that
is already present returns the stored object and does not allocate.

`at(key)` returns that stored object when the key is present. When the key
is absent it throws `std::out_of_range` with
`__throw_out_of_range_fmt` (`flat_map::at: key is not present`), the same
helper `<bitset>` uses. It does not return a default `T`. An empty
container has no node, so `at()` is that miss, not a fabricated element.

`erase` of a present key destroys that node and frees it (`::operator
delete`). A reference saved from `at()` or `operator[]` into that node is
a use-after-free. `erase` of a missing key returns 0 and frees nothing.

Copy and move copy or steal those nodes. The destructor frees every node
that is still linked.

## What falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and pir falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- insert ranges, `insert`, `emplace`, `try_emplace`
- `erase(iterator)`, iterators, `find`, `contains`, `count`, `clear`, `swap`
- `Compare`, `KeyContainer`, and `MappedContainer` (a third template
  argument does not match this template)

## Wiring

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `PRISM_PIR_MODELS`
lists `src/prism/pir/models/cxx/flat_map`. `tests/cpp/test_qa_conformance.cpp`
names `flat_map` in the header array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
`_prism_test_sources` lists `tests/cpp/test_cxx_flat_map.cpp`.

`src/prism/bmc.cpp` still returns `C++ flat_map unencoded`
(`is not a flat_map model`) when the source text contains `flat_map`, and
`src/prism/bmc_unenc.inc` still does the same for `std::flat_map` /
`flat_map<`. Those exits run before the header is inlined. Neither file
was edited. `docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_flat_map.cpp` reads the header. It does not compile it
and it does not run pir. A missing `at()` is `__throw_out_of_range_fmt` (a
`__prism_check` would also count) and the function does not return a
default mapped value. `erase` is described as freeing the node, and the
`::operator delete` is only on the found path.

`tests/cxx_models/flat_map_trace.cpp` includes `<flat_map>`. `show` inserts
one key and reads it with `at()`. `miss` calls `at()` on an empty
`flat_map` and prints `threw` or, if a default were returned, `fake=`.
`uaf` uses a reference after `erase`.

clang++ 18, `-std=c++23 -O1 -fsanitize=address,undefined`, model directory
as `-isystem`. This machine's libstdc++ has no `<flat_map>`, so the trace
was not compiled against the library header. Against the model, `show`
printed size 0, then `at 9`, `replaced 4`, two keys, and
`erase present=1 missing=0`. `miss` printed `threw`. `uaf` aborted with
`heap-use-after-free` in `flat_map::erase` (`operator delete`).

`testdata/conformance/libc-models/flat_map_contracts.cpp` includes
`<flat_map>`. `flat_map_empty_at_false` is `at()` on an empty container
(`CXX-THROW-NOEXCEPT`). `flat_map_erase_uaf_false` is the freed node
(`MEM-UAF`). The true functions insert a key and read it, and erase a
missing key.

`testdata/conformance/prism/cxx/cxx_flat_map_true.cpp` inserts one key and
reads it with `at()` (`no-uncaught`, `expected: true`).
`cxx_flat_map_false.cpp` calls `at()` on an empty `flat_map`
(`expected: false`, witness `[0]`). The yml files use the same fields as
`cxx_array_true.yml`. cmake, ninja, `prism_tests`, and pir were not run.
