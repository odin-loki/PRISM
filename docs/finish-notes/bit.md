# bit slice

Branch `finish/cxx-bit`, base `3e0ebbb46361d4d3c7557657eeeb590d2ae3aa19`.

`<bit>` is one extensionless file, `src/prism/pir/models/cxx/bit`. A directory of that name would make `#include <bit>` fail with EISDIR. The include guard is `_GLIBCXX_BIT`. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml records the contract the harness asks for. `expected` is the boolean `true`. This header is not a proof that a program is free of shift undefined behavior.

## What the model covers

`countl_zero(0)` and `countr_zero(0)` return `numeric_limits<T>::digits`. `countl_one` of an all-ones value is that same width: the complement is zero, and the zero path returns the digit count. `popcount` is the number of one bits. A zero argument takes that width path directly. It does not call `__builtin_clz(0)` or `__builtin_ctz(0)`.

`rotl` and `rotr` reduce the count modulo the width. A multiple of the width, including zero and a negative multiple, returns the value unchanged and does not shift by the width. Any other count moves every bit. A round trip returns the original value and keeps the same `popcount`.

`has_single_bit` is true only for a value with one bit set. `bit_ceil(0)` and `bit_ceil(1)` are 1. `bit_ceil` of a value whose smallest power of two does not fit in the type calls `__prism_check` with class `FUNC-CONTRACT` and the message `bit_ceil: smallest power of two is not representable`. That path does not return 0. In a constant expression the call is ill-formed. `bit_floor(0)` is 0. `endian` is `little`, `big`, and `native`.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and `lower_to_ir` lowers the translation unit again with libstdc++ (`extra.cxx_models`). They are not stubbed:

- `byteswap`
- `bit_cast`

No container header is included.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/bit`

so the existing embed writes `cxx/bit`. `_prism_test_sources` gained `tests/cpp/test_cxx_bit.cpp`. `tests/cpp/test_qa_conformance.cpp` lists `bit` in the header-name array. The skips for `iostream/prism_ostream.hpp` and `deque/deque` are unchanged. The names already in that array stay: `array`, `bitset`, `stack`, `queue`, `span`, `optional`, `priority_queue`, `forward_list`, `initializer_list`.

`testdata/conformance/libc-models/bit_contracts.cpp` is the `#include <bit>` that case searches for. It is not a scored task. There is no false twin.

## Tests

`tests/cxx_models/bit_trace.cpp` includes `<bit>`. Compiled twice with clang++ 18.1.3 (`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-sanitize-recover=all -O1`), once against libstdc++ and once with `-isystem` on the model directory. Stdout matched, including `unsigned clz0 32 digits 32`, `unsigned popA 2`, `counts 1 1 1`, and `round 1 1 1`. `countl_zero(0)` was 8, 32, and 64 for `uint8_t`, `unsigned`, and `uint64_t`.

`bit_ceil(~0u)` aborted with `CHECK FUNC-CONTRACT: bit_ceil: smallest power of two is not representable` and printed no result. `constexpr unsigned too = std::bit_ceil(0x80000001u)` is not a constant expression. Constant `bit_ceil(0x80000000u)` is that value, and constant `bit_ceil(uint8_t{128})` is 128.

`tests/cpp/test_cxx_bit.cpp` checks that `countl_zero(0)` is the width, that `popcount(0xA)` is 2, that the header is a file, and that the header text does not contain `__builtin_clz(0)`. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_bit_true.cpp` calls `countl_zero` on 0 and `popcount` on `0xA` and checks the counts (`memsafety` true). There is no false task.

## Still in front of the model

`src/prism/bmc.cpp` still returns `C++ bit_ceil unencoded` when the source contains `std::popcount` (and when it contains `bit_ceil`, `bit_floor`, or `has_single_bit`). It returns `C++ std::countl_zero unencoded` when the source contains `std::countl_zero`. The popcount check is earlier, so `cxx_bit_true` hits the bit_ceil gate. `src/prism/bmc_unenc.inc` and `src/prism/bmc_encoder.inc` have the same gates. Those files were not edited. Until they go, `cxx_bit_true` does not reach this header.
