# fstream slice

Branch `finish/cxx-fstream`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::ifstream` is the extensionless file `src/prism/pir/models/cxx/fstream`.
A directory of that name would make `#include <fstream>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_FSTREAM`, so libstdc++'s own header is not also included. The file
is a `#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`. This header is not a proof that a file exists.

## What the model covers

`std::basic_ifstream<char>` only, and `std::ifstream` as that alias. The
object holds one bool, `_M_is_open`, initialized false. A default stream is
not open: `is_open()` returns that flag. Nothing in the header stores true.
There is no heap. The destructor is defaulted and does not free. Copy
construction and copy assignment are deleted. Move is not declared.

`read` and every `get` (`get()`, `get(char&)`, `get(char*, n)`, and
`get(char*, n, delim)`) call

```
__prism_check(_M_is_open ? 1 : 0, "FUNC-CONTRACT",
              "std::basic_ifstream::read on a stream that is not open")
```

or the same call with the `get` message. On a default stream the condition
is 0. The caller buffer is not written, including no trailing NUL.
`get(char&)` does not store. `get()` returns `-1`, which is eof, not a
character read from a file.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What falls back

Not declared, so a use fails to compile against this header and pir falls
back (`extra.cxx_models`). They are not stubbed. A successful `open` would
be a fake file. A function that reported a file exists would be the same
kind of lie.

- `open`, `close`
- `ofstream`, `fstream`, `filebuf`, `basic_ofstream`, `basic_fstream`,
  `basic_filebuf`, `wifstream`
- a constructor that takes a path or a file name
- `exists`
- `peek`, `getline`, `ignore`, `operator>>`
- the traits parameter of `basic_ifstream` (the two-argument form)

`basic_ifstream<wchar_t>` fails the `char` static_assert and falls back the
same way. The header does not include `<string>`, `<filesystem>`,
`<memory>`, `<mutex>`, `<iostream>`, `<istream>`, or `<ostream>`.

## Still in front of the model

`src/prism/bmc.cpp` still returns `C++ fstream unencoded: ` plus the engine
plus ` is not an fstream model` when the lowered text matches
`ifstream`, `ofstream`, or `fstream`. `src/prism/bmc_unenc.inc` returns the
same sentence from a function body. `#include <fstream>` matches that word.
Those files were not edited. `src/prism/pir/stage.cpp` was not edited. It
already writes every embedded `cxx/` model and passes that directory as
`-isystem`. PIR was not run, so this slice does not claim the tasks are
proved or refuted.

## Wiring

`PRISM_PIR_MODELS` gained

```
src/prism/pir/models/cxx/fstream
```

so the embedded name is `cxx/fstream`. `_prism_test_sources` gained
`tests/cpp/test_cxx_fstream.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`"fstream"` next to the other public header names. Every name that was
already in that array is still there. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged. `iostream`
and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_fstream.cpp` reads the header. It is a regular file.
The guard, the pragma, the `char` static_assert, and
`using ifstream = basic_ifstream<char>` are present. `_M_is_open` is
initialized false and is never assigned true. The destructor is defaulted.
`__prism_check` is declared once and has no body in the header. Each of the
five `read` / `get` bodies calls it with `FUNC-CONTRACT` before any use of
the buffer, and none of those bodies store a byte. `get()` returns `-1`.
After comments and string literals are stripped, `open`, `close`,
`ofstream`, `fstream`, `filebuf`, `exists`, `peek`, `getline`, and `ignore`
are not identifiers. The forbidden includes are absent, and so are
`::operator new`, `malloc`, and `free(`.

Compiled alone with clang++ 18.1.3 (`-std=c++23`, doctest main), 72
assertions passed. `prism_tests` was not run.

`tests/cxx_models/fstream_trace.cpp` is `#include <fstream>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. Compiled
twice (`-isystem` the model directory, and without it). Default stdout
matched:

```
open 0
```

Both processes exited 0. With the model, `read`, `get`, and `getref` abort
(`rc` 134) and print

```
CHECK FUNC-CONTRACT: std::basic_ifstream::read on a stream that is not open
```

or the `get` message. Stdout is empty: no `byte`, `got`, or `ref` line.
libstdc++ does not abort. `read` prints `byte wxyz`, `get` prints `got -1`,
and `getref` prints `ref Q`. AddressSanitizer stayed quiet on that
libstdc++ run, so `cxx_fstream_false` is `sanitizer_blind`.

A separate driver included the header by `-isystem` and let `__prism_check`
return. Five checks fired. `is_open()` was 0, the `read` buffer stayed
`wxyz`, `get(char&)` left `Q` in place, `get()` returned `-1`, and both
array `get`s left `mnop` in place.

`open("a")`, `std::ofstream`, `basic_ifstream<wchar_t>`,
`basic_ifstream<char, Tr>`, and `ifstream in("missing")` fail to compile
against this header.

`testdata/conformance/prism/cxx/cxx_fstream_true.cpp` checks `is_open()` of
a default stream and does not call `read` or `get` (`memsafety`,
`expected: true`). `cxx_fstream_false.cpp` calls `read` and `get` when the
input is 0 (`expected: false`, witness `[0]`). Both `expected` values are
YAML booleans. There is no task that calls `open` or `exists`.
`testdata/conformance/libc-models/fstream_contracts.cpp` is the
`#include <fstream>` harness: the closed stream, plus `read` and `get` that
must not store a character.
