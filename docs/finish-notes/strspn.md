# strspn and strcspn

Branch `finish/libc-strspn`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`strspn` and `strcspn` are new functions in `src/prism/pir/models/libc/string.cpp`, after `strrchr` and before `memcpy`, still inside `extern "C"`. `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, and `strdup` are the same bytes as before.

A null `s` or a null accept set is `__prism_check` with class `STR-NULL-ARG`. The message names the function and the parameter: `strspn() called with unchecked pointer parameter s`, and the same shape for `accept`, and the same two messages for `strcspn`. The check is followed by `return 0`. That path does not read either string.

`strspn` returns how many leading characters of `s` are in `accept`. The scan compares bytes with `uc`, so the test is the unsigned value, and it stops at the terminator of `s` and at the terminator of `accept`. It does not read the byte after either terminator. An empty `s` returns 0. An empty `accept` returns 0, and that return does not walk `s`.

`strcspn` returns how many leading characters of `s` are not in `accept`. An empty `accept` returns the length of `s`. That length is a walk of `s` that stops on `s`'s terminator and does not read past it, and it does not read past the terminator of `accept`. The first character of `s` that occurs in `accept` stops the scan; the function returns that index and does not read the next byte of `s`.

The comment above each function says this is not PROVED and not CLEAN.

`stdlib.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The new functions are in `string.cpp`, which that list already embeds.

## Tests

`testdata/conformance/libc-models/string_contracts.cpp` already includes `string.cpp`. It now calls `strspn` and `strcspn`. `strspn_true` asserts that an empty `s` and an empty accept are 0, that `"abc"` against `"ab"` is 2, and that `"abc"` against itself is 3. `strcspn_true` asserts that an empty accept is the length of `s`, that an empty `s` is 0, that `"abc"` against `"b"` is 1, and that `"abc"` against itself is 0. `strspn_absent_false` asserts that `"axa"` against `"a"` is 3. `strcspn_stop_false` asserts that `"abc"` against `"b"` is 3. Those two asserts are wrong. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`.

`string_contracts.yml` keeps `format_version`, `input_files: string_contracts.cpp`, `language: C`, and `property: libc-contract`. `strspn_true` and `strcspn_true` are true. `strspn_absent_false` and `strcspn_stop_false` are false, and each `expect_class` is `FUNC-CONTRACT`.

`tests/cpp/test_strspn.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the two function bodies, the disclaimer, the null returns before any read, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_strspn.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model function to be called from a harness. These calls are those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
