# ffsll

Branch `finish/libc-ffsll`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`src/prism/pir/models/libc/strings_compat.cpp` models `ffs`, `ffsl`, and `ffsll`. The bodies of `ffs` and `ffsl` are unchanged. `ffsll(0)` is 0. `ffsll(1)` is 1. `ffsll(2)` is 2. `ffsll(8)` is 4. The result is one plus the index of the least set bit. A value with only the high bit set returns 64. `bit + 1` is never a wrapped negative.

The argument is converted to `unsigned long long` before the scan. The scan shifts that unsigned value until the low bit is set. It does not negate the argument, so the most negative value is not a wrapped negation. The shift count `bit` stays below 64, and the function returns `bit + 1`. That sum is 64 when only the high bit is set, in 1..64 for a nonzero value, and 0 for a zero value. The file comment states that this is not PROVED and not CLEAN.

`strcasecmp` is not declared. Locale functions are not declared.

`stdlib.cpp`, `string.cpp`, `stdio.cpp`, `ctype.cpp`, and `time.cpp` were not edited. No C++ header was added. `PRISM_PIR_MODELS` is unchanged, so the embedded libc models are still `string.cpp`, `stdlib.cpp`, and `stdio.cpp`. The contract harness includes this file, and that included source is what the libc-contract task analyses.

## Tests

`testdata/conformance/libc-models/strings_compat_contracts.cpp` contains the line `#include "../../../src/prism/pir/models/libc/strings_compat.cpp"` and calls `ffsll(`. `ffs_least_true` asserts `ffsll(0LL) == 0`, `ffsll(1LL) == 1`, `ffsll(2LL) == 2`, and `ffsll(8LL) == 4`, and the same four facts for `ffs` and `ffsl`. `ffsll_zero_false` asserts `ffsll(0LL) == 1`. `ffs_zero_false` asserts `ffs(0) == 1`. A failed assert calls `__assert_fail` and is `FUNC-CONTRACT`.

`strings_compat_contracts.yml` has `format_version`, `input_files`, `language: C`, and `property: libc-contract`. Its `expected` map has `ffs_least_true: true`, `ffs_zero_false: false`, and `ffsll_zero_false: false`. Each false function's `expect_class` is `FUNC-CONTRACT`.

`tests/cpp/test_ffs.cpp` reads the model, the harness, and the yml as text from `PRISM_SOURCE_DIR`. It checks the `ffsll` body, the unchanged `ffs` and `ffsl` bodies, the disclaimer, the unsigned scan, the include, the calls, and the yml shape. It does not compile the model.

`CMakeLists.txt` already lists `tests/cpp/test_ffs.cpp` and was not edited. `tests/cpp/test_qa_conformance.cpp` was not edited. That case still requires every libc model `.cpp` to be included by a harness and every defined function to be called. This harness is that include and the `ffsll(` call.

cmake, ninja, and `prism_tests` were not run. This slice does not configure or build.
