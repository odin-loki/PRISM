# barrier slice

Branch `finish/cxx-barrier`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.

`std::barrier` is one extensionless file, `src/prism/pir/models/cxx/barrier`.
A directory of that name would make `#include <barrier>` fail with EISDIR
and every such unit would fall back to libstdc++. The include guard is
`_GLIBCXX_BARRIER`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml files record the contract each task asks
for. `expected` values are YAML booleans. Neither task is marked `PROVED`.

## What the model is

A partial `<barrier>`. The expected arrival count is a `ptrdiff_t` stored
in the object. The constructor stores that count. Copy and copy-assignment
are deleted. The destructor is defaulted. Nothing allocates, and nothing
starts a thread. This header is not a concurrency proof: the count is not
atomic, and `arrive` does not pretend that other threads arrived.

`arrive(n)` subtracts `n` only when `n` is not greater than the remaining
count. An arrive that would pass below zero is `__prism_check` with class
`CXX-BARRIER-ARRIVE` and message `barrier::arrive past the expected count`.
On that path the stored count is left unchanged, so it does not wrap.

`arrive_and_drop()` decreases the expected count by one and does not wait.
If the expected count is already zero, it is `__prism_check` with class
`CXX-BARRIER-DROP`, and the count stays zero.

`wait()` and `arrive_and_wait()` are omitted. A blocking wait that returns
immediately would be a fake synchronization success. Those names are
omitted so the unit fails to compile and `lower_to_ir` lowers it again with
libstdc++ (`extra.cxx_models` records the fallback). The completion-function
template parameter and the arrival token are omitted for the same reason.
The header does not include `<atomic>`, `<latch>`, `<semaphore>`, or
`<thread>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/barrier`

so the existing embed writes `cxx/barrier` and the existing `-isystem`
directory is enough for `#include <barrier>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_barrier.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`barrier` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/barrier_contracts.cpp` is the
`#include <barrier>` that case searches for. It arrives exactly the
expected count and does not call `wait`. It is not a scored task.

## Tests

`tests/cxx_models/barrier_trace.cpp` includes `<barrier>`, arrives exactly
the expected count (`barrier(2)` then `arrive(2)`), and does not call
`wait`. `tests/cpp/test_cxx_barrier.cpp` reads the model header without
running pir: `arrive` past the count is a `__prism_check`, and after
comments are stripped the header does not define `wait(`.

`testdata/conformance/prism/cxx/cxx_barrier_true.cpp` arrives exactly the
expected count and does not call `wait` (`expected: true`).
`cxx_barrier_false.cpp` calls `arrive` past the expected count
(`expected: false`). The yml schema matches `cxx_array_true.yml`:
`format_version`, `input_files`, `language`, `property`, and `expected`
with a YAML boolean. cmake, `prism_tests`, and pir were not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::latch/barrier unencoded` (`is not a sync primitive model`) from
the source text, before the header is inlined. Those files were not
edited. Until that gate is gone, `cxx_barrier_true` and
`cxx_barrier_false` do not reach this header. `docs/PIR.md` was not edited.
