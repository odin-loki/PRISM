# filesystem has_filename

Branch `finish/cxx-filesystem-filename`, base `c164ba71c67bf51330687627679f49368a361928`.

`path::has_filename()` is declared once on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `compare`, `stem`, `filename`, `extension`, `parent_path`,
`operator/`, `replace_extension`, `is_absolute`, `is_relative`, `empty`, and
`has_root_directory` are unchanged. The header still has one
`::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`.

## What the character test returns

`has_filename` reads one stored character. It does not call `filename()`,
and it does not allocate, so its body has no `::operator delete(`. It does
not ask the operating system whether a file is there, and it does not walk
a directory.

- An empty path returns false. The function returns when `_M_len == 0`,
  before it loads the stored pointer.
- A non-empty path whose last character is `/` returns false. `/`, `a/`,
  and `a/b/` have no filename. The index is `_M_len - 1`, which is inside
  the owned length. The terminating NUL is not read.
- Every other non-empty path returns true. `a`, `/a`, `a/b`, `.`, `..`,
  `a/..`, `a/../b`, `/a/../b`, `C:/a`, `\a`, `//net`, `//net/share`,
  `a/b.txt`, and `.txt` have a filename. The two dots stay in the stored
  characters. They are not removed.

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

`tests/cpp/test_cxx_filesystem.cpp` reads `path::has_filename`. The name is
declared once. An empty length returns false before any `_M_str` use, and
the other return compares the last stored character with `/`. The body has
no `_M_str[_M_len]`, `filename()`, `::operator delete(`, `::operator new(`,
`exists`, `stat(`, `opendir`, `lexically_normal`, or `status`. The whole
header still has one `::operator delete(`. After comments are stripped,
`exists`, `absolute`, `lexically_normal`, and `status` are not identifiers.
`compare`, `stem`, `filename`, `extension`, `parent_path`, `operator/`,
`replace_extension`, `is_absolute`, `is_relative`, `empty`, and
`has_root_directory` still have the same empty-path returns they had before.
Compiled alone with clang++ 18.1.3 (`-std=c++23`, doctest main, address and
undefined sanitizers), 6 test cases and 227 assertions passed. `prism_tests`
was not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, stem, compare,
replace_extension, absolute, and root-directory lines. Filename lines
matched too: an empty path and `""` are false, `/`, `a/`, and `a/b/` are
false, and `/a`, `a`, `a/b`, `.`, `..`, `a/..`, `a/../b`, `/a/../b`, `C:/a`,
`\a`, `//net`, `//net/share`, `a/b.txt`, and `.txt` are true. ASan stayed
quiet. Both binaries returned 0.

That host compile is not a proof, and it is not a fuzzer `CLEAN` result.
