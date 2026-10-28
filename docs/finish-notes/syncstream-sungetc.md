# syncstream sungetc

Branch `finish/cxx-syncstream-sungetc`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`sungetc` is declared on the existing header
`src/prism/pir/models/cxx/syncstream`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_SYNCSTREAM`. `#pragma GCC system_header`.
Namespace `std`. The only include is `<cstddef>`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof.

## What sungetc does

`basic_syncbuf` stores a caller `char*`, a `size_t` size, and one `size_t`
offset. Nothing is allocated. `sputc`, `sgetc`, and `sungetc` share that
offset.

`sungetc` returns `int`. When the offset is greater than 0 it decrements the
offset by one, loads `static_cast<unsigned char>(_M_buf[_M_off])` at the new
offset, and returns that value as `int` (0 through 255). A stored 0 is that
byte. A stored `0xFF` is 255. The load is after the decrement, so an offset
that was already the size is not read.

When the offset is 0, `sungetc` calls `__prism_check` with class
`MEM-OOB-READ` and message
`basic_syncbuf::sungetc would read before the caller buffer`. The decrement
and the load sit under `if (__back)`, after the check. That path does not
decrement and does not read before the buffer. `return -1` is after the
check. -1 is the end signal.

A null pointer with a non-zero size is still the constructor
`__prism_check` (`FUNC-CONTRACT`), then
`if (!(__buf != nullptr || __n == 0)) _M_size = 0`. The size is 0 and the
offset is 0, so `sungetc` takes the offset-0 path and does not read that
pointer. A null pointer with size 0 is the same path.

## What stayed

`sputc` is unchanged. The guard is still
`const int __ok = (_M_off < _M_size) ? 1 : 0;`. The check is still
`__prism_check` with class `MEM-OOB-WRITE` and message
`basic_syncbuf::sputc would write past the caller buffer`. The store and
the advance stay under `if (__ok)`. A write at the size does not store and
does not wrap the offset.

`sgetc` is unchanged. The guard is still
`const int __in = (_M_off < _M_size) ? 1 : 0;`. The check is still
`__prism_check` with class `MEM-OOB-READ` and message
`basic_syncbuf::sgetc would read past the caller buffer`. The load sits
under `if (__in)`. A read at the size does not read the pointer.
`return -1` is the end signal after that check.

The constructor check is still
`__prism_check((__buf != nullptr || __n == 0) ? 1 : 0, "FUNC-CONTRACT",`.

`emit` and the `syncstream` class are not declared. A unit that names one
fails to compile against this header. They are not stubbed. A flush that
returns success without a stream would be a fake write.

`<span>`, `<sstream>`, `<string>`, `<ostream>`, and `<mutex>` are not
included.

## Wiring

`PRISM_PIR_MODELS`, `_prism_test_sources`, and the conformance header-name
array were not edited. `syncstream` was already on each of them.
`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` were not edited. A unit
whose lowered text contains `syncbuf` is still `NEEDS-HARNESS`
(`C++ syncbuf unencoded`). This slice does not claim the tasks are proved.

## Tests

`tests/cpp/test_cxx_syncstream.cpp` reads the header. The `sputc` text is
the span from `void sputc(` to `int sgetc(`, so the write check, the store,
and the single `++_M_off` stay on `sputc`. `sgetc` is the span from
`int sgetc(` to `int sungetc(`, so its `MEM-OOB-READ` check, its one
`_M_buf[`, and its `return -1` stay on `sgetc`. `sungetc` is the span from
`int sungetc(` to `private:`. The `MEM-OOB-READ` check is before
`--_M_off`, and that decrement is before
`static_cast<unsigned char>(_M_buf[_M_off])`. That load is the only
`_M_buf[` in `sungetc`. `return -1` is after `return static_cast<int>(__ch)`.

`tests/cxx_models/syncstream_trace.cpp` is the driver. Checked with
clang++ 18.1.3, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -pthread
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, and
`-isystem` on the model directory. Stdout with no argv was:

```
byte 65
kept 65
```

`argv` `get` printed:

```
got 0
got 255
got 65
```

`argv` `back` is three in-range `sgetc` calls, then `sungetc`. Stdout was:

```
back 65
back 255
back 0
```

`argv` `step` writes one byte, then `sungetc` prints `step 65`, then the
next `sungetc` is
`CHECK MEM-OOB-READ: basic_syncbuf::sungetc would read before the caller buffer`
and does not print `under`. `argv` `before` is `sungetc` while the offset
is still 0 on a 1-byte buffer. The same check fires and it does not print
`before`. `argv` `unnull0` is a null pointer with size 0. The same check
fires and it does not print `unread`. `argv` `unforced` continues after
checks: a null pointer with size 1 prints the constructor `FUNC-CONTRACT`
check, then the `sungetc` `MEM-OOB-READ` check, then `unforced -1`.
AddressSanitizer did not report a load. `argv` `stay` is `sungetc` at
offset 0, then `sputc('A')`. Stdout was `stayed -1 65`. Stderr was only the
`sungetc` check. The offset was not decremented, so the write landed in the
caller buffer and was not `MEM-OOB-WRITE`.

`argv` `full`, `wrap`, and `null` are the existing `sputc` and constructor
checks. `argv` `empty`, `null0`, `past`, and `forced` are the existing
`sgetc` checks.

`b.emit()` and `std::syncstream` fail to compile against this header
(`no member named 'emit'` / `no member named 'syncstream'`).

`-M` names `src/prism/pir/models/cxx/syncstream`.
