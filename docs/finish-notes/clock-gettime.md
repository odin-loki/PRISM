# clock_gettime

Branch `finish/libc-clock-gettime`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`clock_gettime` is a new function in `src/prism/pir/models/libc/time.cpp`. `time`, `mktime`, `difftime`, and `clock` are unchanged. The null check in `mktime` is still `if (tp == 0)`, then `PTR-NULL-DEREF` and `return (time_t)-1`, and that check is still before `tp->tm_mon`.

`clockid_t` is `typedef int clockid_t`. `struct timespec` holds `time_t tv_sec` and `long tv_nsec`. A null `timespec *` calls `__prism_check` with class `PTR-NULL-DEREF` and message `clock_gettime null timespec`, then returns `-1`. That return is before any store through the pointer. A live timespec stores `__VERIFIER_nondet_long()` in `tv_sec`. `tv_nsec` is a `__VERIFIER_nondet_long()` value constrained with `__prism_assume` to `0..999999999`, and that assume is before the `tv_nsec` store. The function then returns `0`. That `0` is the success status, not a time. The stores are not a proof of the current time. The comment above the function says clock_gettime is not PROVED and not CLEAN.

`strftime` is still not declared. `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself.

## Tests

`testdata/conformance/libc-models/time_contracts.cpp` calls `clock_gettime(` from `time_null_true`, on a live `struct timespec`. The call names the function for the libc harness scan. It does not assert a timespec value. `time_contracts.yml` was not edited and has no task that expects a specific time.

`tests/cpp/test_time_model.cpp` reads `time.cpp`, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null arm, the `-1` return before either store, the nondet `tv_sec`, the `0..999999999` assume before `tv_nsec`, and that the yml does not name `clock_gettime`. It still requires `if (tp == 0)` before `tp->tm_mon` in `mktime`. It does not compile the model. `CMakeLists.txt` already lists that test and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
