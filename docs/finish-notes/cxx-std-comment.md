# cxx-std-comment slice

Branch `finish/cxx-std-comment`, base `1f3d4de85465663e28412dfcaeabacbc1dc360fe`.

The leading comment in `testdata/conformance/libc-models/cxx_std_contracts.cpp`
said `span`, `optional`, and `unique_ptr` are not replaced by PRISM models,
and that libstdc++'s headers are their model. That is no longer true.

The operational models, when `src/prism/pir/models/cxx` is first on the
include path, are:

- `src/prism/pir/models/cxx/array`
- `src/prism/pir/models/cxx/span`
- `src/prism/pir/models/cxx/optional`
- `src/prism/pir/models/cxx/memory` (`unique_ptr`)

`unique_ptr` is the memory model. `allocator` is not in that header.
`optional::value_or` returns the stored `T` or the argument. `and_then`
and `shared_ptr` are not modelled.

No function, include, or assertion in the harness changed. The model
headers were not edited. `docs/PIR.md` still says these four are not
replaced (the "Not modelled, and why" paragraph, and the sentence on
`cxx_std_contracts.cpp`). That page was not edited here.

cmake, ninja, `prism_tests`, and pir were not run.
