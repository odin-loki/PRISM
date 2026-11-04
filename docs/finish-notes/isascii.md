# isascii

Branch `finish/libc-isascii`, base `63466ba8cbd95157f6dff251c7c8b0475639c4a8`.

`src/prism/pir/models/libc/ctype.cpp` adds `isascii` for the C locale ASCII set. `isdigit`, `isspace`, `isblank`, `isalpha`, `isalnum`, `isupper`, `islower`, `ispunct`, `isprint`, `isgraph`, `iscntrl`, `isxdigit`, `tolower`, and `toupper` are unchanged.

EOF is -1. `isascii` returns nonzero for values 0 through 127. It returns 0 for EOF and for every other value. The body compares the `int` argument with that range. It does not read a string.

The file comment states that this is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN. No other locale is modeled.

`time.cpp`, `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `strings_compat.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged. The contract harness includes this file, and that included source is what the libc-contract task analyses.

## Tests

`testdata/conformance/libc-models/ctype_contracts.cpp` still contains the line `#include "../../../src/prism/pir/models/libc/ctype.cpp"` and still calls the earlier functions. It also calls `isascii(`. `isascii_true` asserts that 0, 127, and `'A'` are nonzero, that 128 and EOF are 0, and that a value is nonzero only when it is in 0 through 127. `isascii_nul_false` asserts `isascii(0) == 0`. That assert is wrong: 0 is inside 0 through 127. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`. There is no yml task that expects a value beyond 0 through 127 to be in the class.

`ctype_contracts.yml` keeps `format_version`, `input_files`, `language: C`, and `property: libc-contract`. Its `expected` map adds `isascii_true: true` and `isascii_nul_false: false`. The false task has `expect_class` `FUNC-CONTRACT`.

`tests/cpp/test_ctype.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the earlier bodies, the disclaimer, the include, and the yml shape, and it checks the `isascii` body and the `isascii(` calls. It does not compile the model. `toascii` stays absent. `CMakeLists.txt` already lists `tests/cpp/test_ctype.cpp` and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model `.cpp` to be included by a harness and every defined function to be called. This harness is that include and the `isascii(` call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
