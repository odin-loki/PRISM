# numbers slice

Branch `finish/cxx-numbers`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.

`std::numbers` is one extensionless file, `src/prism/pir/models/cxx/numbers`.
A directory of that name would make `#include <numbers>` fail. The include
guard is `_GLIBCXX_NUMBERS`. `#pragma GCC system_header`. Namespace
`std::numbers`.

The solver was not run. The yml records the contract the task asks for.
`expected` is the YAML boolean `true`. Nothing here is a proof.

## What the model is

A partial `<numbers>`. Thirteen variable templates, each initialized from
a long double decimal literal (the usual expansion): `e`, `log2e`,
`log10e`, `pi`, `inv_pi`, `inv_sqrtpi`, `ln2`, `ln10`, `sqrt2`, `sqrt3`,
`inv_sqrt3`, `egamma`, `phi`. `pi<long double>` is greater than 3 and less
than 4. `e<long double>` is greater than 2 and less than 3. The literals
are not exact. A comment in the header says a comparison against these
literals is not a proof of a numeric identity.

Special functions are omitted. The header does not include `<cmath>` or
any other standard header. A unit that names a non-template
`std::numbers::pi` (the standard `double` constant) does not match this
file, so it fails to compile against the model and `lower_to_ir` lowers
that translation unit again with libstdc++ (`extra.cxx_models`).

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained one line:

```text
src/prism/pir/models/cxx/numbers
```

so the existing embed writes `cxx/numbers` and the existing `-isystem`
directory is enough for `#include <numbers>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_numbers.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`numbers` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/numbers_contracts.cpp` is the
`#include <numbers>` that case searches for. It is not a scored task.
There is no false twin.

## Tests

`tests/cpp/test_cxx_numbers.cpp` reads the header as text. It checks that
a comment says the literals are not a proof, and that pi's digits start
with `3.14`. It does not compile the header.

`tests/cxx_models/numbers_trace.cpp` includes `<numbers>` and checks
`pi > 3`, `pi < 4`, `e > 2`, and `e < 3` on the `long double`
instantiations.

`testdata/conformance/prism/cxx/cxx_numbers_true.cpp` is the same check
(`no-oob`, `expected: true`). There is no false task. The yml shape matches
`cxx_array_true.yml`. cmake, `prism_tests`, and pir were not run.
