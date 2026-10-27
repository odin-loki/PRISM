# semaphore slice

Branch `finish/cxx-semaphore`, base `3e0ebbb46361d4d3c7557657eeeb590d2ae3aa19`.

`std::counting_semaphore` is one extensionless file,
`src/prism/pir/models/cxx/semaphore`. A directory of that name would make
`#include <semaphore>` fail with EISDIR. The include guard is
`_GLIBCXX_SEMAPHORE`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` values are YAML booleans.

This header is not a concurrency proof. It does not include `<atomic>`,
`<latch>`, or `<thread>`. `src/prism/conc/` was not edited.

## What the model is

`counting_semaphore<LeastMaxValue>` holds one `ptrdiff_t` counter. The
constructor stores its argument when that argument is in `[0, max]`. Outside
that range `__prism_check` fires with class `CXX-SEMAPHORE-MAX` and the
counter stays 0, so it still does not exceed max.

`release(n)` that would leave the counter above the max template argument,
or that passes a negative update, is the same check
(`counting_semaphore::release exceeds max`). The update is added only when
the check passes. The counter is not wrapped.

`try_acquire` returns false when the counter is zero and does not decrement.
When the counter is positive it decrements by one and returns true. It does
not return true while the counter is zero. That false result is defined. It
is not a check, and it does not change a later `release`.

`max()` is the template argument. `binary_semaphore` is
`counting_semaphore<1>`. Copy construction and copy assignment are deleted.
The destructor is defaulted. It does not free anything: the counter is a
member, not a heap object.

## What falls through to libstdc++

The blocking wait and the timed waits are not declared. A blocking wait that
returned immediately would be a fake success. Omitted names fail to compile
against this header and `lower_to_ir` lowers that translation unit again
with libstdc++ (`extra.cxx_models`). They are not stubbed.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/semaphore`

so the existing embed writes `cxx/semaphore` and the existing `-isystem`
directory is enough for `#include <semaphore>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_semaphore.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`semaphore` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/semaphore_contracts.cpp` is the
`#include <semaphore>` that case searches for. It is not a scored task.

## Tests

`tests/cxx_models/semaphore_trace.cpp` includes `<semaphore>`. The default
path prints `zero=0`, `took=1`, `empty=0`: an empty `try_acquire` is false,
and the following `release` still leaves a token. argv `over` releases past
max on a `counting_semaphore<1>` that already holds one. The model prints
`CHECK CXX-SEMAPHORE-MAX` and `over a=1 b=0` (the counter stayed at one; it
did not wrap to zero and it did not become two).

`tests/cpp/test_cxx_semaphore.cpp` is that trace, plus a read of the header.
The header text has no blocking `acquire(`. A translation unit that calls
`acquire()` does not compile against this header. Without a usable clang++
the run is `NOTRUN`, same as the other C++ model driver. The header read
does not need the compiler.

cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_semaphore_true.cpp` is memsafety true: it
takes the token `release` added and checks `try_acquire` on empty is false.
`cxx_semaphore_false.cpp` is `release` past max (`witness` `[1]`).
`try_acquire` on zero is not that task. The false yml is `sanitizer_blind`.
On libstdc++ (GCC 14) `counting_semaphore<N>::max()` returns N, and `release`
still adds with no check against that N (the atomic backend's limit is
`INT_MAX`). The label self-check does not define `_GLIBCXX_ASSERTIONS`, and
ASan/UBSan stay clean when the counter moves from 1 to 2.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ std::latch/barrier unencoded` (`is not a sync primitive model`) when
the source text contains `counting_semaphore`, before this header is
inlined. A separate gate still reports `C++ binary_semaphore unencoded`
when the source names `binary_semaphore`. Those files were not edited.
Until the `counting_semaphore` gate is gone, `cxx_semaphore_true` and
`cxx_semaphore_false` do not reach this header through bmc. `docs/PIR.md`
was not edited.
