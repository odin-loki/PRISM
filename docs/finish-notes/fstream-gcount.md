# fstream gcount

Branch `finish/cxx-fstream-gcount`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`std::basic_ifstream::gcount` is a member of the existing extensionless file
`src/prism/pir/models/cxx/fstream`. A directory of that name would make
`#include <fstream>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_FSTREAM`. The file is still
a `#pragma GCC system_header`. The function is in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The result is
not `PROVED` and not `CLEAN`. This header is not a proof that a file exists,
and `gcount` is not a proof of a character count.

## What gcount does

`gcount` is a member of `std::basic_ifstream` only. It is not a member of
`std::basic_ofstream`.

The return type is `std::ptrdiff_t`, which is libstdc++ `std::streamsize`.
The header does not include `<ios>`. The signature is

```
std::ptrdiff_t gcount() const { return 0; }
```

Nothing in this header reads a file. `read` and `get` do not extract a
character, and `gcount` does not store a count of its own. The body is
`return 0`. That 0 is the extraction count of a header that never reads a
file. It is not a character count taken from a file. The body does not call
`fopen` or `fread`. It does not call `__prism_check`. It does not call
`__builtin_trap`. It does not read `_M_is_open` and it does not assign
`_M_is_open`. The open flag stays as it was.

`basic_ifstream` still holds one bool. There is no `_M_gcount`.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What stayed

`is_open`, `read`, `get` (`get()`, `get(char&)`, `get(char*, n)`, and
`get(char*, n, delim)`), `tellg`, `tellp`, and both destructors are
unchanged. `is_open()` returns `_M_is_open` and stays false. `read` and
`get` are still `FUNC-CONTRACT` on a stream that is not open. `get()` still
returns `-1`. The caller buffer is still not written. `tellg` on a stream
that is not open still checks and then `__builtin_trap`, and its body still
has no `return`. That path does not return 0 and does not return a file
position. `tellp` on a `basic_ofstream` that is not open still checks and
then `__builtin_trap`, and its body still has no `return`. That path does
not return 0 and does not return a file position. The destructors are still
defaulted and do not free. Each object still holds one bool. Copy
construction and copy assignment stay deleted.

`open` and `close` stay omitted. They were not added. `ofstream`, `fstream`,
`filebuf`, `basic_fstream`, `basic_filebuf`, `wifstream`, `exists`, `peek`,
`getline`, `ignore`, and `operator>>` stay omitted. `write`, `put`, and
`seekp` stay omitted. A constructor that takes a path or a file name stays
omitted. The traits parameter of `basic_ifstream` stays omitted. A use of
an omitted name fails to compile against this header, and pir falls back
(`extra.cxx_models`). They are not stubbed.

The header still does not include `<string>`, `<filesystem>`, `<memory>`,
`<mutex>`, `<iostream>`, `<istream>`, or `<ostream>`.

## Wiring

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp`
were not edited. `CMakeLists.txt` was not edited. No CMake line was dropped.
No new test file was added. The existing `_prism_test_sources` line for
`tests/cpp/test_cxx_fstream.cpp` is still there. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/fstream`. `tests/cpp/test_qa_conformance.cpp`
was not edited. No new conformance task was added, so this slice does not
record an `expected` value for `gcount`.

## Tests

`tests/cpp/test_cxx_fstream.cpp` reads the header. `std::ptrdiff_t gcount() const`
is present, once, and it appears before `class basic_ofstream`. After
comments and string literals are stripped, `gcount` is an identifier. The
body is `{ return 0; }`. That body does not mention `_M_is_open`,
`__prism_check`, `__builtin_trap`, `fopen`, or `fread`, and it contains one
`return`. The `tellg` and `tellp` bodies do not mention `gcount`.

`long tellg()` and `long tellp()` are still present. Each body still calls
`__prism_check` with class `FUNC-CONTRACT` before `__builtin_trap()`, and
neither body contains `return` or `return 0`. `__builtin_trap()` occurs
twice. The closed-stream
`__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",` call occurs seven
times: the five existing `read` / `get` bodies, `tellg`, and `tellp`.
`gcount` is not one of them. `is_open()` is still `{ return _M_is_open; }`.
`_M_is_open = true` does not occur. Both objects initialize `_M_is_open` to
false. `open`, `close`, `ofstream`, `fstream`, `peek`, `getline`, `write`,
`put`, and `seekp` are still not identifiers in the stripped header.
