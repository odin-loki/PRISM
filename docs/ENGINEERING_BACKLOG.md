# Engineering backlog (PARTIAL roadmap rows)

Actionable next work for **PARTIAL** items in [ROADMAP_STATUS.md](ROADMAP_STATUS.md).
Owner-only rows (history rewrite, GPU hardware, LoRA) are omitted. Supply-chain
**1.1 tracked checkout size** is **retired** (not a goal); see [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md).

## CI

**Lint gate (local):** `./build-werror/prism src/prism --stage lints,warnings --no-llm` → `scripts/lint_failed_count.sh` **0** after astlint self-lint fixes (2026-10-25).

**2026-10-27 (corpus consolidation follow-up):** `main` was red after `4effa335` moved corpora to `testdata/` without updating every doctest path. Fixed: `tests/conformance` → `testdata/conformance` in QA/SV-COMP tools; `manifest_commit()` reads `third_party/MANIFEST.toml` from `PRISM_SOURCE_DIR`; planted inline corpus under `testdata/planted/`; PIR Lean export `L memcpy` line matches `proofs/refinement/fixtures/intrinsics.pirl` (width + memmove flag only). Local `./build/prism_tests`: **1023 passed**, 1 skipped (solver bench).

Latest green **PRISM CI** on `main` is `4eb0b4ab1` (run [`40163391068`](https://github.com/odin-loki/PRISM/actions/runs/40163391068)), newer than the old citation `7b652bf9f`. `4effa335e` and the path-fix commits `3316cf909` / `e59a8b3df` are **not** green. Do not move the green pointer onto those commits until PRISM CI succeeds.

## 2.6 Standard library (libc + C++ models)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| `getenv_true` → PROVED | **Done:** `__prism_obj_size` + NUL; `harness.h` `extern "C"` for `assert` in `.cpp` harnesses | `stdlib_contracts.cpp`, `harness.h` |
| `strcat`/`strncat` unbounded BOUNDED | **Started:** Houdini `MemScopeAt` (`∀x < i: off + x < size(p)`) generated from store scope offsets; still need `MemEq`/`MemFrame` fixpoint + Z3 timing on `strcat_any_true` / `strncat_any_true` | `houdini.inc`, `unbounded_string_contracts.cpp` |
| `vector_insert_true` timeout | **In progress:** `PRISM_INDEX_LE3` + `seed_vs_bin` + `materialize_vs`; direct `pir` ~61 min failed (no output); `prism-qa conformance --stages pir` still **Law 7** (no findings, ~96 s) — raise mem limit / disable solver cache / debug OOM | `memory.cpp`, `encode.cpp`, `vector_insert.cpp` |
| printf not in libc harnesses | **Done (doc):** translator + `mem_libc.c` boundary in `docs/PIR.md` §8.2 | `libc_format.cpp` |
| iostreams / unordered / libc++ | New `models/cxx/` headers + harnesses | `docs/PIR.md` "Not modelled" |

## 2.8 AST lints (`astlint`)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| ~287 regex-only classes | Port high-impact rules with `testdata/` plants | `checkers_core.cpp` → `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`STR-UNBOUNDED-COPY`**, **`API-GETS`** doctest in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-STRTOK`**, **`API-TMPNAM`**, **`API-MKTEMP`**, **`API-UMASK`** AST ports (+ prior batch) | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **STR-MISSING-NUL** / **MEM-MEMSET-SWAP** / **STR-STRNCPY-NUL** / **MEM-REALLOC-SELF** testdata doctests | `test_astlint.cpp` |
| Done (2026-10-25) | **`API-MKSTEMP`**, **`API-CHMOD-WORLD`**, **`API-SETUID`** AST ports in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-SYSTEM`**, **`API-SIGNAL`**, **`API-GETENV-NULL`**, **`API-STRDUP-NULL`**, **`PTR-UNCHECKED-ALLOC`** AST + testdata doctests | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`INT-SHIFT-UB`**, **`PTR-NULL-DEREF`**, **`MEM-STACK-ESCAPE`** testdata doctests; **`return buf`** after `strip_casts` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Next | Pick from regex-only table (~265 left) | `checkers_core.cpp` → `astlint_checks.cpp` |
| Template / flow limits | Document NOTRUN; defer dependent-template rules | `astlint.hpp` |

## 2.8 / D2 — Retire hand `bmc` encoder

Retirement **blocked** until PIR is a **superset** of bmc reach on `testdata/`.

| Gate | Baseline (`docs/PIR.md`) |
|------|---------------------------|
| Hard conflicts | 0 (investigate new `PROVED*` vs `FAILED`) |
| Superset | `matrix[PROVED*, absent] + [PROVED*, NEEDS-HARNESS] → 0` (was **353**) |
| Agreement | Raise `same bucket where both report` (was **2398/2462**) |

Tool: `prism-qa pir-vs-bmc`. Doctest: synthetic only in `test_qa.cpp` — **no CI gate** yet.

## 3.1 Portfolio vs Z3 alone

Rerun 4,179-VC benchmark with Bitwuzla fix + learned scheduler (`docs/SOLVERS.md`).

## 3.2 Certified mode (loop-free suite)

| Gap | Next action |
|-----|-------------|
| `fn_macro_true` / `long_mul_true` over budget | Raise `PRISM_CERTIFY_BUDGET`, `PRISM_CHECKER_MEM`; use cert cache |
| Full-suite rerun | `prism-qa conformance --certified` after libc/C++ growth |

Env: `PRISM_CERTIFY_BUDGET`, `PRISM_CERT_CACHE_MAX`, `PRISM_CHECKER_MEM` (`src/prism/pir/stage.cpp`, `encode.cpp`).

## 6.3 SV-COMP

| Priority | Task |
|----------|------|
| 1 | **ILP32** (`--data-model ILP32`) — largest lever on 203/417 `unknown`s |
| 2 | Proof yield on remaining `unknown`s (assert forms, invariants, timeouts) |
| 3 | Release tarball + `fm-tools.yml` + BenchExec upstream `prism.py` |
| 4 | CI: `prism svcomp score` + witness validation (0 incorrect) |
| 5 | Property scope (memsafety vs overflow+reach only) |

C++ surface: `prism svcomp`, `src/prism/svcomp/`. **Keep** `tools/svcomp/prism.py` (BenchExec contract).

## 8.2 LLVM → PIR refinement (Lean checker)

Grow **`agree-ext`** beyond 139 functions (`docs/PROOFS_REFINEMENT.md`; `pir-lean-check` on `testdata/pir` + `testdata`):

1. **D118** — **Done (checker):** Lean `hobjsz` + `memcpy selfOk` align C++; `pir-lean-check` **139** `agree-ext`, `heap` fixture **15** `agree-ext`, `mismatch=0`; `hobjsz` full sim in `XRefine` still `admit`
2. **D136 (partial)** — stdio / FILE models: C++ Lean export emits `L hhavoc` for
   `__prism_havoc_bytes` (`export_lean.cpp`; checker still `outside` until Lean
   `hhavoc` + `MemTr::write_havoc`). Remaining: `PRISM_FILEK` in `hfree`, `L
   hread`/`hhavoc` parity for `__prism_read_range`, `__prism_check` in stdio bodies.
3. **`__prism_check` / `__prism_memset` / `__prism_fresh_cstr`** for inlined libc bodies

Verify: `prism-qa pir-lean-check` on `testdata/pir` + `testdata`.

## C++ port / Python elimination ([CPP_PORT_PLAN.md](CPP_PORT_PLAN.md))

| Priority | Item |
|----------|------|
| 1 | **Done when**: Dockerfile without `python3` (CI BenchExec-only Python) |
| 2 | **Done:** `port/ai-gen-tools` closed in `CPP_PORT_PLAN.md` |
| 3 | **Done (2026-10-25):** `prism-qa docs-check` + assurance-check; active docs/`README` prose points at C++ paths and doctests (not deleted `tests/test_*.py`) |
| 4 | Appendix A **code** partials remain: checkers, rapid, GUI, LTL (`docs/CPP_PORT_PLAN.md` appendix A) |
| 5 | `tests/cpp/test_thread.cpp` (appendix C) |

**Permanent:** `tools/svcomp/prism.py`; scanned-project Python linters; `third_party/` build scripts.

## Subagent run (2026-10-25)

Eight Composer 2.5 passes informed this backlog: libc/stdlib contracts, astlint
`STR-UNBOUNDED-COPY`, certified budgets, SV-COMP, pir-vs-bmc retirement gates,
Lean refinement D118/D136, CPP port appendix vs status block, `vector_insert` value sets.

## SV-COMP handoff (work/svcomp, 2026-10-29)

- Done: bmc encodes a forward goto into a later block (CIL `if (0)` switch encoding). It is refused when the jump enters a loop pass or switch arm, or skips a declaration. `(void *)0` is modelled as the integer 0. Tests are in tests/cpp/test_bmc_svcomp.cpp.
- Scores, all 0 incorrect: no-overflow 118/192, unchanged with the goto build. unreach-call was 127/225 on the old build. The rescore with goto plus `(void *)0` (log /workspace/logs/svcomp-score-unreach-5.log, output /tmp/svg-unreach5) was still running at handoff, so record its result.
- Remaining s3_* gaps: s3_clnt_2 has an unbounded state-machine `while(1)` that is only proved to 8 unrollings, so main's call to it is not modelled. s3_clnt_2-2a and s3_srvr_* report UNINIT-READ. Check whether that read is real or a model gap before treating it as a bug.
- s3 UNINIT-READ checked (2026-10-30): neither is a model gap in the reads themselves. s3_clnt_2-2a: `s__state = ag_X ^ ag_Y` (ag_Y nondet) can jump to state 4352, which reads `s__s3__tmp__next_state___0` before any write, so the read is real in C (indeterminate value; not part of unreach-call). s3_srvr_1a: the report analyses `ssl3_accept(initial_state)` with an arbitrary argument (cex initial_state=16384, reads `s__init_buf___0`); from main (8464) no assignment ever sets 16384/8192/24576, so it is unreachable in context. Neither finding is on main, so neither blocks the verdict. What does: the unbounded state-machine loop (main BOUNDED, or NEEDS-HARNESS because the looping callee is not inlined). Next lever there is the loop, not the uninit check.
- Houdini (2026-10-30, 398d61218): cut loops may now contain `__VERIFIER_nondet_*` calls (fresh value per evaluation; writes nothing), and templates include congruences `x % c == r` (c = 2,3,4). jain_1-1/2-1/4-2/5-2/6-1 now prove (were BOUNDED); jain_7-2 still unknown. Loop-dir scan log: /workspace/logs/svcomp-loopscan.log.
- Next: loop invariants for the BOUNDED-only tasks (loop-acceleration array_*, loop-invgen up/seq-3/half_2, loop-new count_by_*/gauss_sum). Candidate approach: feed Houdini candidates (linear equalities and bounds over loop counters) to bmc's havoc step as assumptions, proved inductive first.
- Box: run every score through /workspace/bin/prism-run with --jobs 2. Long scores can wait a long time for a slot and can be killed by the OOM killer. Build in build-goto/ so the binary a running score uses is not replaced.
- Pending (2026-10-30 01:00): (a) unreach-call rescore /tmp/svg-unreach5 (build-goto, pre-398d61218) was at 127/225 tasks; record in docs/SVCOMP.md only if 0 incorrect. (b) Then rescore unreach-call AND no-overflow with build/ (398d61218+) through prism-run --jobs 2; the Houdini change should add the jain_* tasks. (c) Loop-dir scan (rsscap, one task at a time) is writing /workspace/logs/svcomp-loopscan.log: so far no wrong answers; count_by_k, gauss_sum*, half still unknown (need non-template invariants, e.g. sum == i*(i+1)/2 -> nonlinear; next idea: templates `2*s == i*i + i` guarded by bounds so no overflow).
- 2026-10-30 02:00: 39fb3909d adds non-linear/three-variable Houdini templates; gauss_sum and count_by_k now prove; half/half_2 still BOUNDED (need the loop-1 post-state carried into loop 2). Full prism_tests on 398d61218: 1346/1353, the 7 failures are all astlint 'stddef.h not found' (libclang resource dir; fixed on work/astlint 620d26eb6, not in this branch). Bmc/ai/Houdini subset 205/205 on 39fb3909d. build-inv/ holds the 39fb3909d binary. Loop scan rerun on build-inv: /workspace/logs/svcomp-loopscan2.log (slow, ~4 min/task: check the bigger template set's cost; the 240->480 cap may need a smarter order). Rescore both properties with build-inv after /tmp/svg-unreach5 finishes (it was at 129/225). Note: `prism svcomp` run from the worktree writes witness.yml into cwd; `git checkout witness.yml` before committing.
- 2026-10-30 02:30: unreach-call 130/225, 0 incorrect, recorded. 32331646c: cheap Houdini set first (count_by_2 1 s, gauss_sum 9 s), extended only when open; half proves (half_2 not rechecked). NOT done: the bmc/ai test subset on 32331646c and both rescores with build-inv/ -- the root filesystem ran out of inodes (df -i 100%; biggest: /workspace/logs/pvb 240k files, /tmp/cc2 228k, /workspace/rd-*-build, wt-lifter). build-goto/ was deleted (it was only for the finished unreach5 run). Next: once inodes are free, run `build-inv/prism_tests -tc="*bmc*,*houdini*,*Houdini*,*invariant*,ai*"`, then `prism-run build-inv/prism svcomp score --property {unreach-call,no-overflow} --jobs 2 --out /tmp/svg-{unreach,noovf}6`, then the loop scan (/tmp/loopscan.sh).
- 2026-10-30 04:20: bmc/ai/Houdini subset 207/207 on build-inv (32331646c). half_2 now `true`. Rescores on build-inv running (started 01:55): logs /workspace/logs/svcomp-score-{unreach,noovf}-6.log, out /tmp/svg-unreach6 (99/225 task dirs at 04:20), /tmp/svg-noovf6 (102/192). When score.md appears: record in docs/SVCOMP.md only if incorrect 0, then `find /tmp/svg-X6 -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +` (keep results.json, score.md). The separate loop scan is skipped: the unreach-call rescore covers every loop-* task.

- svcomp handoff (38365f215): no-overflow rescore 6 recorded (124/192, 0 incorrect; scratch deleted). unreach-call rescore 6 still running on build-inv (/tmp/svg-unreach6, log /workspace/logs/svcomp-score-unreach-6.log): when done, record only if 0 incorrect, then delete per-task dirs. Goto-loop rewrite (goto_loops_to_while) is committed but built only in build/ (do not relink build-inv mid-rescore); it proves the gauss_sum reducers; it is not in either rescore. Next: rescore unreach with it; remaining BOUNDED categories: step open (14), call in loop (7), nested loop (2).

- svcomp handoff (688c9ea1f): unreach6 recorded (143/225, 0 incorrect). Final unreach rescore 7 queued via prism-run on the frozen binary /workspace/prism-frozen/4252890f4/prism (goto-loop rewrite + phase-guard templates), out /tmp/svg-unreach7, log /workspace/logs/svcomp-score-unreach-7.log. Record only with 0 incorrect, then delete its per-task dirs. Watch: nested_1-2 lost its fuzz-found false in rescore 6 (probably the fuzz time budget was used up after a slower bmc/Houdini; not unsound), and s3_srvr_1a and seq-3 hit TIMEOUT. e2e check: tests/e2e/svcomp_goto_loops.sh PRISM. Still open: array_* (needs quantified invariants), sum02-2 (needs a widened 64-bit 2*sn == i*(i-1)), deep-nested (false only after about 2^32 iterations; out of reach).

- svcomp handoff (121d3d88d), regressions:
  - nested_1-2: fixed in 2182331e2. Fuzzing now runs before bmc deepening, and when every 2 s run hits the limit it gets one long run (up to 30 s). Single-task result: false(unreach-call) in 10 s.
  - seq-3 and s3_srvr_1a time out because of deepening: seq-3 base run 12 min, s3 u128 run over 13 min. Since 121d3d88d, deepening is skipped when about 4x the last run would pass 840 s. Single-task validation logs: /tmp/gv/v_*.log.
  - sum02-2 is blocked by Z3. The guarded, widened candidate `n + 1 == 0 || 2*sn == (i+0ULL)*(i+0ULL-1)` comes back unknown in 60 s, because the solver has to reason that i+1 does not wrap. It is unsat in 1.3 s with simplify(som=true), but only when the fact zext(i+1) == zext(i)+1 is given. The candidates were reverted so they don't slow other tasks; integer-suffix support in the expression validator is kept (db44c5d27). Next idea: add the no-wrap fact when the loop guard implies i < MAX, plus a som-simplify retry.
  - unreach7 (frozen 4252890f4) is still running: /tmp/svg-unreach7. It does not include the fuzz, deepening or suffix commits.
  - Validated 121d3d88d on single tasks: s3_srvr_1a unknown in 215 s (u128 skipped), seq-3 unknown in 634 s (u32 skipped); was TIMEOUT. sum02-2: even with the case i != MAX, Z3 cannot relate zext(i+1) to zext(i)+1 (unknown in 20 s), so it needs an encoder-level rewrite, not more candidates.
- svcomp handoff (a00e0b571): unreach7 recorded, 151/225 with 0 incorrect (new trues: phases_1-1 and three gauss_sum reducers); scratch deleted. Next: freeze a binary at HEAD (fuzz-first with the long run, deepening budget, suffixes) and run unreach8 and noovf7 via prism-run. Expect nested_1-2 to be false again and s3_srvr_1a/seq-3 to be unknown instead of TIMEOUT. sum02-2 needs an encoder-level no-wrap fact for zext(i+1).
- svcomp handoff (04b8dba64): the no-wrap step fact is in. Houdini now asserts the fact before assuming it (tag kStepFactTag+j) and drops it if it fails; there is a negative test with a narrow counter. With it, sum02-2's guarded invariant `n + 1 == 0 || sn == (i+0ULL)*(i+0ULL-1)/2` is inductive. main is still BOUNDED, because Z3 returns unknown on the exit check (sn == nl*(nl+1)/2 from zext(i) == nl+1), even with the som/solve-eqs retry and even when given w(i)==nl+1 (/tmp/gv/z7.py). Next idea: rewrite zext(x+1) to zext(x)+1 under x != MAX, or pre-substitute the loop exit equality. Rescores are running on the frozen e4deccc86, which does not include 04b8dba64: /tmp/svg-unreach8 (log svcomp-score-unreach-8.log) and /tmp/svg-noovf7 (log svcomp-score-noovf-7.log). Record each only if it has 0 incorrect, then delete its per-task dirs. Watch whether the som retry slows things down (it adds up to 4 s per unknown query).

### Handoff 2026-10-30 (a5e363a5c)
- sum02-2 PARKED: step fact + som retry make the guarded Gauss invariant inductive only with long retries; the exit check stays unknown on 64-bit nonlinear Z3. Unknown-retry is now bounded (4 per check_program, 1.5 s each); test keeps soundness asserts only.
- Running on frozen e4deccc86: unreach8 (/tmp/svg-unreach8), noovf7 (/tmp/svg-noovf7). Record only if 0 incorrect, then delete per-task dirs.
- Next: categorize 34 no-overflow unknowns by root cause and fix the biggest sound category.
- noovf unknowns (noovf6, 34) by root cause: ~11 bmc BOUNDED/pir none, 4 FAILED, 3 BOUNDED/BOUNDED, 6 NEEDS-HARNESS, 4 UNKNOWN. Biggest sound category: proof of main blocked by the (irrelevant) reach_error/abort FUNC-CONTRACT checks. Fixed by neutralize_error_calls (no-overflow only; replay/fuzz/witness use original). Single tasks now true: array_1-1, array_2-2-simple, array_3-1, array_3-2, nested_1b, NetBSD_loop, jain_7-2 (array_2-1-simple too). Still unknown: array_2-1, array_2-2. All 32 false-expected tasks with error calls still false. Needs a noovf8 rescore on a frozen binary at this commit.
- Correction: 9aec11760 says "+7 trues"; it is +8 (array_2-1-simple also turns true). Commit message left as is (already pushed).
