# memrchr

Branch `finish/libc-memrchr`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`memrchr` is a new function in `src/prism/pir/models/libc/string.cpp`, after `memchr` and before the `malloc` declaration, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `strstr`, `strpbrk`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, and `strdup` are the same bytes as before.

`n == 0` returns a null pointer. That path does not read `s` and does not check `s`. A null `s` with `n > 0` is `__prism_check` with class `STR-NULL-ARG`. The message is `memrchr() called with unchecked pointer parameter s`. The check is followed by `return 0`. That path does not index `s`.

Otherwise the scan compares bytes as `unsigned char`, using `(unsigned char)(c & 0xff)`, the same conversion `memchr` uses. It starts at the last of the `n` bytes and walks backward. A match returns a pointer to that byte. No match returns a null pointer. The index stays in `0 .. n - 1`, so the scan reads only those `n` bytes.

The comment above the function says this is not PROVED and not CLEAN.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new function is in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. It now calls `memrchr`. `memrchr_true` asserts that a length of 0 is a null pointer, that the last `'a'` in the four bytes `a`, `b`, `a`, `c` is two past the start, that `'c'` is the last byte, and that `'z'` is a null pointer. It also checks a symbolic buffer: a hit is inside the `n` bytes, equals `(unsigned char)c`, and no later byte in that range equals `c`. `memrchr_first_false` asserts that the first `'a'` in those four bytes is the result. That assert is wrong. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`.

`string_contracts.yml` keeps `format_version`, `input_files: string_contracts.cpp`, `language: C`, and `property: libc-contract`. `memrchr_true` is true. `memrchr_first_false` is false, and its `expect_class` is `FUNC-CONTRACT`. Existing keys stay, including `memchr_true`, `memchr_oob_false`, and `memchr_oob_false`'s class `MEM-OOB-READ`.

`tests/cpp/test_memrchr.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the `memrchr` body, the unchanged `memchr` body, the disclaimer, the null return before any index, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_memrchr.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_strstr.cpp` and `tests/cpp/test_strspn.cpp` still list their own sources once. Their file-wide count of `This is not PROVED and not CLEAN.` is 5, because `strspn`, `strcspn`, `strstr`, `strpbrk`, and `memrchr` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. These calls are those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
