# bit countl_one and countr_one

Branch `finish/cxx-bit-countl-one`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::countl_one` and `std::countr_one` stay on the existing header
`src/prism/pir/models/cxx/bit`. Each name was already one function template
for an unsigned type, the same shape `countl_zero` uses. The bodies were
replaced in place. There is no second overload of either name. The header
is still one extensionless file. A directory of that name would make
`#include <bit>` fail with EISDIR.

`countl_zero`, `countr_zero`, `popcount`, `rotl`, `rotr`, `byteswap`,
`bit_floor`, `bit_width`, `bit_ceil`, and `has_single_bit` are unchanged.
`countl_zero(0)` still returns `numeric_limits<T>::digits` on the zero path,
before any shift. That path does not call a count-leading builtin.
`bit_ceil` of a value whose smallest power of two does not fit still calls
`__prism_check` with class `FUNC-CONTRACT` and the message
`bit_ceil: smallest power of two is not representable`, then `__builtin_trap`.
That path does not return 0.

PIR was not run. The solver was not run. The result is not PROVED and not
CLEAN. This header is not a proof that a program is free of shift undefined
behavior.

## What countl_one and countr_one do

Both are `constexpr` function templates on an unsigned type
(`static_assert(is_unsigned_v<T>)`). The width is
`numeric_limits<T>::digits`. The header does not call `__builtin_clz` or
`__builtin_ctz`.

- `countl_one(0)` is 0. The zero path returns before any shift. It does not
  call a count-leading builtin on 0, and it does not call `countl_zero(0)`.
- A value whose high bit is set counts that run of leading ones. The index
  stays in `[0, digits)`. The first clear bit ends the run.
- An all-ones value returns the full width from `countl_one`.
- `countr_one(0)` is 0. The zero path returns before any shift.
- A value whose low bit is clear returns 0 and does not scan off the width.
- An all-ones value returns the full width from `countr_one`. The shift
  count in that loop stays below the width (`__i < _Nd`).

`countl_one(uint8_t{0})` is 0. `countl_one(uint8_t{0x80})` is 1.
`countl_one(uint8_t{0xF0})` is 4. `countl_one(uint8_t{0xFF})` is 8.
`countr_one(uint8_t{0})` is 0. `countr_one(uint8_t{2})` is 0.
`countr_one(uint8_t{0x80})` is 0. `countr_one(uint8_t{0x0F})` is 4.
`countr_one(uint8_t{0xFF})` is 8. The same pattern holds for `uint16_t`
(width 16), `uint32_t` and `unsigned` (width 32), and `uint64_t` (width 64):
zero is 0, the high bit alone is 1 leading one, the top four bits are 4,
all ones is the width, a clear low bit is 0 trailing ones, and the low
nibble `0x0F` is 4. `unsigned long` and `unsigned long long` use the same
template. Their zero, clear-low-bit, high-bit, and all-ones checks compare
against `numeric_limits` for that type.

A signed argument fails `static_assert(is_unsigned_v<T>)`, so that call
does not compile against this header and the translation unit falls back
to libstdc++.

## What still falls through to libstdc++

`bit_cast` is not declared. A use fails to compile against this header and
the pir stage lowers that translation unit again with libstdc++
(`extra.cxx_models`). It is not stubbed.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
No CMake source line was added or removed. `_prism_test_sources` already
lists `tests/cpp/test_cxx_bit.cpp`. The conformance header array already
names `bit`. `src/prism/bmc.cpp` was not edited. A source that contains
`std::countl_one` or `std::countr_one` still takes the
`C++ std::countl_zero unencoded` path there.

## Tests

`tests/cpp/test_cxx_bit.cpp` reads the header. It checks one
`constexpr int countl_one(` and one `constexpr int countr_one(`, that
`countl_zero` and `countr_zero` are still one template each, that
`if (__x == 0) return _Nd;` still occurs twice, that
`if (__x == 0) return 0;` occurs twice, that a clear low bit returns 0,
that the trailing-ones loop is bounded by `_Nd`, and that the header
contains `not PROVED and not CLEAN`. The header contains no
`__builtin_clz`, no `__builtin_ctz`, no `__builtin_bswap`, and no
`__builtin_bit_cast`. `bit_cast is not declared` remains, and `bit_cast(`
does not appear. The `bit_ceil` contract message is still in the file.

`tests/cxx_models/bit_trace.cpp` includes `<bit>`. Compiled twice with
clang++ 18.1.3 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -w`), once
against libstdc++ 14 and once with `-isystem` on the model directory.
`-H` names the model file first. Stdout matched, including
`unsigned clz0 32 digits 32`, `unsigned popA 2`, `counts 1 1 1`,
`round 1 1 1`, the byteswap lines, and:

```
ones u8 clo0 0 clohi 1 clorun 4 cloall 8 cro0 0 crolow 0 crohi 0 crorun 4 croall 8
ones u16 clo0 0 clohi 1 clorun 4 cloall 16 cro0 0 crolow 0 crohi 0 crorun 4 croall 16
ones u32 clo0 0 clohi 1 clorun 4 cloall 32 cro0 0 crolow 0 crohi 0 crorun 4 croall 32
ones unsigned clo0 0 clohi 1 clorun 4 cloall 32 cro0 0 crolow 0 crohi 0 crorun 4 croall 32
ones u64 clo0 0 clohi 1 clorun 4 cloall 64 cro0 0 crolow 0 crohi 0 crorun 4 croall 64
```

At `-O1` the model lowers `countl_one(0)` to a test and a return of 0, and
`countr_one` of a clear low bit to a test of that bit and a return of 0.
The generated text contains no `clz`, `ctz`, `lzcnt`, or `tzcnt`.
`countl_zero(0)` still returns 32 for `unsigned` before the bit loop.

A one-file use of `std::bit_cast` fails to compile against the model
(`no member named 'bit_cast' in namespace 'std'`) and compiles against
libstdc++. cmake, `prism_tests`, and pir were not run.
