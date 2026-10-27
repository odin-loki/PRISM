# spanstream slice

Branch `finish/cxx-spanstream`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.
`<spanstream>` is a file, `src/prism/pir/models/cxx/spanstream`, the same shape
as `vector` and `memory`. A directory of that name would make
`#include <spanstream>` fail with EISDIR. The header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml files record the contract each harness asks for. `expected` values
are YAML booleans. This header is not a proof.

## What the model covers

`std::basic_spanbuf<char>` only. It stores a `char` pointer, a size, and a
write offset. Nothing is allocated. The destructor is defaulted and does not
free. The characters belong to the caller.

`sputc` writes one character only when the offset is less than the size, then
advances the offset. When the offset is already the size, `sputc` calls
`__prism_check` (`MEM-OOB-WRITE`) and does not write. A size of 0 starts with
the offset at the size, so the first `sputc` is that check and writes nothing.

A null pointer with a non-zero size is `__prism_check` at construction
(`PTR-NULL-DEREF`). The size stays the value the caller passed. It is not
rewritten into an empty buffer. A null pointer with size 0 is empty.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and
`lower_to_ir` lowers the whole translation unit again with libstdc++
(`extra.cxx_models`). They are omitted because declaring them would include
`<span>` or `<sstream>`. This header does not include `<span>`, `<sstream>`,
`<string>`, or `<string_view>`. They are not stubbed:

- `std::spanstream`, `std::ispanstream`, `std::ospanstream`
- `std::basic_spanstream`, `std::basic_ispanstream`, `std::basic_ospanstream`
- a constructor or `span()` that takes or returns `std::span`
- `basic_spanbuf`'s traits parameter, and `wchar_t` spanbufs

`src/prism/bmc.cpp` still reports spanstream as unencoded from a parse
failure that names it. `src/prism/pir/stage.cpp` was not edited. It already
puts the embedded `cxx/` directory on the include path.

## Tests

`tests/cpp/test_cxx_spanstream.cpp` reads the header. `sputc` at the size is
a `__prism_check` and the store sits inside `if (_M_off < _M_size)`, so that
path does not write. The destructor is defaulted, and the header does not
call `free`.

`tests/cxx_models/spanstream_trace.cpp` includes `<spanstream>`, writes one
character into a 4-byte caller buffer, and prints that byte after the
spanbuf is destroyed. The doctest does not compile the trace.

`testdata/conformance/prism/cxx/cxx_spanstream_true.cpp` writes one character
into a 4-byte buffer and reads that byte back (`no-oob`, true).
`cxx_spanstream_false.cpp` calls `sputc` on a size-0 buffer when `c` is 0
(`false`, witness `[0]`).
`testdata/conformance/libc-models/spanstream_contracts.cpp` includes
`<spanstream>` and states the same write, the size-0 `sputc`, and the null
pointer with a non-zero size.

## Wiring

`PRISM_PIR_MODELS` gained `src/prism/pir/models/cxx/spanstream`.
`_prism_test_sources` gained `tests/cpp/test_cxx_spanstream.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"spanstream"` with the other
public headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged.
