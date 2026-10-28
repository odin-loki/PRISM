# filesystem has_extension

Branch `finish/cxx-filesystem-extension`, base `ada5e223051df081598f9acb6487ce3d3ce39e86`.

`path::has_extension()` is declared once on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `compare`, `stem`, `filename`, `extension`, `parent_path`,
`operator/`, `replace_extension`, `is_absolute`, `is_relative`, `empty`,
`has_root_directory`, and `has_filename` are unchanged. The header still has
one `::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`.

## What the character test returns

`has_extension` reads stored characters. It does not call `filename()` or
`extension()`, and it does not allocate, so its body has no
`::operator delete(`. It does not ask the operating system whether a file is
there, and it does not walk a directory.

- An empty path returns false. The function returns when `_M_len == 0`,
  before it loads the stored pointer.
- A non-empty path whose last character is `/` returns false. `/`, `a/`,
  and `a/b/` have no extension. The index is `_M_len - 1`, which is inside
  the owned length. The terminating NUL is not read.
- Otherwise only the filename is examined: the characters after the last
  `/`. A dot in an earlier component is not an extension, so `a.b/c` has
  none.
- A filename with no `.` has no extension. `a`, `a/b`, `a/../b`, `C:/a`,
  `\a`, `//net`, and `//net/share` have none.
- A filename that is `.` or `..` has no extension. `.`, `..`, `a/.`, and
  `a/..` have none. The two dots stay in the stored characters. They are
  not removed.
- A filename whose only `.` is the first character has no extension.
  `.txt` and `a/.txt` have none.
- A later `.` means the path has an extension. `a/b.txt`, `b.txt`,
  `a/b.tar.txt`, `.txt.md`, `a.`, `...`, `a/../b.txt`, `/a/../b.txt`,
  `C:/a.txt`, and `\a.txt` have one. `...` is not the special name `..`;
  the second `.` is an extension.

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

`CMakeLists.txt` is unchanged. No new test file was added, so no source-list
line was added and none was dropped. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/filesystem`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_filesystem.cpp`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` is unchanged. The yml still records
`cxx_filesystem_true: true`. That expected value is a YAML boolean.

## Tests

`tests/cpp/test_cxx_filesystem.cpp` reads `path::has_extension`. The name is
declared once. An empty length returns false before any `_M_str` use. A
trailing `/` returns false from the last stored character. `.` and `..`
return false. A `.` after the first character of the filename returns true.
The body has no `_M_str[_M_len]`, `filename()`, `extension()`,
`::operator delete(`, `::operator new(`, `exists`, `stat(`, `opendir`,
`lexically_normal`, or `status`. The whole header still has one
`::operator delete(`. After comments are stripped, `exists`, `absolute`,
`lexically_normal`, and `status` are not identifiers. `compare`, `stem`,
`filename`, `extension`, `parent_path`, `operator/`, `replace_extension`,
`is_absolute`, `is_relative`, `empty`, `has_root_directory`, and
`has_filename` still have the same empty-path returns they had before.
Compiled alone with clang++ 18.1.3 (`-std=c++23`, doctest main, address and
undefined sanitizers), 7 test cases and 283 assertions passed. `prism_tests`
was not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, stem, compare,
replace_extension, absolute, root-directory, and filename lines. Extension
lines matched too: an empty path and `""` are false, `/`, `a/`, and `a/b/`
are false, `a`, `a/b`, `a.b/c`, `.`, `..`, `a/.`, `a/..`, `.txt`, `a/.txt`,
`a/../b`, `C:/a`, `\a`, `//net`, and `//net/share` are false, and
`a/b.txt`, `b.txt`, `a/b.tar.txt`, `.txt.md`, `a.`, `...`, `a/../b.txt`,
`/a/../b.txt`, `C:/a.txt`, and `\a.txt` are true. ASan stayed quiet. Both
binaries returned 0.

That host compile is not a proof, and it is not a fuzzer `CLEAN` result.
