# Next week: PRISM's part (week of 2026-11-23)

OS-Proving's part (the precondition database, the proving runs on the frozen binary) is in the OS-Proving repository.
PRISM's items:

| # | Item | Detail |
|---|---|---|
| a | ILP32 and big-endian targets | Finish `docs/wip/pir-targets.patch` (`pir-targets`, commit 2fe3827, never compiled or gated): check each unit with its real `--target` data layout. Expect conflicts with integration-wave3's `stage.cpp` prepare/check split. Build it, pass `tests/cpp/test_pir_targets.cpp` and the full soundness gate before it replaces the NOTRUN stopgap ([wip/README.md](wip/README.md)). |
| b | Precondition-database lookup | If needed: `--pir-param-preconditions FILE` already reads a TSV (`function parameter condition scope status source`). OS-Proving builds the database; PRISM adds a keyed lookup (by function and build) only if the TSV is too large or too slow to pass whole. Decide when the first database arrives. |
| c | Memory guard counts the Z3 heap twice | The encoder's `mem_guard` (`tree RSS + Z3's estimated heap > budget`) counts Z3's heap in the RSS and again as the estimate, so a function stops at about 60% of `PRISM_MEM_BUDGET_MB` ([PIR.md](PIR.md) "Memory budget"). Fix: count Z3 once, keep one doubling of headroom (one doubling once overshot the budget in `priority_queue_contracts.cpp`), re-run the memory-limit sample. |
| d | Windows CI compile-smoke failures | The four Windows compile smokes (`clang-cl`, `cl`, each with and without Z3; [MSVC_PORT.md](MSVC_PORT.md)) fail. Read the logs and fix, keeping the Linux gate green. |
| e | `CONFIG_RANDSTRUCT` seed | The front end drops the seed, so a seedless layout is analysed. Pass the seed through to the lowering, or report NOTRUN for units that use it. |
| f | Bitwuzla missing on Odin's PC | One wide-int test accepts UNKNOWN because Bitwuzla is absent. Install it (`prism-deps tool bitwuzla`), then tighten the test to require the definitive answer. |
| g | Unmerged branches and PRs | Decide on #49, #62 (Lean DB), #63 and any branch not merged into main: merge, close or park each, and record why. |
| h | Remote branch clean-up | See below. |

## Remote branch clean-up (Odin runs this)

All 36 remote branches merged into main can be deleted. The agent's push-delete was refused by the permission system,
so these commands are for Odin to run in a PRISM clone:

```
git fetch --prune origin "+refs/heads/*:refs/remotes/origin/*"
git branch -r --merged origin/main | grep -v "origin/main\|HEAD" | sed "s# *origin/##" | xargs -n1 git push origin --delete
```

Only `main` is to remain (no branches other than main). The `--merged` filter leaves unmerged branches (item g) alone.
