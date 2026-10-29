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

1. **D118** — **Done (checker):** Lean `hobjsz` + `memcpy selfOk` align C++; `pir-lean-check` **139** `agree-ext`, `heap` fixture **15** `agree-ext`, `mismatch=0`; `hobjsz` full sim in `XRefine` proved (XHeap fixed to build on core Lean 4.34, 2026-10-29)
2. **D136 — Done (primitives, 2026-10-29, branch `work/proofs`)** — stdio / FILE model
   primitives proved in Lean with C++ export parity (no sorry, audited, 104 theorems):
   `__prism_havoc_bytes` (`hhavoc`), `__prism_read_range` (`hread`), `__prism_check`
   (`vcheck`), `__prism_memset` (`hmemset`), `__prism_fresh_cstr` (`hfresh`), FILE kind 8
   in `hfree` (`freeChkF`). Fixtures `stdio_file` / `stdio_prims` agree-ext. Next: re-run
   `pir-lean-check` over `testdata` to count how many stdio functions move to agree-ext.

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
