# Lint hits are defects, not proofs

Slice `finish/lint-failed`. A checker finding is `laws::FAILED`. It is not
`PROVED`, `PROVED-UNBOUNDED`, `PROVED-CERTIFIED`, `PROVED-ASSUMING`,
`BOUNDED`, or `CLEAN`. `laws::is_proof` is false for that status.

## Recorder

`prism::lint_add` in `src/prism/checkers_dispatch.cpp` is the function every
regex checker uses (`checkers_core.cpp`, `checkers_api.cpp`,
`checkers_cxx.cpp`). The Clang-AST checker calls the same function and then
sets `extra.engine`. `lint_add` sets:

- `stage` = `lints`
- `status` = `laws::FAILED`
- `strength` = `laws::STRENGTH_FINDS`

No path in those checkers stores `CLEAN` or `PROVED` on a hit. Production
code was left unchanged.

## A layer that cannot run

`prism::astlint::run_lints_ast` with `Backend::Process` and no clang binary
writes one `laws::NOTRUN` row (`extra.layer` = `clang-ast`). That row means
the AST layer did not run. Regex lints still run, and a `gets()` call in the
same file stays `FAILED`. `NOTRUN` is not a pass and it is not "no defect".

## Test

`tests/cpp/test_lint_status.cpp` is one new line in `_prism_test_sources`.
`prism_tests` was not executed in this worktree: this slice does not
configure or build.

- `lint_add records a hit as FAILED and not a proof`
- `run_lints on a gets call is FAILED and not a proof`
- `a lint layer that cannot run is NOTRUN and the regex hit stays FAILED`
