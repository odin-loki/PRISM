# time and mktime

Branch `finish/libc-time`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`time` and `mktime` are the only functions in `src/prism/pir/models/libc/time.cpp`. `strftime` is not declared. `clock` is not declared. A clock that returned 0 would look like a measured duration.

`stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `ctype.cpp` were not edited. `tests/cpp/test_qa_conformance.cpp` was not edited. `docs/PIR.md` was not edited.

## time

`time(time_t *t)` takes its value from `__VERIFIER_nondet_long`. That function is already declared `extern "C"` in `prism_model.h`, which this file includes. When `t` is non-null, the same value is stored through `t`. A null `t` stores nothing.

The comment above the function says time is not PROVED and not CLEAN. The value is an unknown calendar encoding. It is not a wall-clock proof, and it is not a fuzzer CLEAN result.

## mktime

`tm_mon` outside 0..11 or `tm_mday` outside 1..31 calls `__prism_check` with class `FUNC-CONTRACT` and message `mktime() month or day is outside 0..11 or 1..31`, then returns `(time_t)-1`. The check is before that return. The arm does not return 0. 0 would be a successful calendar time.

Month 99 is not normalized into a later year. The failure arm does not read `tm_year`, `tm_sec`, `tm_min`, `tm_hour`, `tm_wday`, `tm_yday`, or `tm_isdst`. Those fields are members of `struct tm` and stay unread on this path. No seconds since 1970 are computed.

The in-range path is `__prism_assume(0)` and then `(time_t)-1`. That path is not a calendar time, so it does not return 0 and does not invent an epoch. The comment above `mktime` says mktime is not PROVED and not CLEAN.

## Wiring

`PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes `time.cpp` itself, which is what the conformance scan requires of every libc model file. `_prism_test_sources` gained one line, `tests/cpp/test_time_model.cpp`.

## Tests

`tests/cpp/test_time_model.cpp` reads `time.cpp` from `PRISM_SOURCE_DIR`. It checks the nondet value, the null store, the out-of-range `(time_t)-1` return, and that `strftime` and `clock` are absent from the file.

`testdata/conformance/libc-models/time_contracts.cpp` includes the model and calls `time` and `mktime`. `time_contracts.yml` has `format_version`, `input_files`, `language: C`, and `property: libc-contract`. `time_null_true` and `time_store_true` are true. `mktime_month99_false` is false and its `expect_class` is `FUNC-CONTRACT`. That function sets month 99 and asserts `mktime` returns 0.

`prism_tests` was not executed. This slice does not configure or build.
