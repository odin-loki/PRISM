# scope

`skip_dir()` now also skips directory names that are never source: `CMakeFiles`, `.vs`, `.idea`, `bazel-bin`, `bazel-out`, `bazel-testlogs`, `dist`, and `cmake-build-debug`. These are exact names in `skip_dirs()`.

`prism-out-gui` is not a new entry. `skip_dir()` already returns true for any name that starts with `prism-out`, and the existing scope doctest already expects `prism-out-gui` to be skipped. Adding it to the set would be redundant.

`compile_commands` is a file (`compile_commands.json`), not a directory, so it is not in the skip list.

Names that only contain `build` in the middle stay in scope. The existing prefix rule is unchanged: a directory is skipped when its name is `build` or starts with `build` (`build-release`), and `rebuild` / `mybuilddir` are not. `cmake-build-debug` is skipped because it is listed, not because of that prefix; `cmake-build-release` is not skipped.

`testdata`, `src`, `include`, `proofs`, `tests`, and `docs` are not skipped.

`tests/cpp/*.cpp` is an explicit source list in `CMakeLists.txt`, not a glob, so the new case is in the existing scope doctest (`tests/cpp/test_main.cpp`, `scope: a CMakeFiles path component is skipped, src is not`). A path with a `CMakeFiles` component is skipped, including `src/CMakeFiles/...`. A path under `src/` with no skipped component is not.
