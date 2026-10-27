# filesystem slice

Branch `finish/cxx-filesystem`, base `e57213d1404ec1821382910b632e2815547aacf9`.

`std::filesystem` is the extensionless file `src/prism/pir/models/cxx/filesystem`.
A directory of that name would make `#include <filesystem>` fail, and the unit
would fall back to libstdc++. The include guard is `_GLIBCXX_FILESYSTEM`, the
file is a `#pragma GCC system_header`, and the type lives in
`namespace std::filesystem`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml records
the contract the harness asks for. `expected` is a YAML boolean. Nothing here
is `PROVED`.

## What the model stores

`path` owns a copy of the characters the caller wrote. The bytes sit in a
block from `::operator new`. `_M_free` is the one `::operator delete` in the
header. The destructor is that call, so the owned copy is freed once. A move
stores `nullptr` in the source and does not free; the source destructor then
sees no block. A copy allocates its own block.

An empty path does not allocate. `size()` is 0 and `empty()` is true.
`size()` is the character count of that owned copy (what `native().size()`
would be). This header does not include `<string>`, so it does not return a
`std::string`.

`path(const char (&)[N])` copies at most `N` characters and stops at the first
NUL. `path(const char*)` reads only inside `__prism_obj_size` of that pointer.
A null pointer is `__prism_check(..., "FUNC-CONTRACT", ...)`, and the path
stays empty. A missing terminator inside the object is `MEM-OOB-READ`, and
this does not invent a length.

- `filename()` of an empty path is an empty path. A filename of one dot is
  only the path whose characters are that dot.
- `filename()` of `a/b` is `b`.
- `extension()` of `a/b.txt` is `.txt`. A filename of `.` or `..`, or a name
  whose only dot is the first character, has an empty extension.
- `parent_path()` of `a/b` is `a`.
- `operator/` inserts one `/` when the two sides would otherwise meet without
  a separator. Both sides' components stay. A component written as `..` stays
  `..`, so `a / ..` is `a/..`. An absolute right-hand side does not replace
  the left-hand side: that replacement would drop the left-hand component.
  `a / /b` is `a/b`.

## What falls back

`lexically_normal` is omitted. Collapsing `..` would invent a path the caller
did not write.

`exists`, `file_size`, `status`, `is_regular_file`, `is_directory`,
`current_path`, and `absolute` are omitted. Those names ask the operating
system whether a file is there. A stub that returns true is a fake file. A
use of an omitted name fails to compile against this header, and the pir
stage lowers that translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed. Iterators, `string()` / `wstring()`, and directory
walking are omitted the same way.

The header does not include `<string>`, `<string_view>`, `<memory>`, or
`<mutex>`.

`src/prism/bmc.cpp` still returns "is not a filesystem model" when the source
text contains `std::filesystem`, before any header is inlined. This slice does
not edit `bmc.cpp` or `src/prism/pir/stage.cpp`. `stage.cpp` already writes
every embedded `cxx/` model and passes that directory as `-isystem`.

## Wiring

`PRISM_PIR_MODELS` gained one line:

```
src/prism/pir/models/cxx/filesystem
```

`_prism_test_sources` gained `tests/cpp/test_cxx_filesystem.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"filesystem"` with the other
public model headers. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_filesystem.cpp` reads the header. It checks the file is a
regular file, the guard and pragma and `namespace std::filesystem` are
present, and `<string>`, `<string_view>`, `<memory>`, and `<mutex>` are not
included. After comments are stripped, `exists`, `file_size`, `status`,
`is_regular_file`, `is_directory`, `current_path`, `absolute`, and
`lexically_normal` are not identifiers. `filename()` of a zero length returns
an empty path and that function does not spell a dot. The destructor calls
`_M_free`, and that function is the only `::operator delete`. The move
constructor clears the source and does not free. Compiled alone with clang++
18.1.3 (`-std=c++23`, doctest main), 40 assertions passed. `prism_tests` was
not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++ 18.1.3
with `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -O1`
compiled it twice (`-isystem` the model directory, and without it). Stdout
matched: empty `filename()` is empty, `a/b` yields `b`, `a/b.txt` yields
`.txt`, `parent_path` of `a/b` is `a`, `a / b` is `a/b`, and `a / ..` is
`a/..`.

A separate driver included the header by path under the same sanitizers.
Empty `size()` is 0, the trace facts hold, `a/b / ..` stays `a/b/..`, a copy
keeps its own block after the original's copy dies, move leaves the source
empty, and a null `const char*` is one `FUNC-CONTRACT` check and an empty
path. ASan stayed quiet.

`testdata/conformance/prism/cxx/cxx_filesystem_true.cpp` checks `filename` of
`a/b` and of an empty path (`memsafety`, true). There is no false task that
calls `exists`. `testdata/conformance/libc-models/filesystem_contracts.cpp`
is the `#include <filesystem>` harness: the same filename facts, plus
extension, parent, and the join that keeps `..`.
