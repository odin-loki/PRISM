# Engineering backlog (PARTIAL roadmap rows)

Actionable next work for **PARTIAL** items in [ROADMAP_STATUS.md](ROADMAP_STATUS.md).
Owner-only rows (history rewrite, GPU hardware, LoRA) are omitted. Supply-chain
**1.1 tracked checkout size** is **retired** (not a goal); see [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md).

## CI

**Lint gate (local):** `./build-werror/prism src/prism --stage lints,warnings --no-llm` → `scripts/lint_failed_count.sh` **0** after astlint self-lint fixes (2026-10-25).

**2026-10-27 (corpus consolidation follow-up):** `main` was red after `4effa335` moved corpora to `testdata/` without updating every doctest path. Fixed: `tests/conformance` → `testdata/conformance` in QA/SV-COMP tools; `manifest_commit()` reads `third_party/MANIFEST.toml` from `PRISM_SOURCE_DIR`; planted inline corpus under `testdata/planted/`; PIR Lean export `L memcpy` line matches `proofs/refinement/fixtures/intrinsics.pirl` (width + memmove flag only). Local `./build/prism_tests`: **1023 passed**, 1 skipped (solver bench).

**2026-10-30 (`prism-finish`):** the PR #8 head (`74e48349c`) was red on three counts, all fixed: (1) `astlint` fixtures did not parse against real glibc headers, and the process front end hid clang's error (diagnostic outside the last 1 MB of merged output), so every planted mark read "missed"; several ported checks (INT-ATOI, MEM-ALLOCA, MEM-VLA-SIZE line, FUNC-NORETURN GNU attribute, LOCK-DOUBLE-UNLOCK across exiting branches) were fixed once the fixtures ran; (2) MSVC `C2375` on the models' `abort` declaration; (3) clang-cl/lld-link duplicate `__prism_check` in `prism_tests`. Local Linux (clang 18, libstdc++ 14, process front end): `prism_tests` **1348/1348** passed, 1 skipped. Open: with a relocated (conda) libclang the libclang front end still cannot find libstdc++ headers, and differs from the process front end on `_Noreturn`, `STR-WCSCPY` and some dead-store rows.

Latest green **PRISM CI** on `main` is `4eb0b4ab1` (run [`40163391068`](https://github.com/odin-loki/PRISM/actions/runs/40163391068)), newer than the old citation `7b652bf9f`. `4effa335e` and the path-fix commits `3316cf909` / `e59a8b3df` are **not** green. Do not move the green pointer onto those commits until PRISM CI succeeds.

## 2.6 Standard library (libc + C++ models)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| `getenv_true` → PROVED | **Done:** `__prism_obj_size` + NUL; `harness.h` `extern "C"` for `assert` in `.cpp` harnesses | `stdlib_contracts.cpp`, `harness.h` |
| `strcat`/`strncat` unbounded BOUNDED | **Started:** Houdini `MemScopeAt` (`∀x < i: off + x < size(p)`) generated from store scope offsets; still BOUNDED: Houdini runs out of time (348 candidates, 4 loops; 3 rounds in 1200 s with `--timeout 200`) — needs cheaper step queries / fewer candidates | `houdini.inc`, `unbounded_string_contracts.cpp` |
| `vector_insert_true` timeout | **Done:** `peel_ite` (dead same-condition ites, single-arg `or`/`and`), merged-set operand retry, `kVsMax` 64 → PROVED at unwind 8 in ~24 s / 1.5 GB; UAF twin FAILED MEM-UAF. Encoder OOM is now Law 7 UNKNOWN (`PRISM_PIR_MEM_MB`, default min(60% RAM, RLIMIT_AS/4)) | `memory.cpp`, `encode.cpp`, `test_pir_mem.cpp` Also (2026-10-30): the vector model's reallocating insert moves trivially copyable elements in one `size()`-bounded loop (71 s on its own). |
| printf not in libc harnesses | **Done (doc):** translator + `mem_libc.c` boundary in `docs/PIR.md` §8.2 | `libc_format.cpp` |
| iostreams / unordered / libc++ | New `models/cxx/` headers + harnesses | `docs/PIR.md` "Not modelled" |
| Z3 hard OOM poisons the process / box OOM | **Done (2026-10-29, `fix/memory-budget`):** one process-wide memory budget (`prism/membudget.hpp`): 75% of min(cgroup limit, MemAvailable, RLIMIT_AS/2), override `PRISM_MEM_BUDGET_MB` (`PRISM_PIR_MEM_MB` still read); trips at 80% of it or when MemAvailable < `PRISM_MEM_RESERVE_MB` (800). Checked in the encoder (SymMem read-over-write / value sets / havoc reads, every encoded node) and by a 100 ms watchdog that `Z3_interrupt`s every live context (pir and portfolio) mid-query: per-function UNKNOWN with the reason, heap `malloc_trim`med between functions, later functions checked (not sticky). Past half the budget the portfolio skips the cache-key copy (`normalized_query`'s `Z3_translate`, measured as the jump that killed `priority_queue_contracts`) and runs one in-process solver. `M_ARENA_MAX=4` set in `main`: glibc's per-thread 64 MB arenas were the address space a 4 GB RLIMIT_AS ran out of at ~28 MB RSS (th_rewriter null deref on `unordered_map`/`sstream`); no crash now. `prism-qa conformance` gives each run its share of MemAvailable and cuts `--jobs` to fit. Box wrapper `/workspace/bin/prism-run`. Left: the hard-limit fresh-process continuation stays as the last resort; `umap_erase_uaf_false` is slow (>12 min, memory flat) — a time problem, not memory | `membudget.cpp`, `encode.cpp`, `memory.cpp`, `portfolio.cpp`, `tests/cpp/test_membudget.cpp` |
| 24 C++ harnesses had no `.yml` | **Done:** labelled (streams, algorithm, chrono, filesystem, ...); `tuple` model fixed for `tuple<T&...>` (map/set models compile again, 11/11) | `testdata/conformance/libc-models`, `models/cxx/tuple` |

## 2.8 AST lints (`astlint`)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| Regex-only classes (was ~287; 4 left, see Next) | Port high-impact rules with `testdata/` plants | `checkers_core.cpp` → `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`STR-UNBOUNDED-COPY`**, **`API-GETS`** doctest in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-STRTOK`**, **`API-TMPNAM`**, **`API-MKTEMP`**, **`API-UMASK`** AST ports (+ prior batch) | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **STR-MISSING-NUL** / **MEM-MEMSET-SWAP** / **STR-STRNCPY-NUL** / **MEM-REALLOC-SELF** testdata doctests | `test_astlint.cpp` |
| Done (2026-10-25) | **`API-MKSTEMP`**, **`API-CHMOD-WORLD`**, **`API-SETUID`** AST ports in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-SYSTEM`**, **`API-SIGNAL`**, **`API-GETENV-NULL`**, **`API-STRDUP-NULL`**, **`PTR-UNCHECKED-ALLOC`** AST + testdata doctests | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`INT-SHIFT-UB`**, **`PTR-NULL-DEREF`**, **`MEM-STACK-ESCAPE`** testdata doctests; **`return buf`** after `strip_casts` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-30) | **`LOCK-ORDER`** AST port (`lock_order.c` plants); libclang front end: driver-derived system includes (`clang -###`), C11 `_Noreturn`, sugared `wchar_t` — lint suite agrees across both front ends | `astlint.cpp`, `astlint_libclang.cpp`, `astlint_checks.cpp` |
| Done (2026-10-30) | **`CTRL-SIBLING-ASYMMETRY`**, **`MEM-CAPACITY-FIRST`** AST ports (`sibling.c`, `capacity.c` plants). Remaining regex-only: **`API-PRECONDITION`** and **`INTENT`** — both compare a comment preamble with the body, so they stay text rules by design | `astlint_checks.cpp`, `test_astlint.cpp` |
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

Grow **`agree-ext`** beyond 491 functions (unchanged after inlined allocas, 8920b58bc; outside 1 865) (`docs/PROOFS_REFINEMENT.md` "All six trees"; `pir-lean-check` over the six `testdata` trees, base 221, mismatch 0):

1. **D118** — **Done (checker):** Lean `hobjsz` + `memcpy selfOk` align C++; `pir-lean-check` **139** `agree-ext`, `heap` fixture **15** `agree-ext`, `mismatch=0`; `hobjsz` full sim in `XRefine` proved (XHeap fixed to build on core Lean 4.34, 2026-10-29)
2. **D136 — Done (primitives, 2026-10-29, branch `work/proofs`)** — stdio / FILE model
   primitives proved in Lean with C++ export parity (no sorry, audited, 104 theorems):
   `__prism_havoc_bytes` (`hhavoc`), `__prism_read_range` (`hread`), `__prism_check`
   (`vcheck`), `__prism_memset` (`hmemset`), `__prism_fresh_cstr` (`hfresh`), FILE kind 8
   in `hfree` (`freeChkF`). Fixtures `stdio_file` / `stdio_prims` agree-ext. Measured: 221 → 287 agree-ext.
3. **Noreturn checks + unreachable blocks — Done (2026-10-29, `work/proofs`)** — `L afail`
   (`assert`, `reach_error`, traps, libstdc++ assertions; `XBlock.nr`) and dead LLVM blocks
   (`xLive` closure checked in `validB`, liveness in `InvX`/`TailX`): 287 → **419** agree-ext,
   mismatch 0.
4. **`invoke` / landing pads without throws — Done (2026-10-29, 65d994197)** — invoke as call +
   `br`, unreachable blocks stubbed (`L dead`/`L stub`, `XBlock.stub`), dead phi incomings dropped:
   419 → **479** agree-ext, mismatch 0. Next by count: real throw/catch (`__cxa_begin_catch` 63,
   `__cxa_allocate_exception` 33, `__throw_*` 29), indirect calls 30, ptrtoint 27, float/double 42,
   pthread/mtx 35, inlined alloca 18, constant-GEP 14.
5. **`ptrtoint` pointer difference + `std::__throw_*` without handlers — Done (2026-10-29,
   d02bdb7f5, 64b4cdd34)** — `L psub` (`PTR-COMPARE` on different objects) and `L ptrtoint`;
   `L throw <typeinfo>` (semantics stuck at the throw, translator mirrors `Tr::raise`: exception
   object, header stores, `type_id`, `CXX-UNCAUGHT` in `main`) when no invoke can be on the stack:
   479 → **491** agree-ext (1 039 / 491 / 267 / 1 869), mismatch 0. Not mirrorable: pthread_mutex_lock /
   mtx_lock / pthread_create are UNENCODED in `translate.cpp` itself; all 63 `__cxa_begin_catch`
   functions are not-ok in C++. Next: `__throw_*` under a handler (landing-pad dispatch; the 29 in
   testdata all sit under an `invoke` of the `operator new` model), throws with arguments,
   inlined alloca 18 (frame alloca tracking + `end_lifetime` at return), constant-GEP 14 (5 ok in C++).

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

## Documentation rewrite (owner request, 2026-10-30)

When the current backlog work is finished: rewrite all documentation from scratch (it has gone stale) and double-check every claim against the code and fresh measurements.

## prism_tests failures not caused by work/libc-models (2026-10-29)

Full run under `/workspace/bin/prism-run` (prlimit --as=4 GB): 15 failed cases.
Four were caused by this branch and are fixed: the libc contract-harness test (true-only ymls now carry a `no-false-twin:` line), the per-function harness test (`_Hash_bytes` moved to the `<typeindex>` model), the deps SBOM drift string (z3 `memory_manager.cpp`), and the regex model source test (`_M_fail` inlined).
- **Environmental (ASan cannot reserve its shadow memory under an address-space cap):** cxx format model setup, certified (combined proof split), label self-check, fuse PRISM_AFL, ai9 lean best-first, ai-assist regress (sanitizer half). All of these pass when run without prlimit.
- **Pre-existing:** the 6 astlint cases (libclang resource dir), and ai-assist regress `test_ai_assist.cpp:433`. That check runs a cmake+ctest build of the generated tests and fails without prlimit too. No ai/ files changed on this branch.
