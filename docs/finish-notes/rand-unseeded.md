# rand is unseeded

Slice `finish/rand-unseeded`, base `e57213d1404ec1821382910b632e2815547aacf9`.

`rand` in `src/prism/pir/models/libc/stdlib.cpp` returns a nondeterministic non-negative `int`: `__VERIFIER_nondet_int`, then `__prism_assume(r >= 0)`. `srand` discards its argument with `(void)s`. Those function bodies are the existing model.

The comment on `srand` and `rand` says the seed is not stored, rand is not a reproducible sequence, and a run of rand is not PROVED and not CLEAN. No seed variable is written, so later `rand` calls are fresh nondet values. That run is a model of an unknown sequence. It is not a proof, and it is not a fuzzer `CLEAN` result.

`atoi`, `atol`, `strtol`, `getenv`, `malloc`, `calloc`, `realloc`, `free`, and `operator new` stay as they are. `string.cpp` and `stdio.cpp` stay as they are.

## Tests

`tests/cpp/test_rand_unseeded.cpp` reads `stdlib.cpp` as text from `PRISM_SOURCE_DIR`. The `srand` body assigns neither a static nor a global. The `rand` body contains `__VERIFIER_nondet_int`. The comment between `strtoul` and `rand` says the seed is not stored, and it also says the sequence is not reproducible and that a run is not PROVED and not CLEAN.

`CMakeLists.txt` lists `tests/cpp/test_rand_unseeded.cpp` as one new line in `_prism_test_sources`.

`prism_tests` was not executed. This slice does not configure or build.
