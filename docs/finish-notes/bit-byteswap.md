# bit byteswap

Branch `finish/cxx-bit-byteswap`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::byteswap` is declared on the existing header `src/prism/pir/models/cxx/bit`.
The header is still one extensionless file. A directory of that name would make
`#include <bit>` fail with EISDIR. `countl_zero`, `countr_zero`, `popcount`,
`rotl`, `rotr`, `bit_ceil`, and `bit_floor` are unchanged. `countl_zero(0)`
still returns `numeric_limits<T>::digits` on the zero path. `bit_ceil` of a
value whose smallest power of two does not fit still calls `__prism_check`
with class `FUNC-CONTRACT` and the message
`bit_ceil: smallest power of two is not representable`, then `__builtin_trap`.
That path does not return 0.

PIR was not run. The solver was not run. This header is not a proof that a
program is free of shift undefined behavior.

## What byteswap does

`std::byteswap` is a function template on an unsigned integer. It reverses
the bytes of that value. `__cpp_lib_byteswap` is `202110L`.

- `byteswap(0)` is 0. The zero path returns before any shift.
- A one-byte unsigned value is returned unchanged. That `if constexpr`
  branch does not shift, and the wider loop is not instantiated for it.
- A wider value is rebuilt from its low byte. The shift distance is one
  byte of that same type (`numeric_limits<unsigned char>::digits`), and the
  loop bound is `sizeof(T)`. The body does not call `__builtin_bswap16`,
  `__builtin_bswap32`, `__builtin_bswap64`, or `__builtin_bswap128`.

`byteswap(uint32_t{0})` is 0. `byteswap(uint32_t{1})` is `0x01000000`.
`byteswap(uint16_t{0x1234})` is `0x3412`. `byteswap(uint32_t{0x12345678})`
is `0x78563412`. `byteswap(uint64_t{0x0123456789ABCDEF})` is
`0xEFCDAB8967452301`. `byteswap(uint8_t{0xAB})` is `0xAB`. A round trip
returns the original value.

A signed argument fails `static_assert(is_unsigned_v<T>)`, so that call
does not compile against this header and the translation unit falls back
to libstdc++.

## What still falls through to libstdc++

`bit_cast` is not declared. A use fails to compile against this header and
the pir stage lowers that translation unit again with libstdc++
(`extra.cxx_models`). It is not stubbed.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_bit.cpp`. The
conformance header array already names `bit`. `src/prism/bmc.cpp` was not
edited. A source that contains `byteswap` still takes the
`C++ byteswap unencoded` path there.

## Tests

`tests/cpp/test_cxx_bit.cpp` reads the header. It checks that
`constexpr _Tp byteswap(_Tp __x)` is present, that the sentence
`byteswap and bit_cast are not declared` is gone, that the text
`bit_cast is not declared` remains, and that the header contains no
`__builtin_bswap` and no `__builtin_bit_cast`. The `countl_zero(0)` return
of `_Nd` and the `bit_ceil` contract message are still in the file.

`tests/cxx_models/bit_trace.cpp` includes `<bit>`. Compiled twice with
clang++ 18.1.3 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`), once against
libstdc++ and once with `-isystem` on the model directory. Stdout matched,
including `unsigned clz0 32 digits 32`, `unsigned popA 2`, `counts 1 1 1`,
`round 1 1 1`, and:

```
swap0 0
swap8 ab
swap16 3412
swap32 78563412
swap1 1000000
swap64 efcdab8967452301
swaprt 1 1 1 1
```

A one-file use of `std::bit_cast` fails to compile against the model
(`no member named 'bit_cast' in namespace 'std'`) and compiles against
libstdc++. cmake, `prism_tests`, and pir were not run.
