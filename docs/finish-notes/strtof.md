# strtof

Branch `finish/libc-strtof`, base `d481207ffc8c452d59399ea19bc752c2f4a2bc26`.

`strtof` is a new function in `src/prism/pir/models/libc/stdlib.cpp`, after `strtod` and before `strtoul`, still inside `extern "C"`. `malloc`, `calloc`, `free`, `realloc`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `atoi`, `atol`, `atoll`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `rand`, `srand`, `getenv`, `qsort`, `bsearch`, and `operator new` / `delete` are unchanged. The comment on `rand` still says the seed is not stored. `string.cpp` and `stdio.cpp` are unchanged. `tests/cpp/test_qsort_null.cpp` was not edited.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message is `strtof() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not call `strlen`, does not write `*end`, and does not scan.

A non-null `s` may call `strlen`. The call is `size_t n = strlen(s)`. When `end` is non-null, the model stores a pointer into the string. The offset is `__VERIFIER_nondet_ulong`, assumed `<=` the length. The stored pointer is `(char *)s + k`, so the end pointer stays inside the string.

The value is unknown. This is not a conversion proof. `strtof` does not return a parsed numeral. It returns `__VERIFIER_nondet_float()`. That name is a nondet intrinsic: the PIR translator treats an undefined `__VERIFIER_nondet_*` call as a havoc of the result width, and a `float` result is the IEEE bits of that width. `prism_model.h` does not declare it, so `stdlib.cpp` declares it with C linkage. `strtof` does not call `strtod`, `strtol`, `strtoll`, or `strtoul`.

The comment above `strtof` says this is not PROVED and not CLEAN. This is a partial model of the null check and of an end pointer inside the string. It is not a proof, and it is not a fuzzer `CLEAN` result.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `stdlib.cpp`. `strtol_true` calls `strtof` after `strtod`. That call does not assert a parsed number. `stdlib_contracts.yml` was not given a `strtof` task.

`tests/cpp/test_strtof.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks that a null `s` returns 0 before `strlen`, that a non-null `end` is stored inside the string, that the result is `__VERIFIER_nondet_float()`, and that the harness calls `strtof`. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strtof.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
