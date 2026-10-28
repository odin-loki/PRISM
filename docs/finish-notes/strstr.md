# strstr and strpbrk

Branch `finish/libc-strstr`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`strstr` and `strpbrk` are new functions in `src/prism/pir/models/libc/string.cpp`, after `strcspn` and before `memcpy`, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strspn`, `strcspn`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, and `strdup` are the same bytes as before.

A null `s` or a null needle is `__prism_check` with class `STR-NULL-ARG`. The message names the function and the parameter: `strstr() called with unchecked pointer parameter s`, and the same shape for `needle`. A null `s` or a null accept uses the same two messages for `strpbrk`, with `accept` in place of `needle`. The check is followed by `return 0`. That path does not read either string.

`strstr` returns a pointer to the first match of `needle` in `s`, or a null pointer when there is none. The scan compares bytes with `uc`, so the test is the unsigned value. An empty needle returns `s` and does not scan `s`: the only load on that path is `needle`'s terminator. A match returns `s + i` at the first index whose following bytes equal `needle`, and that return does not read past `needle`'s terminator. When the remainder of `s` ends before `needle` does, the function returns a null pointer at `s`'s terminator and does not read the next byte. It does not read past the terminator of either string.

`strpbrk` returns a pointer to the first byte of `s` that occurs in `accept`, or a null pointer when none does. An empty accept returns a null pointer, and that return does not walk `s`. The scan stops at the terminator of `s` and at the terminator of `accept`. It does not read the byte after either terminator.

The comment above each function says this is not PROVED and not CLEAN.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new functions are in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. It now calls `strstr` and `strpbrk`. `strstr_true` asserts that an empty needle returns `s`, that `"bc"` in `"abc"` is the pointer one past the start, that `"abc"` against itself is the start, and that `"a"` in `"abc"` is the start. `strpbrk_true` asserts that an empty accept is a null pointer, that `"b"` in `"abc"` is one past the start, that `"abc"` against itself is the start, and that `"z"` in `"abc"` is a null pointer. `strstr_miss_false` asserts that `"bc"` in `"abc"` is a null pointer. `strpbrk_miss_false` asserts that `"b"` in `"abc"` is a null pointer. Those two asserts are wrong. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`.

`string_contracts.yml` keeps `format_version`, `input_files: string_contracts.cpp`, `language: C`, and `property: libc-contract`. `strstr_true` and `strpbrk_true` are true. `strstr_miss_false` and `strpbrk_miss_false` are false, and each `expect_class` is `FUNC-CONTRACT`.

`tests/cpp/test_strstr.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the two function bodies, the disclaimer, the null returns before any read, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strstr.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_strspn.cpp` still lists its own source once. Its file-wide count of `This is not PROVED and not CLEAN.` is 4, because `strspn`, `strcspn`, `strstr`, and `strpbrk` each carry that sentence. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. These calls are those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
