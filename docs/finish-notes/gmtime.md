# gmtime

Branch `finish/libc-gmtime`, base `b703c118fcf4cbb6757f1b7b40a8b740d56dba7b`.

`gmtime` is a new function in `src/prism/pir/models/libc/time.cpp`. `time`, `mktime`, `difftime`, `clock`, and `clock_gettime` are unchanged. The null check in `mktime` is still `if (tp == 0)`, then `PTR-NULL-DEREF` and `return (time_t)-1`, and that check is still before `tp->tm_mon`.

A null `time_t *` calls `__prism_check` with class `PTR-NULL-DEREF` and message `gmtime null time_t`, then returns `0`. That return is before any `tm` field is written. A live `time_t *` returns a pointer to static storage. Each field is `__VERIFIER_nondet_int()`. `tm_mon` is constrained with `__prism_assume` to `0..11` and `tm_mday` to `1..31`, the ranges `mktime` already uses, and those assumes are before the stores. The `time_t` value is not read and not converted. The stores are not a proof of the calendar time. The comment above the function says gmtime is not PROVED and not CLEAN.

`strftime` is still not declared. `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself.

## Tests

`testdata/conformance/libc-models/time_contracts.cpp` calls `gmtime(` from `time_null_true`, on a live `time_t`. The call names the function for the libc harness scan. It does not assert a date. `time_contracts.yml` was not edited and has no task that expects a specific date.

`tests/cpp/test_time_model.cpp` reads `time.cpp`, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null arm, the `0` return before any field store, the nondet fields, the `0..11` and `1..31` assumes before the stores, and that the yml does not name `gmtime`. It still requires `if (tp == 0)` before `tp->tm_mon` in `mktime`. It does not compile the model. `CMakeLists.txt` already lists that test and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
