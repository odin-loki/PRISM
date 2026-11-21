# Next week: PRISM's part (week of 2026-11-23)

OS-Proving's part (the r2 run on the frozen w9 binary, the label merge, the precondition database, more coverage, human review)
is in the OS-Proving repository, `docs/NEXT_WEEK.md`. Its **step 0 is this repository's branches and PRs, and it comes first**
(Odin, 21 Nov: "Lets get them done first next week").

## 0. Branches and open PRs (first)

Only `main` is meant to remain (Odin, 20 Nov: "No other branches other than main"). On 21 Nov the 35 branches fully merged
into main were deleted. 21 remain, plus open PRs #49, #62 and #63. For each: finish and merge (gate on the OS-Proving test
pool, `bash tools/cloud/pbx.sh gate ...`, 0 wrong proofs), or port what is unique and close it; then delete the branch.
Odin approves each deletion of a branch that holds work not in main.

| Branch / PR | State (21 Nov) | Action |
|---|---|---|
| `pir-header-stubs` | 0 commits ahead, fully in main (#80) | delete (Odin's OK) |
| `pir-targets` | 1 ahead (2fe3827), unbuilt WIP; identical copy in `docs/wip/pir-targets.patch` | item a below; then remove `docs/wip/` and delete |
| #49 `pir-kernel-models` | 1 ahead, 16 Nov: atomics modelled only in kernel mode | rebase on main, check against #80's kernel models, gate, merge or close |
| #63 `pir-memory-scaling` | 6 ahead, 19 Nov: memory-scaling measurements (docs 10.10) | compare with #76 (havoc reads as UF, in main); keep what still holds, close |
| #62 `sim-symbolic-ann` | 5 ahead, 19 Nov: `prism sim` and the Lean database | rebuild and test on main; Goedel-Pset-v1 stays local only (library-only set 437 MiB); merge |
| `gpu-proving-ai-providers` | 1 ahead, 15 Nov: GPU input sweep, AI providers | review; merge or close |
| `ci-integration-check` | 3 ahead, 16 Nov: toolchain pins | review; merge or close |
| `port/bench`, `port/cipin`, `port/docs`, `port/goedel`, `port/harden`, `port/precision`, `port/sanitize`, `port/testharness`, `port/winmsvc`, `prove-fast-grammar`, `prove-fresh-seed-and-primed-names`, `prove-link-lake-packages`, `env-registry-fix`, `path-relative-to-scan-root` | about 1,685 to 1,692 "ahead", 5 to 7 Nov: cut before main's history was rewritten | compare by content (`git cherry -v origin/main origin/<b>`, then search main for each change); port anything missing; delete |

Done when `git ls-remote --heads origin` lists only `main`.

## Other items

| # | Item | Detail |
|---|---|---|
| a | ILP32 and big-endian targets | Finish `docs/wip/pir-targets.patch` (`pir-targets`, commit 2fe3827, never compiled or gated): check each unit with its real `--target` data layout (pointer and long widths, byte order, per-target `va_list`, target-width models). Expect conflicts with integration-wave3's `stage.cpp` prepare/check split. Build it, pass `tests/cpp/test_pir_targets.cpp` and the full soundness gate before it replaces the NOTRUN stopgap ([wip/README.md](wip/README.md)). If it lands before OS-Proving's r2 run, refreeze so r2 covers the roughly 15k 32-bit and big-endian units. |
| b | Precondition-database lookup | If needed: `--pir-param-preconditions FILE` already reads a TSV (`function parameter condition scope status source`). OS-Proving builds the database; PRISM adds a keyed lookup (by function and build) only if the TSV is too large or too slow to pass whole. Decide when the first database arrives. |
| c | Memory guard counts the Z3 heap twice | The encoder's `mem_guard` (`tree RSS + Z3's estimated heap > budget`) counts Z3's heap in the RSS and again as the estimate, so a function stops at about 60% of `PRISM_MEM_BUDGET_MB` ([PIR.md](PIR.md) "Memory budget"). Fix: count Z3 once, keep one doubling of headroom (one doubling once overshot the budget in `priority_queue_contracts.cpp`), re-run the memory-limit sample. |
| d | Windows CI compile-smoke failures | The Windows compile smokes (`clang-cl`, `cl`, each with and without Z3; [MSVC_PORT.md](MSVC_PORT.md)) fail on every run. Read the logs and fix, keeping the Linux gate green. |
| e | `CONFIG_RANDSTRUCT` seed | The front end drops the seed, so a seedless layout is analysed. Pass the seed through to the lowering, or report NOTRUN for units that use it. |
| f | Bitwuzla missing on Odin's PC | One wide-int test accepts UNKNOWN because Bitwuzla is absent. Install it (`prism-deps tool bitwuzla`), then tighten the test to require the definitive answer. |
| g | Main CI after the docs commit | Check CI on 1496e61f1 (docs on top of 043fff4) is green apart from the Windows smokes. |
