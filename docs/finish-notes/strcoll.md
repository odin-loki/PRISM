# strcoll

Branch `finish/libc-strcoll`, base `9aa63c50e8498f5b3a62136e9babec338c406d30`.

`strcoll` is a new function in `src/prism/pir/models/libc/string.cpp`, after `strncasecmp` and before `strchr`, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`, `memccpy`, `strdup`, and `strndup` are the same bytes as before.

A null `a` or a null `b` is `__prism_check` with class `STR-NULL-ARG`. The messages are `strcoll() called with unchecked pointer parameter a` and `strcoll() called with unchecked pointer parameter b`. Each check is followed by `return 0`. Those paths do not read either string.

Otherwise the function reads one unsigned byte from each string at the current index and compares those values. This is not a locale collation and it does not call `setlocale`. The scan returns at the first difference, or at the first NUL when the bytes are equal. The index advances only after that pair is equal and not NUL, so the stopping byte is read and the next byte is not. A difference returns `1` or `-1`. Equal strings, including the shared NUL, return `0`. This is not a proof of the comparison.

The comment above the function says this is not PROVED and not CLEAN.

`strndup` of a `SIZE_MAX` copy length is still `MEM-OOB-WRITE` and `__builtin_trap` before `malloc`.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `strcmp_true` calls `strcoll` after the `strcasecmp` call and discards the result. That call does not assert a locale order. `string_contracts.yml` was not given a `strcoll` task and was not given a locale task. `strcmp_true` stays true. `strcmp_signed_false` is unchanged. `strncmp_true` still calls `strncasecmp` and does not call `strcoll`.

`tests/cpp/test_strcoll.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null returns before any read, the unsigned-byte comparison, the stop before the next byte, the unchanged `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `memccpy`, and `strndup` bodies, the disclaimer, the include, the call, and that the yml has no `strcoll` key and no `locale` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strcoll.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_strncasecmp.cpp`, `tests/cpp/test_strcasecmp.cpp`, `tests/cpp/test_memccpy.cpp`, `tests/cpp/test_strndup.cpp`, `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 10, and their `PROVED` and `CLEAN` counts are 10, because `strcoll`, `strncasecmp`, `strcasecmp`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, `strndup`, and `memccpy` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
