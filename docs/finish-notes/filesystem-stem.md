# filesystem stem

Branch `finish/cxx-filesystem-stem`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`path::stem()` is declared on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `filename()`, `extension()`, `parent_path()`, `operator/`, and the
length checks are unchanged. A length that does not fit is still
`__prism_check` then `__builtin_trap`. The header still has one
`::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`.

## What stem returns

`stem()` is `filename()` with `extension()` removed. It copies characters the
caller already wrote. It does not allocate with its own `operator delete`.

- An empty path's stem is an empty path. The function returns `path()` when
  `_M_len == 0`. It does not spell a single dot.
- `stem()` of `a/b.txt` is `b`.
- `stem()` of `a/b` is `b`.
- `stem()` of `.` is `.`.
- `stem()` of `..` is `..`. A component written as `..` stays `..`.
- `stem()` of `.txt` is `.txt`. A filename that is only a dot, or that starts
  with a dot and has no later dot, has an empty extension, so it is its own
  stem.
- `stem()` of `a/b.tar.txt` is `b.tar`. The removed suffix is the same
  extension `extension()` already returns, the part from the last dot.

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

`tests/cpp/test_cxx_filesystem.cpp` reads `path::stem()`. An empty path
returns `path()`, the body does not spell a dot, and the body calls
`filename()` and `extension()`. It does not contain `::operator delete(`.
The whole header still has one such call. No existing test required `stem`
to be absent.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 (`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -O1`)
compiled it twice (`-isystem` the model directory, and without it). Stdout
matched, including the previous filename, extension, parent, and join lines.
Stem lines matched too: an empty stem is empty, `a/b.txt` yields `b`, `a/b`
yields `b`, `.` yields `.`, `..` yields `..`, `.txt` yields `.txt`, `a/..`
yields `..`, and `a/b.tar.txt` yields `b.tar`. ASan stayed quiet.

The same compiler and sanitizers ran
`testdata/conformance/libc-models/filesystem_contracts.cpp` (`fs_stem_true`
with the existing filename and join contracts) and
`testdata/conformance/prism/cxx/cxx_filesystem_true.cpp`, once against the
model and once against libstdc++. Both binaries returned 0. The yml still
records `cxx_filesystem_true: true`. That expected value is a YAML boolean.
`prism_tests` was not run.
