# memccpy

Branch `finish/libc-memccpy`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`memccpy` is a new function in `src/prism/pir/models/libc/string.cpp`, after `memrchr` and before the `malloc` declaration, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`, `strdup`, and `strndup` are the same bytes as before.

`n == 0` returns a null pointer. That path does not read `d` or `s`. A null `d` or a null `s` with `n > 0` is `__prism_check` with class `STR-NULL-ARG`. The messages are `memccpy() called with unchecked pointer parameter d` and `memccpy() called with unchecked pointer parameter s`. Each check is followed by `return 0`. Those paths do not read or write either object.

Otherwise the copy stores source bytes into `d` until `(unsigned char)(c & 0xff)` has been stored, or `n` bytes have been stored, whichever comes first. The index stays in `0 .. n - 1`, so the copy does not read or write index `i` for `i >= n`. When `c` is copied, the return is a pointer to the next byte of `d`. When `c` is not found, the return is a null pointer. This is not a proof of the copy.

The comment above the function says this is not PROVED and not CLEAN.

`strndup` of a `SIZE_MAX` copy length is still `MEM-OOB-WRITE` and `__builtin_trap` before `malloc`.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `memcpy_true` calls `memccpy` after the `memcpy` asserts and discards the pointer. That call does not assert a specific copy. `string_contracts.yml` was not given a `memccpy` task. `memcpy_true` stays true. `memcpy_overlap_false` is unchanged.

`tests/cpp/test_memccpy.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null returns before any index, the `n == 0` arm, the bounded copy, the next-byte return, the unchanged `strndup` body, the disclaimer, the include, the call, and that the yml has no `memccpy` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_memccpy.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_strndup.cpp`, `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 7, because `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, `strndup`, and `memccpy` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
