# system_error slice

Branch `finish/cxx-system-error`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<system_error>` is a file, `src/prism/pir/models/cxx/system_error`, the same
shape as `vector` and `memory`. A directory of that name would make
`#include <system_error>` fail with EISDIR. The header is partial on purpose.
The include guard is `_GLIBCXX_SYSTEM_ERROR`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml records the contract the harness asks for. `expected` is a YAML
boolean `true`. There is no false task.

## What the model covers

`std::error_code` stores an int value and an int category id.

- The default constructor stores value 0 and category id 0. `value()` is 0.
  `operator bool` is false.
- `generic_category()` returns an empty tag, not an `error_category` object.
  `error_code(n, generic_category())` stores `n` and category id 1.
  `value()` returns that stored int. A nonzero `n` stays `n`. It does not
  become 0.
- `operator bool` is true only when `value()` is not 0. `error_code(5, ...)`
  is true. A default `error_code` is false. `error_code(0, generic_category())`
  is false because its value is 0.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). A partial
header is intentional. They are not stubbed:

- `message()`. A fixed string would hide a nonzero value. Omitting the
  member makes the call fail to compile, which is the fallback. The header
  text does not contain the word Success.
- `error_condition`
- the `system_error` exception type (`std::runtime_error`, and the string
  and stdexcept headers, are not included)
- `std::generic_category` as a real category object (the tag above is only
  accepted by this `error_code` constructor)
- `system_category`, `errc`, `make_error_code`, `error_category`

This header does not `#include <string>`, `<string_view>`, or `<stdexcept>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained
`src/prism/pir/models/cxx/system_error`, so the header is embedded as
`cxx/system_error`. `_prism_test_sources` gained
`tests/cpp/test_cxx_system_error.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"system_error"` next to the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.

## Tests

`tests/cpp/test_cxx_system_error.cpp` reads the header. The constructor
stores `__v` into `_M_value`, and `value()` returns `_M_value`, so
`error_code(5, generic_category())` stays 5. The header text does not
contain Success. The default constructor stores 0, and `operator bool`
is `value() != 0`.

`tests/cxx_models/system_error_trace.cpp` includes `<system_error>`. It
builds `error_code(5, generic_category())`, checks `value()` is 5, and
checks a default `error_code` is false. It does not call `message()`.

`testdata/conformance/prism/cxx/cxx_system_error_true.cpp` is the same
check (`no-oob` true). `expected` in the yml is the boolean `true`.
`testdata/conformance/libc-models/system_error_contracts.cpp` is the
`#include <system_error>` the conformance case searches for. It is not a
scored task.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still reject source text
that contains `system_error` or `error_code` (`is not a system_error model`,
`is not an error_code model`) before the header is inlined. Those files
were not edited. Until that gate is gone, `cxx_system_error_true` does not
reach this header. `docs/PIR.md` was not edited.
