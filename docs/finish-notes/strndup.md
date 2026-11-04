# strndup

Branch `finish/libc-strndup`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`strndup` is a new function in `src/prism/pir/models/libc/string.cpp`, after `strdup` and still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`, and `strdup` are the same bytes as before.

A null `s` is `__prism_check` with class `STR-NULL-ARG`. The message is `strndup() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not call `strlen`, does not allocate, and does not read `s`.

`n == 0` does not read `s`. It allocates one byte, stores a NUL, and returns that pointer. If that allocation fails, the function returns a null pointer.

Otherwise the copy is `min(n, strlen(s))` bytes and a trailing NUL. The scan is `while (len < n && s[len])`, so it does not read `s[i]` for `i >= n` and it does not read past the terminator. The bytes are stored with a loop, then `p[len] = 0`. Allocation failure returns a null pointer. This is not a proof of the copy.

The comment above the function says this is not PROVED and not CLEAN.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `strdup_true` calls `strndup` after `strdup` and frees the pointer when allocation succeeds. That call does not assert a specific string. `string_contracts.yml` was not given a `strndup` task. `strdup_true` stays true. `strdup_null_false` is unchanged.

`tests/cpp/test_strndup.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null return before allocation, the `n == 0` arm, the bounded scan, the unchanged `strdup` body, the disclaimer, the include, the call, and that the yml has no `strndup` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strndup.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 6, because `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, and `strndup` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
