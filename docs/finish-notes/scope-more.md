# scope-more

`skip_dir()` now also skips these exact generated-tree directory names: `cmake-build-release`, `.cache`, `coverage`, `htmlcov`, `.gradle`, `.cxx`, and `DerivedData`. `cmake-build-debug` was already in `skip_dirs()`.

These are exact names. A directory whose name only contains one of those words stays in scope (`cache`, `mycoverage`, `htmlcoverage`, `gradle`, `cxx`, `myDerivedData`, `cmake-build-release-old`). `testdata`, `src`, `include`, `proofs`, `tests`, and `docs` stay in scope, and so do names that only contain those words (`mytestdata`, `mysrc`, `includes`, `myproofs`, `mytests`, `mydocs`).

`cmake-build-release` is skipped because it is listed, not because of the `build*` prefix. That prefix is unchanged: `build` and `build-release` are skipped, and `rebuild` / `mybuilddir` are not.

`prism-out-gui` is still not a set entry. `skip_dir()` returns true because the name starts with `prism-out`.

The existing doctest (`tests/cpp/test_main.cpp`, `scope: a CMakeFiles path component is skipped, src is not`) lists the new names and the near-misses. A `coverage` path component is skipped, including under `src/`. A path under `src/` or `docs/` with no skipped component is not.
