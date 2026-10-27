# syncstream slice

Branch `finish/cxx-syncstream`, base `1f3d4de85465663e28412dfcaeabacbc1dc360fe`.

`std::basic_syncbuf` is one extensionless file,
`src/prism/pir/models/cxx/syncstream`. A directory of that name would make
`#include <syncstream>` fail with EISDIR. The include guard is
`_GLIBCXX_SYNCSTREAM`, so libstdc++'s `<syncstream>` is not parsed as well.
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml records the contract each harness asks for.
`expected` values are the booleans `true` and `false`. This header is not a
concurrency proof.

## What the model covers

`basic_syncbuf` stores a caller `char*`, a `size_t` size, and a `size_t`
write offset. Nothing is allocated. The only include is `<cstddef>`.
`<span>`, `<sstream>`, `<string>`, `<ostream>`, `<spanstream>`, and
`<mutex>` are not included.

The constructor stores that pointer and size and sets the offset to 0. A
null pointer with a non-zero size is `__prism_check` with class
`FUNC-CONTRACT` (`basic_syncbuf constructed with a null buffer and a
non-zero size`). A null pointer with size 0 passes that check. The
destructor is defaulted: it does not free the caller buffer and does not
pretend the bytes were emitted to a stream. Copy construction and copy
assignment are deleted. Move construction and move assignment are not
declared.

`sputc` writes one character only when the offset is less than the size,
then advances. The guard is `__ok = (_M_off < _M_size)`. The check is
`__prism_check` with class `MEM-OOB-WRITE` and message
`basic_syncbuf::sputc would write past the caller buffer`, and the store
and the advance are both under `if (__ok)`. When the offset is already the
size, the character is not stored and the offset is not wrapped. The class
is `MEM-OOB-WRITE` so the task property `no-oob` names this check.

`_PRISM_SYNCSTREAM_MODEL` is defined only by this header. The conformance
tasks use it so the same source still compiles against libstdc++, which has
no caller-buffer constructor.

## What is omitted

`emit` and the `syncstream` class are not declared. A unit that names one
fails to compile against this header and `lower_to_ir` lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed. A flush that returns success without a stream would be a fake
write. Nothing in this header writes to stdout.

## Still in front of the model

`src/prism/bmc_unenc.inc` still returns
`C++ syncbuf unencoded: {} is not a syncbuf model` when the function body
names `basic_syncbuf`. `src/prism/bmc.cpp` returns
`C++ syncbuf unencoded: ` plus the engine plus ` is not a syncbuf model`
when the lowered text contains `syncbuf`. That status is `NEEDS-HARNESS`.
It is not a proof. Those files were not edited. `src/prism/pir/stage.cpp`
was not edited. PIR was not run, so this slice does not claim the tasks
are proved.

## Wiring

`PRISM_PIR_MODELS` gained one line,
`src/prism/pir/models/cxx/syncstream`. The existing embed writes
`cxx/syncstream`. `_prism_test_sources` gained
`tests/cpp/test_cxx_syncstream.cpp`. `tests/cpp/test_qa_conformance.cpp`
lists `syncstream` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_syncstream.cpp` reads the header as text. A write at
the size is the `__prism_check` above, and that call is before the only
`_M_buf[_M_off] = __c` and the only `++_M_off`. The header does not define
`emit(`. It does not define `class syncstream`.

Checked with clang++ 18.1.3, libstdc++ 14, `-std=c++23
-D_GLIBCXX_ASSERTIONS -pthread -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. `tests/cxx_models/syncstream_trace.cpp`
compiled with `-isystem` on the model directory. Stdout was:

```
byte 65
kept 65
```

`byte 65` is `'A'` read back from the caller buffer. `kept 65` is that
same byte after the destructor. `-M` names
`src/prism/pir/models/cxx/syncstream`.

`argv` `full` is `sputc` on a size-0 buffer. The model aborts with
`CHECK MEM-OOB-WRITE: basic_syncbuf::sputc would write past the caller buffer`
and does not print `wrote`. `argv` `wrap` is a second `sputc` after one
character has filled a 1-byte buffer. The same check fires and it does not
print `wrapped`. `argv` `null` is a null pointer with size 1. The model
aborts with
`CHECK FUNC-CONTRACT: basic_syncbuf constructed with a null buffer and a non-zero size`
and does not print `constructed`. A null pointer with size 0 constructs,
then `sputc` is the `MEM-OOB-WRITE` check.

`b.emit()` and `std::syncstream` fail to compile against this header
(`no member named 'emit'` / `no type named 'syncstream'`).

`testdata/conformance/prism/cxx/cxx_syncstream_true.cpp` writes `'A'` into
a 4-byte caller buffer and returns that byte (`no-oob` true). With the
model on the include path the call returns 65. `cxx_syncstream_false.cpp`
is `sputc` one past a size-0 buffer when the input is 0 (`false`, witness
`[0]`). `sanitizer_blind` names that function: the model does not store
the byte, and the libstdc++ compilation of the same file has no
caller-buffer constructor, so it returns without a write and the sanitizer
stays clean. `testdata/conformance/libc-models/syncstream_contracts.cpp`
is the `#include <syncstream>` the conformance case searches for. It is
not a scored task.

cmake, `prism_tests`, and pir were not run.
