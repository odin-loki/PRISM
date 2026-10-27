# shared_mutex slice

Branch `finish/cxx-shared-mutex`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<shared_mutex>` is a file, `src/prism/pir/models/cxx/shared_mutex`, the same
shape as `vector` and `memory`. A directory of that name would make
`#include <shared_mutex>` fail with EISDIR. The header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. Expected values are
YAML booleans.

## What the model covers

`std::shared_mutex` only. The object stores an exclusive flag and a shared
count. Nothing is allocated. There is no thread and no owner id. This header
is not a concurrency proof.

`try_lock()` sets the exclusive flag only when that flag is clear and the
shared count is 0. Otherwise it returns false and changes nothing.
`try_lock_shared()` increments the shared count only when the exclusive flag
is clear. If exclusive is set it returns false and does not increment.
Several shared holds are a larger count. They are not several objects.

`unlock()` requires the exclusive flag. `unlock_shared()` requires a shared
count greater than 0. Otherwise `__prism_check` with class `FUNC-CONTRACT`.
The failing path does not invent a holder: `unlock` does not set the
exclusive flag, and `unlock_shared` does not increment the count. A zero
count is not decremented.

## What falls through to libstdc++

`lock()` and `lock_shared()` are omitted so the unit fails to compile and
pir falls back. A blocking call that returned immediately would be a fake
acquisition. They are not stubbed. `shared_timed_mutex`, `shared_lock`, and
`native_handle` are not declared either, so those names fall back the same
way (`extra.cxx_models`).

The header does not include `<mutex>`, `<thread>`, or `<memory>`.

`src/prism/bmc.cpp` still returns "not a condvar/shared-mutex model" when the
function source contains `shared_mutex`. `src/prism/bmc_unenc.inc` and
`src/prism/bmc_encoder.inc` still throw that same unencoded reason. This
slice does not edit those gates. The hand parser is not this model. PIR's
clang path is what puts `cxx/shared_mutex` on the include path. The header
is not a concurrency proof, and it does not remove the `CXX-SHARED-MUTEX`
lint.

## Tests

`tests/cpp/test_cxx_shared_mutex.cpp` reads the header. Exclusive `try_lock`
returns false while `_M_shared != 0`, and only then may it set the exclusive
flag. `unlock` of an unlocked `shared_mutex` is `__prism_check` on the
exclusive flag. The header does not define `lock(`. `try_lock_shared` returns
false before it increments when exclusive is set. `unlock_shared` checks
`_M_shared > 0` and does not invent a shared hold.

`tests/cxx_models/shared_mutex_trace.cpp` includes `<shared_mutex>`. It takes
one shared hold, sees exclusive `try_lock` fail, releases it, then takes the
exclusive hold and sees `try_lock_shared` fail. The doctest does not shell
out to that file.

`testdata/conformance/prism/cxx/cxx_shared_mutex_true.cpp` is
`try_lock_shared` once, a second `try_lock` (exclusive) false, then
`unlock_shared` (`memsafety` true). `cxx_shared_mutex_false.cpp` is
`unlock_shared` when the shared count is 0 (`memsafety` false, witness
`[0]`). That failure is `__prism_check`. A sanitizer replay of libstdc++
does not define that intrinsic, so the false task is `sanitizer_blind`.

`testdata/conformance/libc-models/shared_mutex_contracts.cpp` includes
`<shared_mutex>`. The shared-then-exclusive and exclusive-then-shared
sequences are true. `unlock_shared` at count 0 and `unlock` with the
exclusive flag clear are `FUNC-CONTRACT`.

## Wiring

`PRISM_PIR_MODELS` gained `src/prism/pir/models/cxx/shared_mutex`.
`_prism_test_sources` gained `tests/cpp/test_cxx_shared_mutex.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"shared_mutex"` next to the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.
