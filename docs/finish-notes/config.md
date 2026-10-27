# Config which and install hints

Slice `finish/config-which`. Edits are `src/prism/config.cpp`, the
`Config::which` / `which_adapter` cases in `tests/cpp/test_adapters.cpp`,
and one comment in `tests/cpp/test_main.cpp`. `include/prism/config.hpp`
is unchanged.

## Behaviors

1. `adapter_install` already names the C++ tool. A pinned stage returns
   `prism-deps tool <component> (pinned in third_party/MANIFEST.toml)`.
   Anything else returns `<stage> is a system tool, not pinned by
   prism-deps (see third_party/MANIFEST.toml)`. The string does not tell
   the user to run a Python fetch script. Locked by
   `tests/cpp/test_main.cpp`, `tests/cpp/test_deps.cpp`, and
   `tests/cpp/test_adapters.cpp`. No change in this slice.
2. `Config::which` does not treat a directory or a non-executable file as
   a tool.
   - POSIX was already a regular file plus `access(..., X_OK)`, in PATH
     order. Unchanged. `tests/cpp/test_adapters.cpp` case
     `config: which needs an executable file, like shutil.which`.
   - Windows used to call `SearchPathA` and stop at the first match, so a
     directory or any same-named file counted, and a bare name never
     looked for `.bat` or `.cmd`. It now walks the same directories
     SearchPath walks: the application directory, the current directory,
     the system directory, the 16-bit `System` directory, the Windows
     directory, then `PATH`. A hit must be a regular `.exe`, `.bat`, or
     `.cmd`. A bare name still takes `.exe` from anywhere in that list
     before `.bat` or `.cmd`, so an existing `.exe` (including
     `System32\bash.exe` when PATH was replaced) stays where it was.
     `.bat` then `.cmd` are used only when no `.exe` exists, earlier
     directories first. A name that already ends in `.exe` / `.bat` /
     `.cmd` is that file. Any other dotted name (`tool.12`) still gets
     those suffixes appended, so `tool.12.exe` is found. A Microsoft
     Store python stub under `\WindowsApps\` is still skipped (Law 1).
3. `which_adapter` already expands a leading `~`, `~/`, or `~\` through
   `expand_user` before the `--tool` existence check. The Windows case
   now also checks `~\klee`. An explicit `--tool` path is still any
   regular file the user named (on Windows a path with no extension also
   tries a sibling `.exe`). The executable-kind rule is `which`, which
   is the PATH step.

Search order of `which_adapter` is unchanged: `--tool`, then the pinned
`prism-deps tool` build, then `which`.

## Left unchanged on purpose

- Manifest pins and `prism-deps licence-check`. No pin or licence edit.
- `include/prism/config.hpp`. Nothing in this slice needed a new declaration.
- `src/prism/solver/util.cpp` still has its own `SearchPathA` walk for
  solver tools. That file is outside this slice.
- `tests/cpp/test_diff.cpp` still says `Config::which` is SearchPath. The
  directory order that comment depends on (system directories before
  PATH, no gcc/clang there) is the same. The file is not part of this slice.
- `docs/MSVC_PORT.md` still describes the old `SearchPathA` call. Same
  directory order, including the Store-stub probe.

## Tests

`tests/cpp/test_adapters.cpp`

- `config: which on Windows accepts exe, bat, and cmd only` — a directory,
  a directory named `.exe`, and a file with no executable extension are
  missing; an earlier `.cmd` beats a later `.bat`; a `.bat` in that same
  earlier directory beats the `.cmd`; a later `.exe` beats both; an
  earlier plain file does not hide a later `.exe`; `tool.12` resolves to
  `tool.12.exe`.
- `config: --tool beats the pinned build, which beats PATH; PATH is last`
  — `~\klee` expands the same way as `~/klee` on Windows.

Not executed here: no `prism_tests` binary for this worktree, and the
build stays off OneDrive.
