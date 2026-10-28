# atoll

Branch `finish/libc-atoll`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`atoll` is a new function in `src/prism/pir/models/libc/stdlib.cpp`, after `strtoull` and before `strtoul`, still inside `extern "C"`. `malloc`, `calloc`, `free`, `realloc`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `atoi`, `atol`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `rand`, `srand`, `getenv`, `qsort`, `bsearch`, and `operator new` / `delete` are unchanged. The comment on `rand` still says the seed is not stored. `string.cpp` and `stdio.cpp` are unchanged. `tests/cpp/test_qsort_null.cpp` was not edited.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message is `atoll() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not call `strlen`.

A non-null `s` may call `strlen`, so the end is inside the string. The call is `(void)strlen(s)`. There is no end pointer to store.

The value is unknown. This is not a conversion proof. `atoll` does not return a parsed numeral. It returns `__VERIFIER_nondet_longlong()`. That name is already declared for `strtoll`: the PIR translator treats an undefined `__VERIFIER_nondet_*` call as a havoc of the result width, and the bmc encoder gives `__VERIFIER_nondet_longlong` the width of `long long`. `atoll` does not call `strtol` or `strtoll`.

The comment above `atoll` says this is not PROVED and not CLEAN. This is a partial model of the null check and of a terminator inside the string. It is not a proof, and it is not a fuzzer `CLEAN` result.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `stdlib.cpp`. `strtol_true` calls `atoll` after `strtoull`. That call does not assert a parsed number. `stdlib_contracts.yml` was not given an `atoll` task.

`tests/cpp/test_atoll.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks that a null `s` returns 0 before `strlen`, that the result is `__VERIFIER_nondet_longlong()`, and that the harness calls `atoll`. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_atoll.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
