# strtod

Branch `finish/libc-strtod`, base `b703c118fcf4cbb6757f1b7b40a8b740d56dba7b`.

`strtod` is a new function in `src/prism/pir/models/libc/stdlib.cpp`, after `atoll` and before `strtoul`, still inside `extern "C"`. `malloc`, `calloc`, `free`, `realloc`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `atoi`, `atol`, `atoll`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `rand`, `srand`, `getenv`, `qsort`, `bsearch`, and `operator new` / `delete` are unchanged. The comment on `rand` still says the seed is not stored. `string.cpp` and `stdio.cpp` are unchanged. `tests/cpp/test_qsort_null.cpp` was not edited.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message is `strtod() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not call `strlen`, does not write `*end`, and does not scan.

A non-null `s` may call `strlen`. The call is `size_t n = strlen(s)`. When `end` is non-null, the model stores a pointer into the string. The offset is `__VERIFIER_nondet_ulong`, assumed `<=` the length. The stored pointer is `(char *)s + k`, so the end pointer stays inside the string.

The value is unknown. This is not a conversion proof. `strtod` does not return a parsed numeral. It returns `__VERIFIER_nondet_double()`. That name is a nondet intrinsic: the PIR translator treats an undefined `__VERIFIER_nondet_*` call as a havoc of the result width, and a `double` result is the IEEE bits of that width. `prism_model.h` does not declare it, so `stdlib.cpp` declares it with C linkage. `strtod` does not call `strtol`, `strtoll`, `strtoul`, or `atoll`.

The comment above `strtod` says this is not PROVED and not CLEAN. This is a partial model of the null check and of an end pointer inside the string. It is not a proof, and it is not a fuzzer `CLEAN` result.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `stdlib.cpp`. `strtol_true` calls `strtod` after `atoll`. That call does not assert a parsed number. `stdlib_contracts.yml` was not given a `strtod` task.

`tests/cpp/test_strtod.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks that a null `s` returns 0 before `strlen`, that a non-null `end` is stored inside the string, that the result is `__VERIFIER_nondet_double()`, and that the harness calls `strtod`. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strtod.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
