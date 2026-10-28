# strncasecmp

Branch `finish/libc-strncasecmp`, base `64932ab4bc10c3392ce2a0043946cda480104467`.

`strncasecmp` is a new function in `src/prism/pir/models/libc/string.cpp`, after `strcasecmp` and before `strchr`, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strcasecmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`, `memccpy`, `strdup`, and `strndup` are the same bytes as before.

A null `a` or a null `b` is `__prism_check` with class `STR-NULL-ARG`. The messages are `strncasecmp() called with unchecked pointer parameter a` and `strncasecmp() called with unchecked pointer parameter b`. Each check is followed by `return 0`. Those paths do not read either string. `n == 0` then returns 0 and does not read.

Otherwise the function reads one byte from each string at the current index, folds only ASCII `A`-`Z` into `a`-`z`, and compares those unsigned values. Bytes outside ASCII `A`-`Z` are left as they are. This is not a locale comparison. The loop reads index `i` only while `i + 1 < n`. A folded difference or a NUL breaks that loop, and the same index is compared again. When the loop ends because `i + 1` is no longer below `n`, `i` is `n - 1`, and that byte is the last one read. The byte at index `n` is not read. A difference returns `1` or `-1`. Equal bytes through a shared NUL, or through `n` bytes, return `0`. This is not a proof of the comparison.

The comment above the function says this is not PROVED and not CLEAN.

`strndup` of a `SIZE_MAX` copy length is still `MEM-OOB-WRITE` and `__builtin_trap` before `malloc`.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `strncmp_true` calls `strncasecmp` after the `strncmp` asserts and discards the result. That call does not assert a specific comparison. `string_contracts.yml` was not given a `strncasecmp` task and was not given a locale task. `strncmp_true` stays true. `strncmp_prefix_false` is unchanged. `strcmp_true` still calls `strcasecmp` and does not call `strncasecmp`.

`tests/cpp/test_strncasecmp.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the null returns before any read, the zero-length return before any read, the ASCII fold, the stop before index `n`, the unchanged `strcmp`, `strncmp`, `strcasecmp`, `memccpy`, and `strndup` bodies, the disclaimer, the include, the call, and that the yml has no `strncasecmp` key and no `locale` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strncasecmp.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_strcasecmp.cpp`, `tests/cpp/test_memccpy.cpp`, `tests/cpp/test_strndup.cpp`, `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 9, because `strncasecmp`, `strcasecmp`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, `strndup`, and `memccpy` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
