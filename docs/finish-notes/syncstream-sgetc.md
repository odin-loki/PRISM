# syncstream sgetc

Branch `finish/cxx-syncstream-sgetc`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`sgetc` is declared on the existing header
`src/prism/pir/models/cxx/syncstream`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_SYNCSTREAM`. `#pragma GCC system_header`.
Namespace `std`. The only include is `<cstddef>`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof.

## What sgetc does

`basic_syncbuf` stores a caller `char*`, a `size_t` size, and one `size_t`
offset. Nothing is allocated. `sputc` and `sgetc` share that offset.

`sgetc` returns `int`. When the offset is less than the size it loads
`static_cast<unsigned char>(_M_buf[_M_off])`, advances the offset by one,
and returns that value as `int` (0 through 255). A stored 0 is that byte.
A stored `0xFF` is 255.

When the offset is already the size, including a size of 0, `sgetc` calls
`__prism_check` with class `MEM-OOB-READ` and message
`basic_syncbuf::sgetc would read past the caller buffer`. The load sits
under `if (__in)`, after the check. That path does not read the pointer
and does not return a character from the buffer. `return -1` is after the
check. -1 is the end signal.

A null pointer with a non-zero size is still the constructor
`__prism_check` (`FUNC-CONTRACT`), then
`if (!(__buf != nullptr || __n == 0)) _M_size = 0`. The size is 0, so
`sgetc` takes the end path and does not read that pointer. A null pointer
with size 0 is the same `sgetc` path: the offset is already the size.

## What stayed

`sputc` is unchanged. The guard is still
`const int __ok = (_M_off < _M_size) ? 1 : 0;`. The check is still
`__prism_check` with class `MEM-OOB-WRITE` and message
`basic_syncbuf::sputc would write past the caller buffer`. The store and
the advance stay under `if (__ok)`. A write at the size does not store and
does not wrap the offset.

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
`int sgetc(` to `private:`. The `MEM-OOB-READ` check is before
`static_cast<unsigned char>(_M_buf[_M_off])`. That load is the only
`_M_buf[` in `sgetc`. The advance is after the load, and `return -1` is
after `return static_cast<int>(__ch)`.

`tests/cxx_models/syncstream_trace.cpp` is the driver. Checked with
clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS -pthread
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

`argv` `empty` is `sgetc` on a size-0 buffer. The model aborts with
`CHECK MEM-OOB-READ: basic_syncbuf::sgetc would read past the caller buffer`
and does not print `got`. `argv` `null0` is a null pointer with size 0.
The same check fires and it does not print `read`. `argv` `past` reads the
one byte in a 1-byte buffer (`once 65`), then the next `sgetc` is that
check and it does not print `past`. `argv` `forced` continues after
checks: a null pointer with size 1 prints the constructor `FUNC-CONTRACT`
check, then the `MEM-OOB-READ` check, then `forced -1`. AddressSanitizer
did not report a load. `argv` `full`, `wrap`, and `null` are the existing
`sputc` and constructor checks.

`b.emit()` and `std::syncstream` fail to compile against this header
(`no member named 'emit'` / `no type named 'syncstream'`).

`-M` names `src/prism/pir/models/cxx/syncstream`.
