# ctype slice

Branch `finish/libc-ctype`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`src/prism/pir/models/libc/ctype.cpp` models eight `<ctype.h>` functions for the C locale ASCII set: `isdigit`, `isspace`, `isalpha`, `isalnum`, `islower`, `isupper`, `tolower`, and `toupper`.

EOF is -1. Every `is*` function returns 0 for EOF. Every `to*` function returns EOF for EOF. A value that is not EOF and not in the ranges below is not given a class: `is*` returns 0 and `to*` returns that same value.

`isdigit` is nonzero only for `'0'..'9'`. A letter is not a digit. `isalpha` is nonzero only for `'A'..'Z'` and `'a'..'z'`. `islower` is nonzero only for `'a'..'z'`. `isupper` is nonzero only for `'A'..'Z'`. `isalnum` is nonzero only when `isdigit` or `isalpha` is nonzero.

`isspace` is nonzero only for the six C locale standard white-space characters: space, horizontal tab, newline, vertical tab, form feed, and carriage return.

`tolower('A')` is `'a'`, and `'Z'` maps to `'z'`, because those ASCII ranges are contiguous. `tolower` of a non-letter other than EOF returns that same character. `toupper` is the symmetric map: `'a'` becomes `'A'`, and a non-letter other than EOF is unchanged.

The file comment states that this is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN.

`stdlib.cpp`, `string.cpp`, and `stdio.cpp` are unchanged. No C++ header was added. `PRISM_PIR_MODELS` is unchanged. The contract harness includes this file, and that included source is what the libc-contract task analyses.

## Tests

`testdata/conformance/libc-models/ctype_contracts.cpp` contains the line `#include "../../../src/prism/pir/models/libc/ctype.cpp"` and calls each of the eight functions. A `_true` function asserts the ASCII facts, including `isdigit('a') == 0`, `tolower('A') == 'a'`, and `toupper('a') == 'A'`. A `_false` function asserts a wrong fact the model rejects. `isdigit_letter_false` asserts `isdigit('a') != 0`. `tolower_digit_false` asserts `tolower('0') == 'a'`. `ctype_contracts.yml` has `format_version`, `input_files`, `language: C`, and `property: libc-contract`. Its `expected` map has true functions and false functions. Each false function has `expect_class` `FUNC-CONTRACT`, the class of a reached assert.

`tests/cpp/test_ctype.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the eight function bodies, the disclaimer, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_ctype.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model `.cpp` to be included by a harness and every defined function to be called. This harness is that include and those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
