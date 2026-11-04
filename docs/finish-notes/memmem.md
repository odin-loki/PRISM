# memmem

Branch `port/wip-d`, base `main` at `0a9571725`. First written against `4ed2c6c39ced97f48b018465acfc7ce6948b3bab`.

`memmem` is a new function in `src/prism/pir/models/libc/string.cpp`, after `memccpy` and before the `malloc` declaration, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strcoll`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memset_explicit`, `memcmp`, `memchr`, `memrchr`, `memccpy`, `strdup`, and `strndup` are the same bytes as before.

A needle length of 0 returns the haystack pointer and does not read, even when the haystack pointer is null. That return is the first statement.

A null haystack with a haystack length greater than 0 is `__prism_check` with class `PTR-NULL-DEREF` and message `memmem null haystack`, then `__builtin_trap`, before any read. A null needle with a needle length greater than 0 is `__prism_check` with class `PTR-NULL-DEREF` and message `memmem null needle`, then `__builtin_trap`, before any read. The haystack check is first.

Otherwise the search stays inside the two lengths. A needle longer than the haystack returns a null pointer and does not index either object. A match returns a pointer at the first window whose bytes equal the needle. The window index stops at `haystacklen - needlelen`: that last window is read, and the index is not incremented past it. A byte of the needle is read only while its index is less than `needlelen`. A miss returns a null pointer. This is not a proof of the search.

The comment above the function says this is not PROVED and not CLEAN.

`strndup` of a `SIZE_MAX` copy length is still `MEM-OOB-WRITE` and `__builtin_trap` before `malloc`. `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strcoll`, and `memccpy` are unchanged.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. `memchr_true` calls `memmem` after the `memchr` asserts and discards the pointer. The needle length is 0. That call does not assert a found offset. `string_contracts.yml` was not given a `memmem` task. `memchr_true` stays true. `memchr_oob_false` is unchanged and does not call `memmem`.

`tests/cpp/test_memmem.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the zero-length return before any read, the two null traps before any index, the bounded search, the miss, the unchanged `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strcoll`, `memccpy`, and `strndup` bodies, the disclaimer, the include, the call, and that the yml has no `memmem` key. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_memmem.cpp` as one new line in `_prism_test_sources`. No existing test source line was removed. `tests/cpp/test_strcoll.cpp`, `tests/cpp/test_strncasecmp.cpp`, `tests/cpp/test_strcasecmp.cpp`, `tests/cpp/test_memccpy.cpp`, `tests/cpp/test_strndup.cpp`, `tests/cpp/test_memrchr.cpp`, `tests/cpp/test_strstr.cpp`, and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 11, and their `PROVED` and `CLEAN` counts are 11, because `memmem`, `strcoll`, `strncasecmp`, `strcasecmp`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memrchr`, `strndup`, and `memccpy` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. This call is that call.

Built with the pinned clang 23 in WSL and run with `prism_tests`: each touched libc test file, `-tc="*libc*,*conformance*"`, and `-tc="*naming*,*env*"`.
