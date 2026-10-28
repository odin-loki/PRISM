# filesystem compare

Branch `finish/cxx-filesystem-compare`, base `020c4b6ffaf889df75f6fe7beb103349d38fbfaf`.

`path::compare(const path&)` is declared on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `filename()`, `extension()`, `stem()`, `parent_path()`, `operator/`,
and the length checks are unchanged. A length that does not fit is still
`__prism_check` then `__builtin_trap`. The header still has one
`::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`.

## What compare returns

`compare` reads the characters each path already stores. It returns `-1`,
`0`, or `1`. It does not allocate, so it has no `::operator delete(`.

- An empty path compared with an empty path returns `0`. Both lengths are
  0, and neither pointer is read.
- `a` compared with `a` returns `0`.
- `a` compared with `b` returns `-1`. `b` compared with `a` returns `1`.
  Each stored byte is read as an `unsigned char`, so the order is the order
  of those bytes.
- `a` compared with `ab` returns `-1`. `ab` compared with `a` returns `1`.
  When one stored sequence is a prefix of the other, the shorter one is less.
- `a/..` compared with `a/..` returns `0`. `a/..` compared with `.` is not
  `0`. `..` compared with `..` returns `0`. The two dots stay two characters.
  `compare` does not remove them.

`compare` does not ask the operating system whether a file is there. Its
body does not call `__prism_check`.

## What still falls back

`lexically_normal` is still omitted. Collapsing `..` would invent a path the
caller did not write.

`exists`, `file_size`, `status`, `is_regular_file`, `is_directory`,
`current_path`, and `absolute` are still not declared. A stub that returns
true is a fake file. A use of an omitted name fails to compile against this
header, and the pir stage lowers that translation unit again with libstdc++
(`extra.cxx_models`).

The header does not include `<string>`, `<string_view>`, `<memory>`, or
`<mutex>`.

## Wiring

`CMakeLists.txt` is unchanged. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/filesystem`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_filesystem.cpp`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` is unchanged.

## Tests

`tests/cpp/test_cxx_filesystem.cpp` reads `path::compare`. Two empty lengths
return `0` before any `_M_str` load. The body orders stored bytes and
returns `-1`, `0`, or `1`. It does not contain `::operator delete(`,
`__prism_check`, `exists`, `stat(`, a quoted `..`, or `lexically_normal`.
The whole header still has one `::operator delete(`. `filename()` of an
empty path and `stem()` of an empty path still return `path()`. The length
message `std::filesystem::path length does not fit` is still followed by
`__builtin_trap`. No existing test required `compare` to be absent.
`prism_tests` was not run, so those `CHECK`s were not executed.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, and stem lines. Compare lines
matched too: an empty path equals an empty path, an empty path is less than
`a`, `a` is less than `b`, `a` is less than `ab`, `a/..` equals `a/..`,
`a/..` is not `.`, and `..` equals `..`. The printed values are the signs,
so a libstdc++ magnitude other than `-1` or `1` still matches. ASan stayed
quiet. Both binaries returned 0.

The same compiler and sanitizers ran
`testdata/conformance/libc-models/filesystem_contracts.cpp` (`fs_compare_true`
with the existing filename, join, and stem contracts) and
`testdata/conformance/prism/cxx/cxx_filesystem_true.cpp`, once against the
model and once against libstdc++. Both binaries returned 0. The yml still
records `cxx_filesystem_true: true`. That expected value is a YAML boolean.
`prism_tests` was not run.
