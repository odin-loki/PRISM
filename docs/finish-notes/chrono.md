# chrono slice

Branch `finish/cxx-chrono`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.

`std::chrono` is one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`.

The solver was not run. The yml files record the contract each harness asks
for. This header is not a proof. cmake, `prism_tests`, and pir were not run.

## What the model covers

A partial `<chrono>`. `duration` stores a signed 64-bit count (`_Rep` must
be a signed 64-bit type; the member is that count). `count()` returns it.
A default duration is zero, and adding or subtracting zero leaves the other
count unchanged. Zero stays zero.

Addition and subtraction that would overflow the representation are a
precondition failure: `__prism_check` with class `INT-SIGNED-OVF`. The
overflow path returns a zero duration. It does not wrap to a negative count
and return that as success. A duration of the maximum count plus one count
is that failure.

`<ratio>` is not included. Another change owns that header. The default
period is a local stand-in so `duration<long long>` parses here. Different
periods are not converted.

## What is omitted

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- clocks
- `time_point` conversions that call the OS
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait)
- `duration_cast`, the calendar typedefs, and compound assignment

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/chrono`

so the existing embed writes `cxx/chrono`. `_prism_test_sources` gained
`tests/cpp/test_cxx_chrono.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`chrono` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_chrono.cpp` reads the header as text. It checks that
the file is not a directory, that `<ratio>` is not included, and that each
of addition and subtraction calls `__prism_check` before returning a sum.
The overflow path is `return duration<_Rep, _Period>(0)` after the check.
The header does not contain a wrapping `a + b` returned as the sum without
that check.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
prints a zero count, `2 + 3`, and `5 - 1`. `ovf` adds the maximum count to
1. `testdata/conformance/libc-models/chrono_contracts.cpp` is the
`#include <chrono>` the conformance case searches for.

`testdata/conformance/prism/cxx/cxx_chrono_true.cpp` adds two small
durations and checks the count (`no-overflow`, `expected: true`).
`cxx_chrono_false.cpp` adds a duration of the maximum int64 to a duration
of 1 (`expected: false`, witness `[0]`). Both `expected` values are YAML
booleans. A clang++ `-fsanitize=undefined` build of the false task against
libstdc++ aborts on that signed addition. Against this header the same sum
calls `__prism_check` and does not return a negative count.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ chrono unencoded` (`is not a chrono model`) from the source text,
before the header is inlined. Those files were not edited. Until that gate
is gone, a unit that names `std::chrono` does not reach this header.
`docs/PIR.md` was not edited.
