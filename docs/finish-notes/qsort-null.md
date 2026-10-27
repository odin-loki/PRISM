# qsort and bsearch null checks

Slice `finish/qsort-null`, base `1278b9dc714ceaee54277a298c50212376942489`.

`qsort` and `bsearch` are new functions in `src/prism/pir/models/libc/stdlib.cpp`, after `getenv` and still inside `extern "C"`. `malloc`, `calloc`, `realloc`, `free`, `abs`, `labs`, `llabs`, `atoi`, `atol`, `strtol`, `strtoul`, `rand`, `srand`, `getenv`, and `operator new` / `delete` are the same bytes as before. The comment immediately above `rand` still says the seed is not stored, that rand is not a reproducible sequence, and that a run of rand is not PROVED and not CLEAN. `string.cpp` and `stdio.cpp` are unchanged. No C++ header was added.

## qsort

`nmemb == 0` or `size == 0` returns before any read of `base` and before any call through `compar`.

When both are positive, `nmemb * size` is `__builtin_mul_overflow`. A wrap is `__prism_check` with class `MEM-OOB-READ` and message `qsort nmemb * size overflows`, then `__prism_assume(0)`. That arm does not index the array and does not return a finished sort: the assume makes the path infeasible, so the function epilogue is not a successful `qsort`. The product is not used as a wrapped walk length.

On the path where the product fits, a null `base` is `__prism_check` with class `PTR-NULL-DEREF` and message `qsort() called with unchecked pointer parameter base`, then `return` with no load. A null `compar` is `__prism_check` with class `FUNC-CONTRACT` and message `qsort() called with unchecked pointer parameter compar`, then `return` with no call. After those checks the bytes stay as they were. The model does not swap elements into an invented order and does not claim the array is sorted.

The comment above `qsort` says qsort is not PROVED and not CLEAN. This is a partial model of the preconditions, not a proof and not a fuzzer `CLEAN` result.

## bsearch

`nmemb == 0` or `size == 0` returns NULL and does not read.

A wrapping `nmemb * size` is `__prism_check` with class `MEM-OOB-READ` and message `bsearch nmemb * size overflows`, then NULL. That arm does not walk a wrapped length.

When the count and the element size are positive and the product fits, a null `key` or a null `base` is `__prism_check` with class `PTR-NULL-DEREF`, and a null `compar` is `__prism_check` with class `FUNC-CONTRACT`. Each of those returns NULL and does not read. A miss returns NULL. The model does not allocate or return a pointer that is not an element it found. It does not read past `nmemb` elements: it does not walk the array.

The comment above `bsearch` says a miss returns NULL, that the model does not read past `nmemb` elements, and that bsearch is not PROVED and not CLEAN. Returning NULL does not prove the key is absent: the model never walks the elements, so it has no hit to report.

## Tests

`tests/cpp/test_qsort_null.cpp` reads `stdlib.cpp` from `PRISM_SOURCE_DIR`. It checks the zero-count returns, the null checks and their classes, the overflow arms, that neither function reads or calls `compar`, that `qsort` does not claim a sorted array, that every `bsearch` return is NULL, and that the pre-existing function texts are still in the file.

`CMakeLists.txt` lists `tests/cpp/test_qsort_null.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still expects every function defined in the libc models to be called from a contract harness. This slice does not add that call. The missing harness is not PROVED and not CLEAN. A missing tool on a later run is `NOTRUN`, not a clean result.

`prism_tests` was not executed. This slice does not configure or build.
