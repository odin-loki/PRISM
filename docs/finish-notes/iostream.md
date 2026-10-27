# iostream slice

`<iostream>` is not replaced. The pir stage still lowers units that include it
with libstdc++. Where that code recurses or calls into `libstdc++.so`, the
result stays `NEEDS-HARNESS`, as `docs/PIR.md` already records for iostreams.

## What is modelled

`src/prism/pir/models/cxx/iostream/prism_ostream.hpp` is a recording buffer,
`prism::cxx_model::ostream_buf`. Tests include that path. It is not
`std::ostream` and it does not live in namespace `std`.

It appends:

- a decimal `int` (no width, no base, no showpos)
- a C string
- a counted character range (`write`)

The characters are kept with the same storage shape as the `basic_string`
model: a 15-character local buffer, `_M_create`'s growth (a request below
twice the old capacity becomes twice that capacity, clamped to `max_size()`),
`::operator new` / `::operator delete`, and element loops rather than
`memcpy`. One extra byte is a trailing NUL. `data()` has `size()` characters.
A heap block is `capacity() + 1` bytes.

A failed stream keeps the characters it already stored and records nothing
further. That is the same outcome as a stream that has already failed a write
(stdio returns EOF or a short count once the error is known; an ostream
sentry does not insert). The state is entered by `set_bad()`, by a null
string with a non-zero length, or by a length that does not fit. It stays
set. Ordinary tests include this header and check the buffer, so the failure
is that explicit state. It is not a nondet branch on every insert (that is
how the libc `stdio` models treat an external file).

`tests/cpp/test_cxx_iostream.cpp` includes the header by path and checks the
recorded text, the growth step from 15 to 30, and the failed state.
`testdata/conformance/prism/cxx/cxx_ostream_true.cpp` and
`cxx_ostream_false.cpp` do the same. They do not include `<iostream>`. The
true task reads a recorded character inside the buffer. The false task reads
one past the heap block (`capacity() + 1` on a block of `capacity() + 1`
bytes) after a 16-character `write`.

## What still falls through

Anything that includes a real iostream header, and anything this buffer does
not implement:

- `<iostream>`, `<ostream>`, `<istream>`, `<sstream>`, `<ios>`, `<streambuf>`
- `cin` / `cout` / `cerr` / `clog`, `std::basic_ostream`, `std::basic_istream`
- floats, pointers, `bool`, manipulators, `endl`, width, precision, hex
- locales, facets, `wchar_t`, virtual `streambuf`
- `operator>>`, `getline`, file streams

`<sstream>` is a separate model on another branch. This slice does not add
`models/cxx/sstream` and does not define `std::ostringstream`.

## Why `<iostream>` itself was not replaced

`docs/PIR.md` ("C++ library models"): `<string>` is reached from every
iostream and exception header, so the string class cannot be swapped.
iostreams stay libstdc++'s code. A drop-in replacement would be a file named
`iostream` (or `ostream`) on the model include path. The pir stage only puts
headers there after they are embedded from `CMakeLists.txt` and written out
by `write_cxx_models` (`stage.cpp` passes that directory as `-isystem`). Both
of those files belong to other slices. Doing it here would also mean defining
`_GLIBCXX_IOSTREAM` so libstdc++'s header is skipped. That header pulls in
`ios`, `ostream`, `istream`, `streambuf`, and `string`. A short file that
only sets the include guard would hide that graph and break every unit which
includes `<iostream>`. This header does not set those guards.

The conformance tasks include `prism_ostream.hpp` by a path relative to the
task file. They do not need the pir stage to rewrite `#include <iostream>`.
