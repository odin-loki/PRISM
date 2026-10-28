# spanstream sputn

Branch `finish/cxx-spanstream-sputn`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`sputn` is a member of the existing `std::basic_spanbuf<char>` in
`src/prism/pir/models/cxx/spanstream`. The header is still a file. A
directory of that name would make `#include <spanstream>` fail with EISDIR.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans. This header is not a proof. `sputn` is not PROVED and
not CLEAN.

## What sputn does

`sputn`, `sputc`, `sgetc`, and `sungetc` share the one offset. The count
is `std::ptrdiff_t`, which is libstdc++ `std::streamsize`. The header does
not include `<ios>`.

A negative count calls `__prism_check` with class `FUNC-CONTRACT` and
message `std::basic_spanbuf::sputn negative count`, then `__builtin_trap`.
That arm does not read the source and does not write.

A count of 0 returns 0. That arm does not read the source and does not
write. A null source with count 0 takes this path.

A null source with a positive count calls `__prism_check` with class
`PTR-NULL-DEREF` and message `std::basic_spanbuf::sputn null source`, then
`__builtin_trap`. That arm does not write.

Otherwise the byte length is `count * sizeof(char)`. That product is
formed only in the arm where it fits in `size_t`. When it does not fit,
`sputn` calls `__prism_check` with class `FUNC-CONTRACT` and message
`std::basic_spanbuf::sputn count times element size does not fit`, then
`__builtin_trap`. That arm does not multiply and does not write. The
length is not a wrapped product. For `char`, the element size is 1, so a
non-negative `ptrdiff_t` fits in `size_t`. This trace does not hit that arm.

On the fit path, `sputn` stores `min(count, remaining)` characters, advances
the offset by that many, and returns that count. It does not write past the
buffer. The return is not the requested count when the buffer is shorter,
so the unwritten tail is not stored. A full buffer returns 0 and does not
read the source.

## What stayed

`sputc` is unchanged. At the size, including a size of 0, it is still
`__prism_check` with class `MEM-OOB-WRITE` and message
`std::basic_spanbuf::sputc write at size`. The store and the advance stay
under `if (_M_off < _M_size)`.

`sgetc` is unchanged. At the size, including a size of 0, it is still
`__prism_check` with class `MEM-OOB-READ` and message
`std::basic_spanbuf::sgetc read at size`. The load sits under
`if (_M_off < _M_size)`.

`sungetc` is unchanged. At offset 0 it is still `__prism_check` with class
`MEM-OOB-READ` and message
`std::basic_spanbuf::sungetc read before the buffer`. The decrement and the
load sit under `if (_M_off > 0)`.

A null pointer with a non-zero size is still `__prism_check` at construction
(`PTR-NULL-DEREF`). The size stays the value the caller passed. It is not
rewritten into an empty buffer.

The destructor stays `~basic_spanbuf() noexcept = default`. It does not free.

The header still does not include `<span>`, `<sstream>`, `<string>`, or
`<string_view>`. These types are still not declared:

- `std::spanstream`, `std::ispanstream`, `std::ospanstream`
- `std::basic_spanstream`, `std::basic_ispanstream`, `std::basic_ospanstream`

`src/prism/bmc.cpp` was not edited. `src/prism/pir/stage.cpp` was not
edited. `CMakeLists.txt` was not edited. The conformance header-name array
in `tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_spanstream.cpp` reads the header. The negative arm is
`FUNC-CONTRACT` then `__builtin_trap`, and it does not name `_M_ptr` or
return. Count 0 is `return 0` before the null check, with no load and no
store. The null arm is `PTR-NULL-DEREF` then `__builtin_trap`, with no
store. The product arm traps and does not multiply. The multiply sits in
the next arm, the loop bound is the stored count, and the return is that
count. `sputc`, `sgetc`, and `sungetc` do not name `sputn` and do not call
`__builtin_trap`.

`tests/cxx_models/spanstream_trace.cpp` still writes `'A'` and prints that
byte after the spanbuf is destroyed. It also calls `sgetc` and `sungetc`,
then `sputn` of 6 characters into a 4-byte buffer.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS -pthread
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Werror`, and
`-isystem` on a copy of the model directory. The compile ran on ext4
(`/tmp/spanstream-sputn`). `-M` names
`/tmp/spanstream-sputn/cxx/spanstream`, that copy of
`src/prism/pir/models/cxx/spanstream`. Stdout with no argv was:

```
byte=A
got=66
back=66
n=4 clip=ABCD
```

`argv` `exact` printed `exact=2 AB`. `argv` `zero` is count 0 with a null
source and printed `zero=0 Z`. `argv` `full` is `sputc` into a one-byte
buffer, then `sputn` of two characters, and printed `full=0 Z`. AddressSanitizer
did not report a fault on those runs.

`argv` `neg` aborts with
`CHECK FUNC-CONTRACT: std::basic_spanbuf::sputn negative count` and does
not print `neg`. `argv` `null` aborts with
`CHECK PTR-NULL-DEREF: std::basic_spanbuf::sputn null source` and does not
print `null`. The trace stub aborts inside `__prism_check`, so those two
runs do not reach `__builtin_trap`.

A `/tmp` driver whose check returns, compiled the same way, takes the trap.
Negative count and a null source with a positive count both die with
`SIGILL`. A handler printed `ill buf=ZZZZ` and neither run printed `wrote`.

`testdata/conformance/prism/cxx/cxx_spanstream_sputn_true.cpp` requests 6
characters into a 4-byte buffer (`no-oob`, true).
`cxx_spanstream_sputn_false.cpp` calls `sputn` with a negative count when
`c` is 0 (`false`, witness `[0]`). When `c` is not 0 it writes two
characters. `sanitizer_blind` names that function: the failing path does
not write, and libstdc++ has no caller-buffer constructor, so a native
replay stays clean.
`testdata/conformance/libc-models/spanstream_contracts.cpp` states the
two-character write, the clipped write of 6 into 4, count 0, the negative
count, and the null source with a positive count.
The existing `sputc`, `sgetc`, and `sungetc` tasks are unchanged.
