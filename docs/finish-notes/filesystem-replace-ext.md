# filesystem replace_extension

Branch `finish/cxx-filesystem-replace-ext`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`path::replace_extension(const path&)` is declared on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `compare`, `stem`, `filename`, `extension`, `parent_path`, and
`operator/` are unchanged. A length that does not fit is still
`__prism_check` then `__builtin_trap`. The header still has one
`::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`.

## What replace_extension writes

`replace_extension` rewrites the characters the path already stores. It does
not call the operating system. When the result is a new block, that block
comes from `::operator new`, and the previous block is released by the one
`::operator delete(` in `_M_free`. The function body has no
`::operator delete(`.

- An empty path stays empty. The function returns `*this` when `_M_len == 0`,
  before it loads the stored pointer and before it reads the replacement.
  A non-empty replacement is not written. libstdc++ stores that replacement:
  an empty path replaced with `.md` is `.md` there. This model leaves it
  empty.
- `a/b.txt` replaced with `.md` is `a/b.md`.
- `a/b.txt` replaced with `md` is `a/b.md`. A replacement that does not start
  with `.` gains one dot before it.
- `a/b.txt` replaced with an empty extension is `a/b`. The trailing suffix is
  removed and no dot is left behind.
- `..` replaced with an empty extension stays `..`. `..` replaced with `.md`
  is `...md`. `extension()` does not treat `..` as a suffix, so those two
  dots stay, and the replacement is appended after them.
- `a/b.txt` replaced with `..` is `a/b..`. The two dots are characters. They
  are not removed as a parent directory.
- `a/b.tar.txt` replaced with `.md` is `a/b.tar.md`. Only the trailing
  extension is replaced.
- `.txt` replaced with `.md` is `.txt.md`. A leading-dot name has an empty
  extension, so the replacement is appended.
- A replacement that is the same path is the characters that remain after the
  old suffix is removed. `a/b.txt` replaced with itself is `a/b.a/b`.
- The owned length plus a NUL must fit in `size_t`. A sum that would wrap is
  `__prism_check(0, "MEM-OOB-WRITE", "std::filesystem::path replace_extension length does not fit")`
  and then `__builtin_trap`. The allocation size is that length plus one,
  after the check.

## What still falls back

`lexically_normal` and `lexically_relative` are still omitted. Collapsing
`..` would invent a path the caller did not write.

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
`tests/cpp/test_qa_conformance.cpp` is unchanged. The yml still records
`cxx_filesystem_true: true`. That expected value is a YAML boolean.

## Tests

`tests/cpp/test_cxx_filesystem.cpp` reads `path::replace_extension`. An empty
path returns `*this` before any `__rep` or `_M_str` use. The body calls
`extension()`, quotes `..`, and adds a dot only when the replacement does not
start with `.`. It contains the length `__prism_check` and `__builtin_trap`.
It does not contain `::operator delete(`, `exists`, `lexically_normal`,
`lexically_relative`, or `stat(`. The whole header still has one
`::operator delete(`. `compare`, `stem`, `filename`, `extension`,
`parent_path`, and `operator/` still have the same empty-path and `..`
returns they had before. Compiled alone with clang++ 18.1.3 (`-std=c++23`,
doctest main, address and undefined sanitizers), 3 test cases and 100
assertions passed. `prism_tests` was not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, stem, and compare lines.
Replace lines matched too: `a/b.txt` with `.md` is `a/b.md`, with an empty
extension is `a/b`, and with `md` is `a/b.md`. `..` with an empty extension
stays `..`. `..` with `.md` is `...md`. `a/b.txt` with `..` is `a/b..`.
An empty path with an empty extension stays empty. `a/b.tar.txt` with `.md`
is `a/b.tar.md`. `.txt` with `.md` is `.txt.md`. ASan stayed quiet. Both
binaries returned 0.

A separate driver included the model header under the same sanitizers. An
empty path replaced with `.md` or with `md` stays empty, and `c_str()` of
that path is an empty C string. `a/b.` with an empty extension is `a/b`.
`..` with `md` is `...md`. `a/..` with `.md` is `a/...md`. `.` with `.md`
is `..md`. `...` with an empty extension is `..`. `a/b.txt/` with `.md` is
`a/b.txt/.md`. `foo` with `.md` is `foo.md`, and `foo` with an empty
extension stays `foo`. A copy keeps `a/b.txt` after the original is replaced
with `.md`. `a/b.txt` replaced with itself is `a/b.a/b`. ASan stayed quiet.
The driver returned 0. That empty-path case is not in the shared trace,
because libstdc++ stores `.md`.

The same compiler and sanitizers ran
`testdata/conformance/libc-models/filesystem_contracts.cpp`
(`fs_replace_ext_true` with the existing filename, join, stem, and compare
contracts) and `testdata/conformance/prism/cxx/cxx_filesystem_true.cpp`, once
against the model and once against libstdc++. Both binaries returned 0.
Those contracts use the calls that agree: empty replacement on an empty path,
`a/b.txt` to `a/b.md` and to `a/b`, `md` gaining a dot, and `..` staying
`..`. `prism_tests` was not run.
