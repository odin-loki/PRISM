# Replay when exec is off

`prism::svcomp::replay` (`src/prism/svcomp/replay.cpp`, declared in `include/prism/svcomp.hpp`) already refuses to run task code without `--allow-exec`. This slice leaves that function as it is and locks the gate in `tests/cpp/test_svcomp_replay.cpp`.

## Behavior

With `allow_exec` false, and after a task that does not need nondet values is read, replay returns before it looks up a compiler:

- JSON field `replay` is `notrun`
- `why` is `replay executes task code: re-run with --allow-exec (Law 9)`
- the result is not `false`, `true`, or `ok`, and the JSON text has no SV-COMP false verdict (`false` or `false(<prop>)`)

The data-model argument is not consulted on that path. `ILP32` does not skip the gate and does not add `-m32` while exec is off. The default stays **LP64**: the parameter default is `"LP64"`, and `-m32` is added only when `is_ilp32` is true and exec is allowed.

A missing compiler is also `notrun` (`why`: `no C compiler (clang/gcc) on PATH`). That lookup runs only after exec is allowed.

A task that calls `__VERIFIER_nondet_*` with no nondet values is still `unsupported`, including when exec is off. That check stays in front of the exec gate. The new test uses a tiny `main` with no nondet call, so the gate is what answers.

## Tests

`tests/cpp/test_svcomp_replay.cpp` is listed in `CMakeLists.txt` `_prism_test_sources` (the test target does not glob). It calls the public `replay`:

- `allow_exec` false, data model `LP64`, a tiny C file in a temp directory: `replay` is `notrun`, `why` mentions `--allow-exec`, and the JSON text contains no false verdict.
- The same call with data model `ILP32` and `allow_exec` false is still `notrun`.

## Left alone

`replay.cpp` was not edited. Also left alone: `cli_svcomp.cpp`, `decide.cpp`, `witness.cpp`, `stage.cpp`, the default data model, and `third_party/`.
