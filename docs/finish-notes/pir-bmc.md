# pir vs bmc retirement gates

Slice base: `e59a8b3df966e33c78ffbd9ced0a8fc90465b16d`.
WIP reviewed: `origin/cursor/pir-bmc-superset-gates-0791` at `a550400209c4a5bc17d7cb4dbce5776f5ab32dde`.

The hand bmc encoder is not retired. Nothing in this slice flips a retirement flag, deletes `bmc`, or claims the testdata superset gate passed. `prism-qa pir-vs-bmc` was not run on `testdata/`.

## Landed

`prism-qa pir-vs-bmc` now counts and prints the retirement counters from the matrix:

- Hard conflicts: `PROVED*` vs `FAILED` in either direction. `PROVED*` is `PROVED`, `PROVED-UNBOUNDED`, `PROVED-ASSUMING`, and `PROVED-CERTIFIED`.
- Superset gaps: `matrix[PROVED*, absent] + matrix[PROVED*, NEEDS-HARNESS]`. Derived from those cells after the matrix is filled. `BOUNDED` is not a `PROVED*` (Law 2). `ERROR` and `UNKNOWN` (including `NOTRUN` / `TIMEOUT`) are not gap cells.
- Agreement: `same bucket where both report: agree_both/both_report`. Same non-absent bucket over functions where neither side is absent. Printed in the text report and in JSON (`agree_both`, `both_report`). It is not an exit condition; the backlog says to raise it and gives no cutoff.

`--gate` exits 1 when either retirement counter is nonzero, after the text report and any `--json` file are written. Stderr is one line per failed counter. Exit 0 with `--gate` means both counters are 0. Exit 2 is still a tool failure (unknown option, unreadable report, missing `prism` binary, no `report.json`). Without `--gate` the process still prints the counters and exits 0, including when they fail.

JSON adds `agree_both`, `both_report`, `superset_gaps`, and `gate` with `hard_conflicts`, `superset`, and `overall` set to `PASS` or `FAIL`.

An empty report has both counters at 0, so `--gate` exits 0. That is the counter definition, not a corpus result.

Doctest `pir-vs-bmc retirement gates, superset cells and exit codes` in `tests/cpp/test_qa.cpp` (and the unbuilt reference copy `tests/cpp/test_qa_static.cpp`) locks the buckets, the two gap cells, the agreement line, the JSON `gate` object, and the exit codes on synthetic findings. It does not run the corpus. The older fixture still expects one hard conflict, agreement `1/3`, and superset gaps `0`.

## Rejected

`docs/ENGINEERING_BACKLOG.md` (WIP one-line edit). Out of scope for this slice. The existing sentence is still right: the doctest is synthetic, and `--gate` is not wired into CI.

`src/prism/pir/stage.cpp` (`write_ok_recovery_tu`, `try_ok_recovery`, `format_params`, `Lowered::include_flags`). Not landed. No `PIR-OK-RECOVERY` hunk.

When clang fails, `run_unit` already writes the failure down: `ERROR`, or `NOTRUN` with an install hint when a header is missing (Law 7). The WIP runs first, before that classification. It parses the original file with `extract_functions`, writes a new translation unit of include lines plus reconstructed `*_ok` bodies, and if that slice lowers it returns `run_unit` on the slice. Findings keep the original relative path (`Unit::rel` is copied; only `path` changes), so a `PROVED` or `FAILED` from the slice is filed as the original function. The unit-level `ERROR` it also appends has no function name, so `pir-vs-bmc` ignores it.

That slice is a different program. It drops every function not named `*_ok`, plus macros, typedefs, globals, and helpers those functions call. A `PROVED` on it is not a check of the unit clang rejected, and it would shrink `PROVED*|absent` without pir covering bmc. A missing-header `NOTRUN` would be replaced whenever the slice compiles without that header (Law 7). Re-running the checker on a rewritten file is also not a small hunk, and another change is in the same file.

An honest per-function stand-in was not added either. `NOTRUN` would move `PROVED*|absent` into `PROVED*|UNKNOWN` and make the superset counter look better without a check. `NEEDS-HARNESS` is the wrong reason (and it is itself a gap cell). `BOUNDED` would claim a bounded check that did not run.

## Command to run later

On an ext4 checkout of `finish/pir-bmc-gates` (`~/PRISM` after sync). Not OneDrive, and not `/mnt/c`.

```bash
cd ~/PRISM
scripts/wsl_build.sh
cmake --build build-wsl --target prism prism-qa prism_tests -j"$(nproc)"
./build-wsl/prism_tests --test-case='*pir-vs-bmc*'
./build-wsl/prism-qa pir-vs-bmc testdata --bin ./build-wsl/prism --gate --json "$HOME/pir-vs-bmc-gate.json"
echo "exit=$?"
```

Read the exit code, `hard conflicts`, `superset gaps`, `same bucket where both report`, and the JSON `gate` object. Do not treat a synthetic doctest `PASS` as this run.
