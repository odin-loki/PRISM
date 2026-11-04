# filesystem has_stem

`path::has_stem()` is on the existing header
`src/prism/pir/models/cxx/filesystem`. The header is still a file, not a
directory. The header still has one `::operator delete(`, in `_M_free`.

Nothing here is `PROVED`.

## What has_stem returns

`has_stem()` was `!stem().empty()`. That built a `stem()` copy, which
allocates, and the function was not `noexcept`. libstdc++ declares it
`noexcept`. It is now declared `noexcept` and defined out of line as
`return has_filename();`.

- An empty path has no stem. `has_filename()` returns false before the
  stored pointer is loaded.
- A non-empty path whose last character is `/` has no stem.
- `.` and `..` have a stem: those filenames are their own stems.
- `.txt` has a stem: a filename whose only dot is the first character is
  its own stem.
- `a/b.txt` has a stem, `b`.

Every filename the model stores has a non-empty stem, so the answer is the
same as `!stem().empty()`. The function reads no character that
`has_filename()` does not read and does not allocate. It does not walk the
filesystem.

## What stayed

`stem()`, `filename()`, `extension()`, `has_filename()`, `has_extension()`,
`has_parent_path()`, `replace_extension()`, and `compare()` are unchanged.
`exists`, `status`, `absolute`, and `lexically_normal` are still not
declared.

## Tests

`tests/cpp/test_cxx_filesystem.cpp` reads `has_stem` as text. The
declaration and the definition each appear once, and the body is
`return has_filename();` with no `_M_str`, no `_M_len`, no `stem()`, and no
allocation. `!stem().empty()` is gone from the header. The empty-path early
return in `has_filename()` still comes before its first `_M_str` read.

`tests/cxx_models/filesystem_trace.cpp` prints `has_stem()` for nineteen
paths: empty (default-constructed and from `""`), `/`, `/a`, `a`, `a/b`, `a/`, `a/b/`, `.`, `..`, `a/..`,
`a/../b`, `/a/../b`, `C:/a`, `\a`, `//net/share`, `//net`, `a/b.txt`, and
`.txt`. The trace binary against the model and the trace binary against
libstdc++ print the same lines.
