# filesystem is_absolute

Branch `finish/cxx-filesystem-absolute`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`path::is_absolute()` and `path::is_relative()` are each declared once on the
existing header `src/prism/pir/models/cxx/filesystem`. The header is still a
file, not a directory. `compare`, `stem`, `filename`, `extension`,
`parent_path`, `operator/`, and `replace_extension` are unchanged. The header
still has one `::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`.

## What the character tests return

`is_absolute` reads one stored character. It does not allocate, so its body
has no `::operator delete(`. It does not ask the operating system whether a
file is there, and it does not walk a directory.

- An empty path returns false from `is_absolute`. The function returns when
  `_M_len == 0`, before it loads the stored pointer.
- A path whose first character is `/` is absolute. `/`, `/a`, `/a/../b`, and
  `//net/share` are absolute. The two dots stay in the stored characters.
  They are not removed.
- Every other non-empty path is not absolute. `a`, `a/b`, `a/../b`, `C:/a`,
  and `\a` are not absolute.
- `is_relative()` is the opposite: it returns `!is_absolute()`. An empty
  path is relative. That body does not load the stored pointer and does not
  walk the filesystem.

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

`tests/cpp/test_cxx_filesystem.cpp` reads `path::is_absolute` and
`path::is_relative`. Each name is declared once. An empty length returns
false before any `_M_str` use, and the other return compares the first
stored character with `/`. `is_relative` returns `!is_absolute()` and does
not contain `_M_str`, `::operator delete(`, `exists`, `stat(`, `opendir`, or
`lexically_normal`. The whole header still has one `::operator delete(`.
After comments are stripped, `exists`, `absolute`, `lexically_normal`, and
`status` are not identifiers. `compare`, `stem`, `filename`, `extension`,
`parent_path`, `operator/`, and `replace_extension` still have the same
empty-path returns they had before. Compiled alone with clang++ 18.1.3
(`-std=c++23`, doctest main, address and undefined sanitizers), 4 test cases
and 142 assertions passed. `prism_tests` was not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, stem, compare, and
replace_extension lines. Absolute lines matched too: an empty path and `""`
are relative, `/`, `/a`, `/a/../b`, and `//net/share` are absolute, and `a`,
`a/b`, `a/../b`, `C:/a`, and `\a` are relative. `is_relative` is the
opposite on each of those paths. ASan stayed quiet. Both binaries returned 0.

That host compile is not a proof, and it is not a fuzzer `CLEAN` result.
