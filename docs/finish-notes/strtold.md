# strtold

Branch `finish/libc-strtold`, base `64932ab4bc10c3392ce2a0043946cda480104467`.

`strtold` is a new function in `src/prism/pir/models/libc/stdlib.cpp`, after `strtof` and before `strtoul`, still inside `extern "C"`. `malloc`, `calloc`, `free`, `realloc`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `atoi`, `atol`, `atoll`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `strtof`, `rand`, `srand`, `getenv`, `qsort`, `bsearch`, and `operator new` / `delete` are unchanged. The comment on `rand` still says the seed is not stored. `string.cpp` and `stdio.cpp` are unchanged. `tests/cpp/test_qsort_null.cpp` was not edited.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message is `strtold() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not call `strlen`, does not write `*end`, and does not scan.

A non-null `s` may call `strlen`. The call is `size_t n = strlen(s)`. When `end` is non-null, the model stores a pointer into the string. The offset is `__VERIFIER_nondet_ulong`, assumed `<=` the length. The stored pointer is `(char *)s + k`, so the end pointer stays inside the string.

The value is unknown. This is not a conversion proof. `strtold` does not return a parsed numeral. It returns `__VERIFIER_nondet_longdouble()`. That name is a nondet intrinsic: the PIR translator treats an undefined `__VERIFIER_nondet_*` call as a havoc of the result width when that width is modelled. `long double` is `x86_fp80` on the LP64 clang lower, and that format stays `UNENCODED` (docs/PIR.md, "What is not encoded" and "Floating point"). The return is not a modelled IEEE value. `prism_model.h` does not declare `__VERIFIER_nondet_longdouble`, so `stdlib.cpp` declares it with C linkage. `strtold` does not call `strtof`, `strtod`, `strtol`, `strtoll`, or `strtoul`.

The comment above `strtold` says this is not PROVED and not CLEAN. This is a partial model of the null check and of an end pointer inside the string. It is not a proof, and it is not a fuzzer `CLEAN` result.

## Tests

`testdata/conformance/libc-models/stdlib_contracts.cpp` already includes `stdlib.cpp`. `strtol_true` calls `strtold` after `strtof`. That call does not assert a parsed number. The return type is `long double`. On the LP64 clang lower that is `x86_fp80`, and `value_width` refuses that format before the call is inlined, so this call does not keep `strtol_true` a proof of the end pointer. `stdlib_contracts.yml` was not given a `strtold` task. The existing `strtol_true: true` line was left in place.

`tests/cpp/test_strtold.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks that a null `s` returns 0 before `strlen`, that a non-null `end` is stored inside the string, that the result is `__VERIFIER_nondet_longdouble()`, and that the harness calls `strtold`. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strtold.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
