# strtoll and strtoull

Branch `finish/libc-strtoll`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`strtoll` and `strtoull` are new functions in `src/prism/pir/models/libc/stdlib.cpp`, after `strtol` and before `strtoul`, still inside `extern "C"`. `malloc`, `calloc`, `free`, `realloc`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `atoi`, `atol`, `strtol`, `strtoul`, `rand`, `srand`, `getenv`, `qsort`, `bsearch`, and `operator new` / `delete` are unchanged. The comment on `rand` still says the seed is not stored. `string.cpp` and `stdio.cpp` are unchanged. `tests/cpp/test_qsort_null.cpp` was not edited.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message names the function: `strtoll() called with unchecked pointer parameter s`, and the same shape for `strtoull`. The check is followed by `return 0`. That path does not call `strlen`, does not write `*end`, and does not scan.

A non-null `s` follows `strtol`. The model calls `strlen`. When `end` is non-null, it stores a pointer into the string. The offset is `__VERIFIER_nondet_ulong`, assumed `<=` the length. The stored pointer is `(char *)s + k`.

The value is unknown. This is not a conversion proof. Neither function returns a parsed numeral.

`strtoll` returns `__VERIFIER_nondet_longlong`. That name is a nondet intrinsic: the PIR translator treats an undefined `__VERIFIER_nondet_*` call as a havoc of the result width, and the bmc encoder gives `__VERIFIER_nondet_longlong` the width of `long long`. `prism_model.h` does not declare it, so `stdlib.cpp` declares it with C linkage. `strtoull` returns `__VERIFIER_nondet_ulonglong`, declared the same way. That result is unsigned. `strtoull` does not call `strtol` or `strtoll`, and it does not cast either of them, so a negative value does not become the unsigned result.

The comment above the two functions says they are not PROVED and not CLEAN. This is a partial model of the null check and of an end pointer inside the string. It is not a proof, and it is not a fuzzer `CLEAN` result.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `stdlib.cpp`. `strtol_true` calls `strtoll` and `strtoull` after `strtoul`. Those calls do not assert a parsed number. `stdlib_contracts.yml` was not given a `strtoll` or `strtoull` task.

`tests/cpp/test_strtoll.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks that a null `s` returns 0 before `strlen` and before any store through `end`, that `strtoull` does not call `strtol`, and that the harness calls both functions. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strtoll.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. These calls are those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
