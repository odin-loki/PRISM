# syncstream sputn

Branch `finish/cxx-syncstream-sputn`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`sputn` is declared on the existing header
`src/prism/pir/models/cxx/syncstream`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_SYNCSTREAM`. `#pragma GCC system_header`.
Namespace `std`. The only include is `<cstddef>`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof. This slice does not claim the tasks
are proved. It does not claim a clean result.

## What sputn does

`basic_syncbuf` stores a caller `char*`, a `size_t` size, and one `size_t`
offset. Nothing is allocated. `sputc`, `sputn`, `sgetc`, and `sungetc` share
that offset.

`sputn` returns `ptrdiff_t`. The count is a `ptrdiff_t`.

A negative count calls `__prism_check` with class `FUNC-CONTRACT` and
message `basic_syncbuf::sputn count is negative`, then `__builtin_trap`.
That path does not write. It does not return a count.

A count of 0 returns 0. That return is before any load of the source, so a
null source is not read.

A positive count writes only while the offset is less than the size. The
loop condition is `__stored < __n && _M_off < _M_size`. Each stored
character is `_M_buf[_M_off] = __s[__stored]`, then the offset advances by
one. The return is `__stored`, the number of characters placed in the
caller buffer. A count larger than the bytes still left returns that
smaller number. It does not write past the buffer. It does not return the
requested count when the buffer is short, and it does not flush.

When the offset is already the size, including a size of 0, `sputn` returns
0 before the source is read.

A null pointer with a non-zero size is still the constructor
`__prism_check` (`FUNC-CONTRACT`), then
`if (!(__buf != nullptr || __n == 0)) _M_size = 0`. The size is 0 and the
offset is 0, so `sputn` takes the no-room path and does not write through
that pointer.

## What stayed

`sputc` is unchanged. The guard is still
`const int __ok = (_M_off < _M_size) ? 1 : 0;`. The check is still
`__prism_check` with class `MEM-OOB-WRITE` and message
`basic_syncbuf::sputc would write past the caller buffer`. The store and
the advance stay under `if (__ok)`. A write at the size does not store and
does not wrap the offset.

`sgetc` is unchanged. `sungetc` is unchanged.

The constructor check is still
`__prism_check((__buf != nullptr || __n == 0) ? 1 : 0, "FUNC-CONTRACT",`.

`emit` and the `syncstream` class are not declared. A unit that names one
fails to compile against this header. They are not stubbed. A flush that
returns success without a stream would be a fake write. `sputn` does not
do that.

`<span>`, `<sstream>`, `<string>`, `<string_view>`, `<ostream>`, and
`<mutex>` are not included.

## Wiring

`PRISM_PIR_MODELS`, `_prism_test_sources`, and the conformance header-name
array were not edited. `syncstream` was already on each of them.
`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` were not edited. A unit
whose lowered text contains `syncbuf` is still `NEEDS-HARNESS`
(`C++ syncbuf unencoded`). This slice does not claim the tasks are proved.

## Tests

`tests/cpp/test_cxx_syncstream.cpp` reads the header. The `sputc` text is
the span from `void sputc(` to `ptrdiff_t sputn(`, so the write check, the
store, and the single `++_M_off` stay on `sputc`. `sputn` is the span from
`ptrdiff_t sputn(` to `int sgetc(`. The `FUNC-CONTRACT` check is before
`if (__n < 0) __builtin_trap();`, that trap is before
`if (__n == 0) return 0;`, and that return is before
`if (_M_off >= _M_size) return 0;`. The loop bound
`while (__stored < __n && _M_off < _M_size)` is before the only
`_M_buf[_M_off] = __s[__stored];`. `return __stored;` is after that store.
The body does not `return __n;`. `sgetc` is the span from `int sgetc(` to
`int sungetc(`. `sungetc` is the span from `int sungetc(` to `private:`.

`tests/cxx_models/syncstream_trace.cpp` is the driver. Checked with
clang++ 18.1.3, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -pthread
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, and
`-isystem` on a copy of the model directory. Stdout with no argv was:

```
byte 65
kept 65
```

`argv` `put` stores three characters in a 4-byte buffer. Stdout was:

```
put 3
keptn 65 98 67 0
```

The fourth byte stays 0. The bytes are still in the caller buffer after
the destructor.

`argv` `zero` is a count of 0 and a null source. Stdout was `zero 0 90`.
The stored `'Z'` is unchanged. AddressSanitizer did not report a load.

`argv` `room` asks for 5 characters from a 2-byte source into a 2-byte
buffer. Stdout was `room 2 65 66`. AddressSanitizer did not report a read
past the source or a write past the buffer.

`argv` `noroom` is `sputc('A')`, then `sputn` of a null source with count
4. Stdout was `noroom 0 65`. `argv` `emptyn` is a positive count into a
size-0 buffer and a null source. Stdout was `emptyn 0 90`. Neither path
reads the null source.

`argv` `rest` is `sputc('A')`, then `sputn` of the three characters that
still fit. Stdout was:

```
rest 3 65 66 67 68
```

`argv` `neg` is a negative count. The process aborts. Stderr is
`CHECK FUNC-CONTRACT: basic_syncbuf::sputn count is negative`. It does not
print `stored`. `argv` `neggo` lets that check return. The same line is
printed, then the process dies with SIGILL from `__builtin_trap`, and it
does not print `stored`.

`argv` `after` fills a 2-byte buffer with `sputn` and prints `filled 2`,
then `sputc` is
`CHECK MEM-OOB-WRITE: basic_syncbuf::sputc would write past the caller buffer`
and does not print `extra`. The stored count was not a flush that made
room for another character.

`argv` `putforced` continues after checks: a null pointer with size 1
prints the constructor `FUNC-CONTRACT` check, then `putforced 0`. It does
not print a `MEM-OOB-WRITE` line. AddressSanitizer did not report a store.

`argv` `get` and `argv` `back` are the existing `sgetc` and `sungetc`
paths. `argv` `full` is the existing `sputc` check.

`b.emit()` and `std::syncstream` fail to compile against this header
(`no member named 'emit'` / `no type named 'syncstream'`).

`-M` names the `syncstream` header on that include path.
