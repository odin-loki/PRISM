# fstream tellg

Branch `finish/cxx-fstream-tellg`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::basic_ifstream::tellg` is a member of the existing extensionless file
`src/prism/pir/models/cxx/fstream`. A directory of that name would make
`#include <fstream>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_FSTREAM`. The file is still
a `#pragma GCC system_header`. The function is in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The result is
not `PROVED` and not `CLEAN`. This header is not a proof that a file exists,
and `tellg` is not a proof of a file position.

## What tellg does

The return type is `long`.

A default stream is not open. `_M_is_open` is initialized false, and nothing
in this header stores true. `tellg` calls

```
__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",
              "std::basic_ifstream::tellg on a stream that is not open");
```

and then `__builtin_trap`. On a default stream the condition is 0. The
function body has no `return`. It does not return 0. It does not return a
file position. There is no `pos_type`, `streampos`, `streamoff`, or `fpos`
in that body.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What stayed

`is_open`, `read`, `get` (`get()`, `get(char&)`, `get(char*, n)`, and
`get(char*, n, delim)`), and the destructor are unchanged. `is_open()`
returns `_M_is_open` and stays false. `read` and `get` are still
`FUNC-CONTRACT` on a stream that is not open. `get()` still returns `-1`.
The caller buffer is still not written. The destructor is still defaulted
and does not free. The object still holds one bool. Copy construction and
copy assignment stay deleted.

`open` and `close` stay omitted. `ofstream`, `fstream`, `filebuf`,
`basic_ofstream`, `basic_fstream`, `basic_filebuf`, `wifstream`, `exists`,
`peek`, `getline`, `ignore`, and `operator>>` stay omitted. A constructor
that takes a path or a file name stays omitted. The traits parameter of
`basic_ifstream` stays omitted. A use of an omitted name fails to compile
against this header, and pir falls back (`extra.cxx_models`). They are not
stubbed.

The header still does not include `<string>`, `<filesystem>`, `<memory>`,
`<mutex>`, `<iostream>`, `<istream>`, or `<ostream>`.

## Wiring

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp`
were not edited. `CMakeLists.txt` was not edited. The existing
`_prism_test_sources` line for `tests/cpp/test_cxx_fstream.cpp` is still
there. `PRISM_PIR_MODELS` already lists `src/prism/pir/models/cxx/fstream`.
`tests/cpp/test_qa_conformance.cpp` was not edited. No new conformance task
was added, so this slice does not record an `expected` value for `tellg`.

## Tests

`tests/cpp/test_cxx_fstream.cpp` reads the header. `long tellg()` is present.
After comments and string literals are stripped, `tellg` is an identifier.
The body calls `__prism_check` with class `FUNC-CONTRACT` and message
`std::basic_ifstream::tellg on a stream that is not open`, and that call is
before `__builtin_trap()`. The body does not contain `return`, `return 0`,
or a position type, and it does not assign `_M_is_open`. The message occurs
once. `__builtin_trap()` occurs once. The closed-stream
`__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",` call occurs six times:
the five existing `read` / `get` bodies, plus `tellg`. `is_open()` is still
`{ return _M_is_open; }`. `open`, `close`, `ofstream`, `fstream`, `peek`,
and `getline` are still not identifiers in the stripped header.
