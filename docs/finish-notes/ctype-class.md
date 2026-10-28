# ctype class

Branch `finish/libc-ctype-class`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`src/prism/pir/models/libc/ctype.cpp` adds six `<ctype.h>` classifications for the C locale ASCII set: `isblank`, `isxdigit`, `isprint`, `isgraph`, `ispunct`, and `iscntrl`. `isdigit`, `isspace`, `isalpha`, `isalnum`, `islower`, `isupper`, `tolower`, and `toupper` are unchanged.

EOF is -1. Each new function returns 0 for EOF. A value outside that function's range returns 0.

`isblank` is nonzero only for space and horizontal tab. `isxdigit` is nonzero only for `'0'..'9'`, `'A'..'F'`, and `'a'..'f'`. `isprint` is nonzero only for `0x20` through `0x7E`. `isgraph` is nonzero only for `0x21` through `0x7E`. `ispunct` is nonzero only for a graphic character that is not a digit and not a letter. `iscntrl` is nonzero only for `0x00` through `0x1F` and `0x7F`.

The file comment states that this is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN. No other locale is modeled.

`time.cpp`, `stdlib.cpp`, `string.cpp`, `stdio.cpp`, and `strings_compat.cpp` were not edited. `PRISM_PIR_MODELS` is unchanged. The contract harness includes this file, and that included source is what the libc-contract task analyses.

## Tests

`testdata/conformance/libc-models/ctype_contracts.cpp` still contains the line `#include "../../../src/prism/pir/models/libc/ctype.cpp"` and still calls the original eight functions. It also calls `isblank`, `isxdigit`, `isprint`, `isgraph`, `ispunct`, and `iscntrl`. Each new name has a `_true` function and a `_false` function. A `_true` function asserts a value inside the range and EOF. A `_false` function asserts a wrong fact the model rejects: a newline is blank, `'G'` is a hexadecimal digit, DEL is printable, space is graphic, a letter is punctuation, or space is a control character.

`ctype_contracts.yml` keeps `format_version`, `input_files`, `language: C`, and `property: libc-contract`. Its `expected` map has a true task and a false task for each new function. Each false task has `expect_class` `FUNC-CONTRACT`, the class of a reached assert.

`tests/cpp/test_ctype.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the original eight bodies, the disclaimer, the include, and the yml shape, and it checks the six new bodies and the new calls. It does not compile the model. `CMakeLists.txt` already lists `tests/cpp/test_ctype.cpp`. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model `.cpp` to be included by a harness and every defined function to be called. This harness is that include and those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
