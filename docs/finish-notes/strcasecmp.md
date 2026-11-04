# strcasecmp

Branch `finish/libc-strcasecmp`, base `f636171290aae9a1711c3a1e459bc51722bdbc4f`.

`strcasecmp` is a new function in `src/prism/pir/models/libc/string.cpp`, after `strncmp` and before `strchr`, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`, `memccpy`, `strdup`, and `strndup` are the same bytes as before.

A null `a` or a null `b` is `__prism_check` with class `STR-NULL-ARG`. The messages are `strcasecmp() called with unchecked pointer parameter a` and `strcasecmp() called with unchecked pointer parameter b`. Each check is followed by `return 0`. Those paths do not read either string.

Otherwise the function reads one byte from each string at the current index, folds only ASCII `A`-`Z` into `a`-`z`, and compares those unsigned values. Bytes outside ASCII `A`-`Z` are left as they are. This is not a locale comparison. The scan returns at the first folded difference, or at the first NUL when the folded bytes are equal. The index advances only after that pair is equal and not NUL, so the stopping byte is read and the next byte is not. A difference returns `1` or `-1`. Equal strings, including the shared NUL, return `0`. This is not a proof of the comparison.

The comment above the function says this is not PROVED and not CLEAN.

`strndup` of a `SIZE_MAX` copy length is still `MEM-OOB-WRITE` and `__builtin_trap` before `malloc`.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `strcmp_true` calls `strcasecmp` after the `strcmp` asserts and discards the result. That call does not assert a specific comparison. `string_contracts.yml` was not given a `strcasecmp` task. `strcmp_true` stays true. `strcmp_signed_false` is unchanged.

`tests/cpp/test_strcasecmp.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null returns before any read, the ASCII fold, the stop before the next byte, the unchanged `strcmp`, `strncmp`, and `strndup` bodies, the disclaimer, the include, the call, and that the yml has no `strcasecmp` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strcasecmp.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_memccpy.cpp`, `tests/cpp/test_strndup.cpp`, `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 8, because `strcasecmp`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, `strndup`, and `memccpy` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
