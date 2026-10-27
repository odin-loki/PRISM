# scope-pods

`skip_dir()` now also skips these exact directory names: `Pods`, `Carthage`, `.swiftpm`, and `.bundle`. They are entries in `skip_dirs()`. The existing names stay in the set, including `.git`, `node_modules`, `third_party`, `build`, `cmake-build-release`, `.cache`, and `coverage`.

A directory is skipped when its name is exactly one of those four. A directory whose name only contains one of those words stays in scope: `myPods`, `swiftpm`, `bundle`, and `PodsOld`. `src` and `docs` stay in scope.

The `build*` and `prism-out*` prefix rules are unchanged. `cmake-build-release` remains a listed name.

## Tests

`tests/cpp/test_scope_pods.cpp` calls `prism::scope::skip_dir` (`include/prism/scope.hpp`). `Pods`, `Carthage`, `.swiftpm`, and `.bundle` are skipped. `myPods`, `swiftpm`, `bundle`, `PodsOld`, `src`, and `docs` are not.

`CMakeLists.txt` gained one line in `_prism_test_sources`: `tests/cpp/test_scope_pods.cpp`. The test target lists sources and does not glob. `tests/cpp/test_main.cpp` is unchanged.

`prism_tests` was not executed. This slice does not configure or build.
