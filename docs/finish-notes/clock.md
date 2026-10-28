# clock

Branch `finish/libc-clock`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`clock` is a new function in `src/prism/pir/models/libc/time.cpp`. `time`, `mktime`, and `difftime` are unchanged. The null check in `mktime` is still `if (tp == 0)`, then `PTR-NULL-DEREF` and `return (time_t)-1`, and that check is still before `tp->tm_mon`.

`difftime` still takes the difference with `__builtin_sub_overflow`. When that subtraction does not fit in `time_t`, the function calls `__prism_check` with class `FUNC-CONTRACT` and message `difftime() difference does not fit in time_t`, then `__builtin_trap`. That arm has no return. It does not return a wrapped difference and it does not return 0. The fitting path is still `return (double)diff`.

`clock_t` is `typedef long clock_t`. `clock()` stores `__VERIFIER_nondet_long()` in a `clock_t` and returns that value. It does not return a constant. The body does not read a `struct tm` and does not mention `tm_mon`. It is not a proof of elapsed processor time. The comment above the function says clock is not PROVED and not CLEAN.

`strftime` is still not declared. `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself.

## Tests

`testdata/conformance/libc-models/time_contracts.cpp` calls `clock(` from `time_null_true`. The call names the function for the libc harness scan. It does not assert a clock reading. `time_contracts.yml` has no task that expects a specific clock value.

`tests/cpp/test_time_model.cpp` reads `time.cpp`, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It still requires `if (tp == 0)` before `tp->tm_mon` in `mktime`. It requires that `clock()` does not mention `tm_mon`. It checks the nondet return, the overflow trap in `difftime`, and that the yml does not name `clock`. It does not compile the model. `CMakeLists.txt` already lists that test and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
