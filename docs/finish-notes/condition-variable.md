# condition_variable slice

Branch `finish/cxx-condition-variable`, base `1278b9dc714ceaee54277a298c50212376942489`.

`<condition_variable>` is a file, `src/prism/pir/models/cxx/condition_variable`.
A directory of that name would make `#include <condition_variable>` fail with
EISDIR. The include guard is `_GLIBCXX_CONDITION_VARIABLE`, the usual
libstdc++ guard. The header does not include a libstdc++ file to learn that
name. `#pragma GCC system_header`. Namespace `std`. The header is partial on
purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml records the contract the harness asks for. `expected` is the YAML
boolean `true`. This header is not a concurrency proof.

## What the model covers

`std::condition_variable` only. The object stores one `bool`. Nothing is
allocated. There is no thread. A default condition_variable has the flag
false.

`notify_one()` and `notify_all()` set that flag. They do not run a waiter.
They do not invent a woken thread. Both write the same member. A second
notify leaves the flag set.

Copy construction and copy assignment are deleted. Move is not declared.
The destructor is defaulted. It does not free anything: the flag is a
member, not a heap object.

`extern "C" void __prism_check(int, const char*, const char*)` is declared
and not defined. Nothing calls it. notify has no failing operation.

The header does not include `<mutex>`, `<thread>`, `<atomic>`, `<memory>`,
`<chrono>`, or `<functional>`.

## What falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They
are not stubbed:

- `wait`, `wait_for`, and `wait_until`. A blocking wait that returned
  immediately would be a fake wakeup.
- `condition_variable_any`
- `cv_status`
- `notify_all_at_thread_exit`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/condition_variable`

so the existing embed writes `cxx/condition_variable` and the existing
`-isystem` directory is enough for `#include <condition_variable>`.
`_prism_test_sources` gained `tests/cpp/test_cxx_condition_variable.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `condition_variable` in the public
header array. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged. `iostream` and `ostream` stay absent.

`testdata/conformance/libc-models/condition_variable_contracts.cpp` is the
`#include <condition_variable>` the conformance case searches for. It is not
a scored task. There is no false twin.

## Tests

`tests/cpp/test_cxx_condition_variable.cpp` includes the model by path and
reads the header. The constructor initialises `_M_flag` to false. Each
notify body is `_M_flag = true` and does not call anything. The header does
not define `wait(`, `wait_for(`, or `wait_until(`. `__prism_check` appears
only as a declaration. `sizeof(std::condition_variable)` is `sizeof(bool)`,
and the destructor is trivial.

`tests/cxx_models/condition_variable_trace.cpp` includes
`<condition_variable>`, calls `notify_one` and `notify_all`, and prints
`notified`. It was not compiled in this slice. The doctest does not shell
out to that file.

`testdata/conformance/prism/cxx/cxx_condition_variable_true.cpp` is
memsafety true: it constructs a condition_variable and calls both notifies.
The yml `expected` value is the YAML boolean `true`. There is no false
task. PIR was not run.

## Still in front of the model

`src/prism/bmc_unenc.inc` still returns
`C++ std::condition_variable unencoded` (`is not a condvar/shared-mutex model`)
when the function body names `condition_variable` or `shared_mutex`, before
any header is inlined. `src/prism/bmc.cpp` returns that same reason when the
lowered text contains `condition_variable unencoded` or `shared_mutex`.
`src/prism/bmc_encoder.inc` still throws `condition_variable unencoded` for
those tokens. Those files were not edited. Until that gate is gone,
`cxx_condition_variable_true` does not reach this header through bmc.
`docs/PIR.md` was not edited.

## Update (prism-llvm, LLVM 23 session)
Superseded in part: wait/wait_for/wait_until now release the lock and run
queued threads through bits/prism_sched.h (interleaving exploration), and
<mutex> gained lock(), lock_guard and unique_lock. See coverage-limits.md.
