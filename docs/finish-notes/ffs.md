# ffs and ffsl

Branch `finish/libc-ffs`, base `079b279295e732b434335ce5791f4ba54a4b8f9e`.

`src/prism/pir/models/libc/strings_compat.cpp` models `ffs` and `ffsl` only. `ffs(0)` is 0. `ffs(1)` is 1. `ffs(2)` is 2. `ffs(8)` is 4. The result is one plus the index of the least set bit, and it is never a wrapped negative. `ffsl` follows the same rule on `long`.

The argument is converted to unsigned (`unsigned int` for `ffs`, `unsigned long` for `ffsl`) before the scan. The scan shifts that unsigned value until the low bit is set. It does not negate the argument, so the most negative value is not a wrapped negation. The shift count `bit` stays below 64, and the function returns `bit + 1`. That sum is in 1..64 for a nonzero value, and 0 for a zero value. The file comment states that this is not PROVED and not CLEAN.

`strcasecmp` is not declared. Locale functions are not declared. `ffsll` is not declared.

`stdlib.cpp`, `string.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes this file, and that included source is what the libc-contract task analyses.

## Tests

`testdata/conformance/libc-models/strings_compat_contracts.cpp` contains the line `#include "../../../src/prism/pir/models/libc/strings_compat.cpp"` and calls `ffs(` and `ffsl(`. `ffs_least_true` asserts `ffs(0) == 0`, `ffs(1) == 1`, `ffs(2) == 2`, and `ffs(8) == 4`, and the same four facts for `ffsl`. `ffs_zero_false` asserts `ffs(0) == 1`. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`.

`strings_compat_contracts.yml` has `format_version`, `input_files`, `language: C`, and `property: libc-contract`. Its `expected` map has `ffs_least_true: true` and `ffs_zero_false: false`. The false function's `expect_class` is `FUNC-CONTRACT`.

`tests/cpp/test_ffs.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the two function bodies, the disclaimer, the unsigned scan, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` lists `tests/cpp/test_ffs.cpp` as one new line in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model `.cpp` to be included by a harness and every defined function to be called. This harness is that include and those calls.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
