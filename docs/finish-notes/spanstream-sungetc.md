# spanstream sungetc

Branch `finish/cxx-spanstream-sungetc`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`sungetc` is a member of the existing `std::basic_spanbuf<char>` in
`src/prism/pir/models/cxx/spanstream`. The header is still a file. A
directory of that name would make `#include <spanstream>` fail with EISDIR.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans. This header is not a proof.

## What sungetc does

`sungetc`, `sgetc`, and `sputc` share the one offset.

`sungetc` returns `int`. When the offset is greater than 0 it decrements
the offset by one, then returns the unsigned char at the new offset as
`int` (0 through 255). A stored `0xFF` is 255.

When the offset is already 0, including a size of 0, `sungetc` calls
`__prism_check` with class `MEM-OOB-READ` and message
`std::basic_spanbuf::sungetc read before the buffer`. The decrement and
the load sit under `if (_M_off > 0)`, after the check. That path leaves
the offset at 0 and does not read the pointer. `return -1` is after the
check. -1 is the end signal.

A null pointer with size 0 starts with the offset at 0. `sungetc` takes
that same check and does not read the pointer.

## What stayed

`sputc` is unchanged. At the size, including a size of 0, it is still
`__prism_check` with class `MEM-OOB-WRITE` and message
`std::basic_spanbuf::sputc write at size`. The store and the advance stay
under `if (_M_off < _M_size)`. A write at the size does not store.

`sgetc` is unchanged. At the size, including a size of 0, it is still
`__prism_check` with class `MEM-OOB-READ` and message
`std::basic_spanbuf::sgetc read at size`. The load sits under
`if (_M_off < _M_size)`. A read at the size does not load the pointer.

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

`tests/cpp/test_cxx_spanstream.cpp` reads the header. The `MEM-OOB-READ`
check is before `if (_M_off > 0)`. `--_M_off` is inside that arm, and the
only load is
`static_cast<int>(static_cast<unsigned char>(_M_ptr[_M_off]))` after the
decrement. `return -1` is after that arm. The text before the `if` does
not decrement and does not name `_M_ptr`.

`tests/cxx_models/spanstream_trace.cpp` still writes `'A'` and prints that
byte after the spanbuf is destroyed. It also calls `sgetc` on a one-byte
buffer holding `'B'`, then `sungetc`.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS -pthread
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, and
`-isystem` on a copy of the model directory. The compile ran on ext4
(`/tmp/spanstream-sungetc`). `-M` names
`/tmp/spanstream-sungetc/cxx/spanstream`, that copy of
`src/prism/pir/models/cxx/spanstream`. Stdout with no argv was:

```
byte=A
got=66
back=66
```

`argv` `hi` printed `hi=255`. `argv` `off0` is `sungetc` while the offset
is 0 on a 4-byte buffer. The model aborts with
`CHECK MEM-OOB-READ: std::basic_spanbuf::sungetc read before the buffer`
and does not print `back`. `argv` `empty` is a size of 0. The same check
fires and it does not print `empty`. `argv` `null0` is a null pointer with
size 0. The same check fires and it does not print `read`. `argv` `twice`
prints `once=65`, then the next `sungetc` is that check and it does not
print `again`. `argv` `forced` continues after checks: a null pointer with
size 1 prints the constructor `PTR-NULL-DEREF` check, then the
`MEM-OOB-READ` check, then `forced=-1 size=1`. AddressSanitizer did not
report a load.

`testdata/conformance/prism/cxx/cxx_spanstream_sungetc_true.cpp` reads 255
and puts that byte back (`no-oob`, true).
`cxx_spanstream_sungetc_false.cpp` calls `sungetc` at offset 0 when `c` is
0 (`false`, witness `[0]`). When `c` is not 0 it reads one byte, then puts
that byte back. `sanitizer_blind` names that function: the failing path
does not load the pointer, and libstdc++ has no caller-buffer constructor,
so a native replay stays clean.
`testdata/conformance/libc-models/spanstream_contracts.cpp` states the
in-range putback of 255, `sungetc` at offset 0 on a 4-byte buffer, and the
null pointer with size 0.
The existing `sputc` and `sgetc` tasks are unchanged.
