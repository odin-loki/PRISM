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

**2026-10-29 (per-file run, 300 s / 3 GB cap, `--function-budget 60` default):**
hard conflicts **0** (from 118), superset gaps **14** (from 353 at baseline, 18
at the start of the day), agreement 3450/4602. Gap reasons as the gate prints them:
12 `absent: clang rejects the unit`, 1 `absent: no pir result for the unit`
(`unbounded_string_contracts.cpp` over the cap), 1 `absent` (`sgn`, same unit).

**2026-10-29 rerun (current binary):** hard conflicts **0**, superset gaps **10**,
agreement 3460/4647. Reasons: 8 `absent: clang rejects the unit` (planted lint
snippets with no `#include`), 2 `absent: NOTRUN, GNU C nested function`.
Remaining unbounded proof-rate items in `unbounded_string_contracts.cpp` at a
60 s function budget: `strcat_any_true`, `strncat_any_true`, `strcmp_any_true`,
`strcmp_any_prefix_true`, `strncmp_any_true` BOUNDED. `strcmp_any_prefix_true`
proves PROVED-UNBOUNDED at a 300 s budget (budget-limited). `strcmp_any_true`
is not: Houdini reaches its fixpoint in 3 rounds (42 inductive invariants)
and the final cut-program query (2 inlined `strcmp` loops, 492 properties)
runs out of time even split by property group at 60 s; alone with a 300 s
budget it is PROVED-UNBOUNDED in ~95 s of Houdini (one `strcmp` call alone: 48 s).
With 4 functions checked concurrently at a 150 s budget all five stay BOUNDED
(each spends ~140-150 s: the concurrent searches slow each other). Next:
a solver-time-aware schedule (run the hard rows last, alone, with the
leftover unit time) rather than new invariant templates.
**Done (scheduler):** `PRISM_UNIT_BUDGET=S` (off by default): rows left BOUNDED
by a spent function budget are re-checked alone, in source order, with
max(equal share, min(left, 2 x function budget)) of the unit time left; only a
decisive answer (PROVED*, FAILED) replaces the first; each row records
`rescheduled`. Measured on this file (60 s function budget, 280 s unit
budget, box load 16-44): at `--jobs 4` phase 1 used ~200-270 s and one row was
re-checked (no new answer); at `--jobs 2` (box memory limit) phase 1 alone takes
338 s, nothing is left. The scheduler pays off only when phase 1 finishes well
inside the unit budget; it is not a fix for this file under a 300 s cap.
Standalone (one function per unit, 300 s function budget, `--jobs 2`): `strcat_any_true`
and `strncat_any_true` BOUNDED, "Houdini ran out of time" while still searching
(~181 s, 3 rounds, 27 / 44 inductive); `strncmp_any_true` BOUNDED, fixpoint
reached (47 inductive), final cut-program query out of time. All three stop
at ~181 s of Houdini, below the 300 s function budget: a separate Houdini
search cap is binding: the Houdini deadline is 6 x the per-query `--timeout`
(default 30 s, so 180 s; `houdini.inc` constructor). With `--timeout 60` and a
600 s function budget, `strncmp_any_true` is **PROVED-UNBOUNDED** (316 s of
Houdini): its BOUNDED is a time limit, not a missing template. Next: the
same run for strcat/strncat (still searching at 180 s; may need a
`MemEq`/`MemFrame` template), and decide whether the deadline should follow
the function budget instead of `--timeout`.
**Done (19dea0882):** with a function budget the Houdini deadline is what is
left of it (capped by the run's Houdini budget); 6 x `--timeout` only without
one. `strcat_any_true` alone at a 600 s budget: BOUNDED, Houdini reaches its
fixpoint (5 rounds, 44 inductive) and the final cut-program query is solver
*unknown* at the budget (359 properties). The templates the libc worker named
already exist: `MemEq` at a scope offset (`d[k + x] == s[x]`, x < i) and
`MemFrame` (bytes below the offset keep their entry values), with offsets
from 64-bit scope variables in written addresses (`k = strlen(d)`). Next:
dump the 44 survivors (`pir_invariants` is only written on a proof; add it
to the BOUNDED row too) to see whether those two survive; if they do, the gap
is the final query (Z3 on the quantified instances), not a template.
**Survivors (b64e21f2a, `PRISM_PIR_DUMP_INVARIANTS=1` -> extra
`pir_invariants_surviving` on a non-proof):** `strcat_any_true` at 600 s keeps
43 inductive invariants over its 4 loops, among them the copy
`forall x < i: d[k + x] == s[x]` and the frame `forall x < k + entry(i):
d[x] == d[x] at loop entry` (k = the inlined strlen result). Both needed
templates survive, so the gap is the final cut-program query (solver unknown
when the 600 s function budget is spent), not a template. `houdini_final_budget`
now records how many seconds that query got; next: rerun at 1200 s, then
slice the final query (per property, only invariants of loops that reach it).

**Gate 2026-10-29 18:40 (pf6):** hard conflicts **0**, superset gaps **9**,
agreement 3463/4650: 6 `clang rejects the unit` (lint snippets ill-formed even
with includes: `int*` piped into views, C++26 `std::function_ref`,
`future<int>` from a void `async`), 2 GNU nested function NOTRUN, 1 `clang
timed out on the unit` (`jthread_lint.cpp`, `<thread>` under box load).
`cxx_async_lint.cpp` closed by its `#include <future>`.

**Uncompilable units stay in the superset gate (decision).** They are not
excluded: (a) some are valid code that pir's fixed language level rejects
(dynamic exception specifications are C++11/14, pir lowers C++ as C++23;
GNU nested functions are gcc C that clang does not implement): retiring bmc
would lose real coverage of them; (b) the rest depend on the environment (a
libstdc++ without `<generator>`/`std::views` features), which is Law 1's
case: a missing tool is NOTRUN, never a clean result. Law 7 is met by naming
the reason in the gate output (`absent: clang rejects the unit`), not by
dropping the rows. Next actions: lower a C++ unit at its own `-std=` when
C++23 rejects it (dynamic exception specs), and report GNU nested functions
as NOTRUN with the compiler named.

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

**Handoff 2026-10-29 23:20 (pir-bmc):** db219d691 keeps 30% of the function
budget for Houdini's final query (`PRISM_HOUDINI_FINAL_RESERVE`) and has each
final/optimistic property group assume only the invariants of loops encoded
before it. The pir/repolint/Houdini doctest subset passes (88/88). The full
prism_tests run before it had 8 failed cases (233 assertions), names not
captured: possibly from box OOM or a broken prism-run at the time, not yet
confirmed. A rerun is logging to /workspace/logs/pirbmc-tests-db219.log. Still
to do: the strcat_any_true 1200 s rerun
(/workspace/logs/one6_strcat1200.log; attempt 1 was OOM-killed by the box,
retrying), strncat_any_true at 600 s (its attempt failed because prism-run
was being edited, rc=2; rerun it with /workspace/logs/run_strcat5.sh
strncat_any_true), and the pir-vs-bmc gate (0 hard conflicts required).
