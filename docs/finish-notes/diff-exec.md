# diff without allow-exec

Differential testing compiles a harness and runs both scanned functions. That is an exec step (Law 9). `run_diff` already refused it when the process-wide policy is off; this slice locks that row in its own doctest.

## Status with exec off

`diff_pair` reads `sandbox::allowed()`, the flag `run_pipeline` sets from `Config.allow_exec`. When it is false the pair is one finding from `sandbox::exec_notrun("diff", ...)`, status `NOTRUN`. The list is not empty, and the status is not `CLEAN`. `extra["reason"]` is `executes-scanned-code` and `extra["install"]` is the `--allow-exec` hint. The message names the pair (`diff add_a/add_b: executes code from the scanned tree; ...`). File, function, and line are copied from the first function.

The compiler is not consulted on this path, so a missing gcc/clang cannot turn the row into `CLEAN`. When exec is allowed and neither compiler is on `PATH`, the existing branch is still `NOTRUN` with `extra["install"]` = `install gcc or clang` (`tests/cpp/test_diff.cpp`).

## Already in the stage

`src/prism/stages/diff.cpp` was not edited.

- `POINTER` / `OTHER` is `NEEDS-HARNESS` (a harness would invent a buffer or object).
- A harness deadline is `TIMEOUT` with the message `diff harness timed out (not agreement, not a proof)`.
- Sampled agreement is `CLEAN` with `no disagreement on sampled inputs (not a proof)`.

## Test

`tests/cpp/test_diff_exec.cpp` is listed in `_prism_test_sources` next to `test_diff.cpp` (the test target lists sources and does not glob). `tests/cpp/test_diff.cpp` was not edited.

`diff exec: two SCALAR functions are NOTRUN when allow-exec is off` builds two `SCALAR` functions named `add_a` and `add_b`, holds `sandbox::Policy(false)`, and calls `run_diff`. It expects one `NOTRUN` row with the exec reason, not `CLEAN` and not the missing-compiler message. The policy is restored to the value from before the case.

`prism_tests` was not executed in this slice (no cmake/ninja on this tree).
