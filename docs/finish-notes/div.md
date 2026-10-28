# div, ldiv, and lldiv

Slice `finish/libc-div`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`div`, `ldiv`, and `lldiv` are new functions in `src/prism/pir/models/libc/stdlib.cpp`, after `llabs` and still inside `extern "C"`. The file provides `div_t`, `ldiv_t`, and `lldiv_t`, each with `quot` and `rem`.

Division by zero is `__prism_check` with class `FUNC-CONTRACT`, then `__builtin_trap`. That path does not return a quotient or a remainder. `div(INT_MIN, -1)`, `ldiv(LONG_MIN, -1)`, and `lldiv(LLONG_MIN, -1)` are the same shape: `FUNC-CONTRACT` then `__builtin_trap`. Those paths do not negate the minimum and do not return a wrapped quotient.

Other values use toward-zero division. `quot` is the truncated quotient (`dividend / divisor`). `rem` is `dividend - quot * divisor`. The model does not invent a remainder with `%` or with a second nondeterministic value.

The comment above the three functions says they are not PROVED and not CLEAN. This is a model of the C rule that division by zero and an unrepresentable quotient are undefined. It is not a proof, and it is not a fuzzer `CLEAN` result.

`abs`, `labs`, `llabs`, `malloc`, `calloc`, `free`, `realloc`, `qsort`, `bsearch`, `atoi`, `atol`, `strtol`, `strtoul`, `rand`, `srand`, `getenv`, and `operator new` / `delete` are unchanged. The `abs` minimum checks stay: `FUNC-CONTRACT` then `__builtin_trap`, with no negation and no return on that arm. The comment on `rand` still says the seed is not stored.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `../../../src/prism/pir/models/libc/stdlib.cpp`. `div_quot_true` calls `div`, `ldiv`, and `lldiv` and asserts the truncated quotient and `rem == dividend - quot * divisor`. `div_rem_false`, `ldiv_rem_false`, and `lldiv_rem_false` are asserts that the toward-zero remainder of `-7/3` is `2`. A failed assert is `FUNC-CONTRACT`.

`stdlib_contracts.yml` lists `div_quot_true: true` and the three false tasks. Each false task's `expect_class` is `FUNC-CONTRACT`. Existing abs entries are unchanged, including `abs_intmin_false: "INT-SIGNED-OVF"`.

`tests/cpp/test_div.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the struct members, the disclaimer, both trap arms, the toward-zero formula, the harness calls, and the yml shape. It does not compile the model. The abs minimum checks remain in `tests/cpp/test_abs_min.cpp`. `tests/cpp/test_qsort_null.cpp` was not edited.

`CMakeLists.txt` lists `tests/cpp/test_div.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed.

`prism_tests` was not executed. This slice does not configure or build.
