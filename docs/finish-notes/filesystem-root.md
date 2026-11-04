# filesystem has_root_directory

Branch `finish/cxx-filesystem-root`, base `63466ba8cbd95157f6dff251c7c8b0475639c4a8`.

`path::has_root_directory()` is declared once on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. `compare`, `stem`, `filename`, `extension`, `parent_path`,
`operator/`, `replace_extension`, `is_absolute`, `is_relative`, and `empty`
are unchanged. The header still has one `::operator delete(`, in `_M_free`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`.

## What the character test returns

`has_root_directory` reads one stored character. It does not allocate, so
its body has no `::operator delete(`. It does not ask the operating system
whether a file is there, and it does not walk a directory.

- An empty path returns false. The function returns when `_M_len == 0`,
  before it loads the stored pointer.
- A path whose first character is `/` has a root directory. `/`, `/a`,
  `/a/../b`, `//net`, and `//net/share` have one. The two dots stay in the
  stored characters. They are not removed.
- Every other non-empty path returns false. `a`, `a/b`, `a/../b`, `C:/a`,
  and `\a` do not have a root directory.

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

`tests/cpp/test_cxx_filesystem.cpp` reads `path::has_root_directory`. The
name is declared once. An empty length returns false before any `_M_str`
use, and the other return compares the first stored character with `/`.
The body has no `::operator delete(`, `::operator new(`, `exists`, `stat(`,
`opendir`, `lexically_normal`, or `status`. The whole header still has one
`::operator delete(`. After comments are stripped, `exists`, `absolute`,
`lexically_normal`, and `status` are not identifiers. `compare`, `stem`,
`filename`, `extension`, `parent_path`, `operator/`, `replace_extension`,
`is_absolute`, `is_relative`, and `empty` still have the same empty-path
returns they had before. Compiled alone with clang++ 18.1.3 (`-std=c++23`,
doctest main, address and undefined sanitizers), 5 test cases and 181
assertions passed. `prism_tests` was not run.

`tests/cxx_models/filesystem_trace.cpp` includes `<filesystem>`. clang++
18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra`)
compiled it twice (`-isystem` the model directory, and without it). The
dependency dump names the model header. Stdout matched, including the
previous filename, extension, parent, join, stem, compare,
replace_extension, and absolute lines. Root-directory lines matched too: an
empty path and `""` are false, `/`, `/a`, `/a/../b`, `//net`, and
`//net/share` are true, and `a`, `a/b`, `a/../b`, `C:/a`, and `\a` are
false. ASan stayed quiet. Both binaries returned 0.

That host compile is not a proof, and it is not a fuzzer `CLEAN` result.
