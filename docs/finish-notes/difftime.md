# difftime

Branch `finish/libc-difftime`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`difftime` is a new function in `src/prism/pir/models/libc/time.cpp`. `time` and `mktime` are unchanged. The null check in `mktime` is still `if (tp == 0)`, then `PTR-NULL-DEREF` and `return (time_t)-1`, and that check is still before `tp->tm_mon`.

`difftime(time1, time0)` is the difference `time1 - time0` as a `double`, only when that integer subtraction fits in `time_t`. `__builtin_sub_overflow` writes the `time_t` difference. When the subtraction does not fit, the function calls `__prism_check` with class `FUNC-CONTRACT` and message `difftime() difference does not fit in time_t`, then `__builtin_trap`. That arm has no return. It does not return a wrapped difference and it does not return 0. The fitting path is `return (double)diff`, the `time_t` difference converted to `double`. The body does not read a `tm`. The comment above the function says this is not a calendar proof, and difftime is not PROVED and not CLEAN.

`strftime` is still not declared. `clock` is still not declared. `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself.

## Tests

`testdata/conformance/libc-models/time_contracts.cpp` calls `difftime`. `difftime_fit_true` asserts `difftime(5, 3) == 2.0`, `difftime(3, 5) == -2.0`, and `difftime(4, 4) == 0.0`. The equal case is the real difference 0. `difftime_nofit_false` asserts `difftime(LONG_MAX, LONG_MIN) == 0.0`. That assert is `FUNC-CONTRACT`.

`time_contracts.yml` has `format_version`, `input_files`, `language: C`, and `property: libc-contract`. `difftime_fit_true` is true. `difftime_nofit_false` is false and its `expect_class` is `FUNC-CONTRACT`. `mktime_month99_false` is still false with `expect_class` `FUNC-CONTRACT`.

`tests/cpp/test_time_model.cpp` reads `time.cpp`, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the overflow arm, the `(double)` conversion, that the body does not read a `tm`, and that the `mktime` null check is still before `tp->tm_mon`. It does not compile the model. `CMakeLists.txt` already lists that test and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
