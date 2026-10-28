# fstream write

Branch `finish/cxx-fstream-write`, base `72a1cc5a5402874a154e9266b9807cf1d70f0db6`.

`std::basic_ofstream::write` is a member of the existing extensionless file
`src/prism/pir/models/cxx/fstream`. A directory of that name would make
`#include <fstream>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_FSTREAM`. The file is still
a `#pragma GCC system_header`. The function is in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The result is
not `PROVED` and not `CLEAN`. This header is not a proof that a file exists,
and `write` is not a proof of a byte count.

## What write does

`write` is a member of `std::basic_ofstream` only. It is not a member of
`std::basic_ifstream`.

`std::basic_ofstream` is a class template for `char` only. The traits
parameter is omitted, so the two-argument form fails to compile and falls
back. `basic_ofstream<wchar_t>` fails the `char` static_assert and falls
back the same way. There is no `ofstream` alias.

The signature is

```
basic_ofstream& write(const char*, std::ptrdiff_t)
```

The return type is `basic_ofstream&`. It is not `std::ptrdiff_t` and it is
not a byte count. `std::ptrdiff_t` is the count parameter, the same width
libstdc++ uses for `std::streamsize`. The parameters are not named, so the
body has no pointer to store through and no count to return.

A default stream is not open. `_M_is_open` is initialized false, and nothing
in this header stores true. `write` calls

```
__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",
              "std::basic_ofstream::write on a stream that is not open");
```

and then `__builtin_trap`. On a default stream the condition is 0. The
function body has no `return`. It does not return 0. It does not return the
count. It does not write any byte. It does not assign `_M_is_open`. There
is no `fopen` and no `fwrite` in that body.

`basic_ofstream` holds one bool. Copy construction and copy assignment are
deleted. The destructor is defaulted and does not free. No constructor
takes a path or a file name. `open`, `close`, `put`, and `seekp` are not
declared.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What stayed

`is_open`, `read`, `get` (`get()`, `get(char&)`, `get(char*, n)`, and
`get(char*, n, delim)`), `tellg`, `tellp`, `gcount`, and both destructors
are unchanged. `is_open()` returns `_M_is_open` and stays false. `read` and
`get` are still `FUNC-CONTRACT` on a stream that is not open. `get()` still
returns `-1`. The caller buffer is still not written. `tellg` on a stream
that is not open still checks and then `__builtin_trap`, and its body still
has no `return`. That path does not return 0 and does not return a file
position. `tellp` on a `basic_ofstream` that is not open still checks and
then `__builtin_trap`, and its body still has no `return`. That path does
not return 0 and does not return a file position. `gcount()` is still
`{ return 0; }`. That 0 is not a character count taken from a file, and
`gcount` does not change the open flag. The destructors are still defaulted
and do not free. Each object still holds one bool. Copy construction and
copy assignment stay deleted.

`open` and `close` stay omitted. They were not added. `ofstream`, `fstream`,
`filebuf`, `basic_fstream`, `basic_filebuf`, `wifstream`, `exists`, `peek`,
`getline`, `ignore`, and `operator>>` stay omitted. `put` and `seekp` stay
omitted. A constructor that takes a path or a file name stays omitted. The
traits parameter of `basic_ifstream` stays omitted. The traits parameter of
`basic_ofstream` stays omitted. A use of an omitted name fails to compile
against this header, and pir falls back (`extra.cxx_models`). They are not
stubbed.

The header still does not include `<string>`, `<filesystem>`, `<memory>`,
`<mutex>`, `<iostream>`, `<istream>`, or `<ostream>`.

## Wiring

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp`
were not edited. `CMakeLists.txt` was not edited. No CMake line was dropped.
No new test file was added. The existing `_prism_test_sources` line for
`tests/cpp/test_cxx_fstream.cpp` is still there. `PRISM_PIR_MODELS` already
lists `src/prism/pir/models/cxx/fstream`. `tests/cpp/test_qa_conformance.cpp`
was not edited. No new conformance task was added, so this slice does not
record an `expected` value for `write`.

## Tests

`tests/cpp/test_cxx_fstream.cpp` reads the header. `basic_ofstream& write(const char*, std::ptrdiff_t)`
is present, once, and it appears after `class basic_ofstream` and after
`long tellp()`. After comments and string literals are stripped, `write` is
an identifier. The body calls `__prism_check` with class `FUNC-CONTRACT`
and message `std::basic_ofstream::write on a stream that is not open`, and
that call is before `__builtin_trap()`. The body does not contain `return`,
`return 0`, or `return __n`. It does not name `__s` or `__n`, does not
store through a pointer, and does not assign `_M_is_open`. The message
occurs once. `__builtin_trap()` occurs three times: `tellg`, `tellp`, and
`write`. The closed-stream
`__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",` call occurs eight
times: the five existing `read` / `get` bodies, `tellg`, `tellp`, and
`write`. `is_open()` is still `{ return _M_is_open; }`. `gcount()` is still
`{ return 0; }`. `_M_is_open = true` does not occur. Both objects initialize
`_M_is_open` to false. The header says `This is not PROVED and not CLEAN.`
once. `open`, `close`, `ofstream`, `fstream`, `peek`, `getline`, `put`, and
`seekp` are still not identifiers in the stripped header.
