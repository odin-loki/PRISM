# tuple slice

Branch `finish/cxx-tuple`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`std::tuple` is one extensionless file, `src/prism/pir/models/cxx/tuple`.
A directory of that name would make `#include <tuple>` fail with EISDIR.
The include guard is `_GLIBCXX_TUPLE`. `#pragma GCC system_header`. Namespace
`std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What the model is

Arity 0, 1, and 2. Each element is a data member of the object. `std::get<I>`
returns that member. `tuple_size` and `tuple_element` are specialized for
those arities. `tuple<>` has `tuple_size` 0 and no `get<0>`.

An index that is not a valid compile-time index instantiates
`_Prism_tuple_get` with a `static_assert` and no `get` member, so the call is
ill-formed. There is no function that returns a value-initialized object for
an element the tuple does not have.

`bits/utility.h` supplies the `tuple_size` / `tuple_element` primary
templates (and the cv specializations). The header does not include
`<memory>`, `<variant>`, or a container.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- `tuple_cat`, `apply`, `forward_as_tuple`, `tie`
- comparison
- any arity above 2 (the primary class template is not defined)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/tuple`

so the existing embed writes `cxx/tuple` and the existing `-isystem`
directory is enough for `#include <tuple>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_tuple.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`tuple` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/tuple_contracts.cpp` is the
`#include <tuple>` that case searches for. It is not a scored task: a false
twin would be an out-of-range `get`, and that does not compile.

## Tests

`tests/cxx_models/tuple_trace.cpp` includes `<tuple>` and is compiled twice
(model directory as `-isystem`, and without it). clang++ 18.1.3 against
libstdc++ 14, `-std=c++23`, printed the same lines both ways, including
under `-fsanitize=address,undefined`:

```
get0=42
set0=5
cget0=7
rget0=42
get0b=3
get1=9
bind=3,9
empty=0
size1=1
size2=2
zget=0
```

`get<0>` of `tuple<int>(42)` is 42, and assigning through that reference
stores 5. `tuple<>` has `tuple_size` 0. `get<1>` of `tuple<int>` fails
`static_assert`. `get<0>` of `tuple<>` has no matching function.
`tests/cpp/test_cxx_tuple.cpp` also reads the header text and rejects a
`return` of a value-initialized temporary (`return {}`, `return T{}`,
`return T()`). cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_tuple_true.cpp` is memsafety true: it
reads `get<0>` of a `tuple<int>` that holds the argument. There is no false
task. A runtime out-of-range `get` is not a defect this header can report;
that call is ill-formed, which is the fallback.

## Still in front of the model

`src/prism/bmc.cpp` (`low.find("tuple")`) and `src/prism/bmc_unenc.inc`
(`tuple\s*<`) still return "not a tuple model" from the source text, before
the header is inlined. Those files were not edited. Until that gate is gone,
`cxx_tuple_true` does not reach this header. `docs/PIR.md` was not edited.
