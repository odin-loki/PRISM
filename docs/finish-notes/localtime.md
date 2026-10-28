# localtime

Branch `finish/libc-localtime`, base `d481207ffc8c452d59399ea19bc752c2f4a2bc26`.

`localtime` is a new function in `src/prism/pir/models/libc/time.cpp`. `time`, `mktime`, `difftime`, `clock`, `clock_gettime`, and `gmtime` are unchanged. The null check in `mktime` is still `if (tp == 0)`, then `PTR-NULL-DEREF` and `return (time_t)-1`, and that check is still before `tp->tm_mon`.

A null `time_t *` calls `__prism_check` with class `PTR-NULL-DEREF` and message `localtime null time_t`, then returns `0`. That return is before any `tm` field is written. A live `time_t *` returns a pointer to static storage. Each field is `__VERIFIER_nondet_int()`. The fields are not constrained to a month or day range. The `time_t` value is not read and not converted. The stores are not a proof of the local calendar and not a proof of a timezone. The comment above the function says localtime is not PROVED and not CLEAN.

`strftime` is still not declared. `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself.

## Tests

`testdata/conformance/libc-models/time_contracts.cpp` calls `localtime(` from `time_null_true`, on a live `time_t`. The call names the function for the libc harness scan. It does not assert a date. `time_contracts.yml` was not edited and has no task that expects a specific date.

`tests/cpp/test_time_model.cpp` reads `time.cpp`, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null arm, the `0` return before any field store, the nondet fields, that those fields are not a calendar range, and that the yml does not name `localtime`. It still requires `if (tp == 0)` before `tp->tm_mon` in `mktime`. It does not compile the model. `CMakeLists.txt` already lists that test and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
