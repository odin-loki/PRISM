# mutex slice

Branch `finish/cxx-mutex`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<mutex>` is a file, `src/prism/pir/models/cxx/mutex`, the same shape as
`memory`. A directory of that name would make `#include <mutex>` fail with
EISDIR. The header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans.

## What the model covers

`std::mutex` only. The object stores a locked flag. There is no heap
allocation and no thread. This header is not a concurrency proof.

`try_lock` returns true and sets the flag only when the mutex was unlocked.
When it is already locked, `try_lock` returns false and leaves the flag
locked. It does not return true while locked.

`unlock` on a locked mutex clears the flag. `unlock` on an unlocked mutex is
`__prism_check` with class `CXX-MUTEX-UNLOCK` and the message
`mutex::unlock on an unlocked mutex`. The check does not set the flag first.
It does not pretend the mutex was locked.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). A partial
header is intentional. They are not stubbed:

- `lock`. A blocking lock that returns immediately would be a fake
  acquisition. lock is omitted so the unit fails to compile and pir falls back.
- `timed_mutex`, `recursive_mutex`, `scoped_lock`, `lock_guard`, `unique_lock`

The header does not `#include <thread>`, `<memory>`, or `<shared_mutex>`.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not a C++
mutex model" from the source text before any header is inlined. This slice
does not change that, and it does not edit `stage.cpp`. `stage.cpp` already
puts the embedded `cxx/` directory on the include path.
`tests/cpp/test_concolic.cpp` still expects `mutex_unenc_bad` to be
`NEEDS-HARNESS`.

## Tests

`tests/cpp/test_cxx_mutex.cpp` reads the header. The locked path of
`try_lock` returns false before the flag is set, and the only `return true`
is after that store. `unlock` of an unlocked mutex is `__prism_check` and
does not assign the flag true. The header does not define `lock(`.

`tests/cxx_models/mutex_trace.cpp` includes `<mutex>`. The default path
try_locks once (the second try_lock is false), unlocks, and try_locks again.
`argv` `unlocked` calls unlock on a mutex that was never locked.

`testdata/conformance/prism/cxx/cxx_mutex_true.cpp` try_locks once and the
second try_lock is false (`memsafety` true). `cxx_mutex_false.cpp` unlocks a
mutex that was never locked when `c` is 0 (`false`, witness `[0]`).

`testdata/conformance/libc-models/mutex_contracts.cpp` is the libc-contract
harness (`#include <mutex>`). `mutex_second_trylock_false` plants
`FUNC-CONTRACT` (a second try_lock asserted true). `mutex_unlock_unlocked_false`
plants `CXX-MUTEX-UNLOCK`.

## Wiring

`PRISM_PIR_MODELS` gained `src/prism/pir/models/cxx/mutex`.
`_prism_test_sources` gained `tests/cpp/test_cxx_mutex.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"mutex"` next to the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.
