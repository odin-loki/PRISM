# fstream tellp

Branch `finish/cxx-fstream-tellp`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::basic_ofstream::tellp` is a member of the existing extensionless file
`src/prism/pir/models/cxx/fstream`. A directory of that name would make
`#include <fstream>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_FSTREAM`. The file is still
a `#pragma GCC system_header`. The function is in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The result is
not `PROVED` and not `CLEAN`. This header is not a proof that a file exists,
and `tellp` is not a proof of a file position.

## What tellp does

`std::basic_ofstream` is a class template for `char` only. The traits
parameter is omitted, so the two-argument form fails to compile and falls
back. `basic_ofstream<wchar_t>` fails the `char` static_assert and falls
back the same way. There is no `ofstream` alias.

The return type of `tellp` is `long`.

A default stream is not open. `_M_is_open` is initialized false, and nothing
in this header stores true. `tellp` calls

```
__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",
              "std::basic_ofstream::tellp on a stream that is not open");
```

and then `__builtin_trap`. On a default stream the condition is 0. The
function body has no `return`. It does not return 0. It does not return a
file position. There is no `pos_type`, `streampos`, `streamoff`, or `fpos`
in that body.

`basic_ofstream` holds one bool. Copy construction and copy assignment are
deleted. The destructor is defaulted and does not free. No constructor
takes a path or a file name. `open`, `close`, `write`, `put`, and `seekp`
are not declared.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What stayed

`is_open`, `read`, `get` (`get()`, `get(char&)`, `get(char*, n)`, and
`get(char*, n, delim)`), `tellg`, and the `basic_ifstream` destructor are
unchanged. `is_open()` returns `_M_is_open` and stays false. `read` and
`get` are still `FUNC-CONTRACT` on a stream that is not open. `get()` still
returns `-1`. The caller buffer is still not written. `tellg` still checks
and then `__builtin_trap`, and its body still has no `return`. The
`basic_ifstream` destructor is still defaulted and does not free. That
object still holds one bool. Copy construction and copy assignment stay
deleted.

`open` and `close` stay omitted. `ofstream`, `fstream`, `filebuf`,
`basic_fstream`, `basic_filebuf`, `wifstream`, `exists`, `peek`, `getline`,
`ignore`, and `operator>>` stay omitted. A constructor that takes a path or
a file name stays omitted. The traits parameter of `basic_ifstream` stays
omitted. A use of an omitted name fails to compile against this header, and
pir falls back (`extra.cxx_models`). They are not stubbed.

The header still does not include `<string>`, `<filesystem>`, `<memory>`,
`<mutex>`, `<iostream>`, `<istream>`, or `<ostream>`.

## Wiring

`CMakeLists.txt` was not edited. No CMake line was dropped. The existing
`_prism_test_sources` line for `tests/cpp/test_cxx_fstream.cpp` is still
there. No new test file was added. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/fstream`. `tests/cpp/test_qa_conformance.cpp` was
not edited. No new conformance task was added, so this slice does not record
an `expected` value for `tellp`.

## Tests

`tests/cpp/test_cxx_fstream.cpp` reads the header. `long tellp()` is present.
After comments and string literals are stripped, `tellp` and
`basic_ofstream` are identifiers. `ofstream` is still not an identifier.
The `tellp` body calls `__prism_check` with class `FUNC-CONTRACT` and
message `std::basic_ofstream::tellp on a stream that is not open`, and that
call is before `__builtin_trap()`. The body does not contain `return`,
`return 0`, or a position type, and it does not assign `_M_is_open`. The
message occurs once. `__builtin_trap()` occurs twice: once in `tellg` and
once in `tellp`. The closed-stream
`__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",` call occurs seven
times: the five existing `read` / `get` bodies, `tellg`, and `tellp`.
`is_open()` is still `{ return _M_is_open; }`. `open`, `close`, `ofstream`,
`fstream`, `peek`, `getline`, `write`, `put`, and `seekp` are still not
identifiers in the stripped header. `_M_is_open = true` does not occur.
Both objects initialize `_M_is_open` to false.
