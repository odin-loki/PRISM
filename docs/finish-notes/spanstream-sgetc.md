# spanstream sgetc

Branch `finish/cxx-spanstream-sgetc`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`sgetc` is a member of the existing `std::basic_spanbuf<char>` in
`src/prism/pir/models/cxx/spanstream`. The header is still a file. A
directory of that name would make `#include <spanstream>` fail with EISDIR.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans. This header is not a proof.

## What sgetc does

`sgetc` and `sputc` share the one offset.

When the offset is less than the size, `sgetc` returns that byte as an
unsigned char converted to `int` (0..255), then advances the offset by one.
A byte of 255 is 255. It is not the end signal.

When the offset is already the size, including a size of 0, `sgetc` calls
`__prism_check` with class `MEM-OOB-READ` and message
`std::basic_spanbuf::sgetc read at size`. The pointer is not read, and the
function does not return a character from the buffer. After that check it
returns `-1`, char_traits-style EOF, only as that end signal.

A null pointer with size 0 starts with the offset at the size. `sgetc` takes
that same check and does not read the pointer.

## What stayed

`sputc` is unchanged. At the size, including a size of 0, it is still
`__prism_check` with class `MEM-OOB-WRITE` and it does not write.

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

`tests/cpp/test_cxx_spanstream.cpp` reads the header. The `sgetc` check is
before the only load. The load is
`static_cast<int>(static_cast<unsigned char>(_M_ptr[_M_off]))` inside
`if (_M_off < _M_size)`, and `++_M_off` is in that same arm. `return -1` is
after that arm. The pointer constructor still initialises `_M_size(__n)` and
does not assign the size in the body.

`tests/cxx_models/spanstream_trace.cpp` still writes `'A'` and prints that
byte after the spanbuf is destroyed. It also calls `sgetc` on a one-byte
buffer holding `'B'` and prints that int. This slice did not compile the
trace.

`testdata/conformance/prism/cxx/cxx_spanstream_sgetc_true.cpp` reads the
first byte of a 4-byte buffer (`no-oob`, true).
`cxx_spanstream_sgetc_false.cpp` calls `sgetc` on a size-0 buffer when `c`
is 0 (`false`, witness `[0]`). `sanitizer_blind` names that function: the
model does not load the pointer, and libstdc++ has no caller-buffer
constructor, so a native replay stays clean.
`testdata/conformance/libc-models/spanstream_contracts.cpp` states the
in-range read, the size-0 `sgetc`, and the null pointer with size 0.
The existing `sputc` tasks are unchanged.
