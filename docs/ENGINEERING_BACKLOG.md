# Engineering backlog (PARTIAL roadmap rows)

Actionable next work for **PARTIAL** items in [ROADMAP_STATUS.md](ROADMAP_STATUS.md).
Owner-only rows (history rewrite, GPU hardware, LoRA) are omitted. Supply-chain
**1.1 tracked checkout size** is **retired** (not a goal); see [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md).

## 2.6 Standard library (libc + C++ models)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| `getenv_true` → PROVED | **Done:** `__prism_obj_size` + NUL; `harness.h` `extern "C"` for `assert` in `.cpp` harnesses | `stdlib_contracts.cpp`, `harness.h` |
| `strcat`/`strncat` unbounded BOUNDED | New Houdini templates for four-loop concat | `src/prism/pir/houdini.inc`, `unbounded_string_contracts.cpp` |
| `vector_insert_true` timeout | Value sets for `data()+i`, `i ∈ [0,3]` (map-insert pattern) | `src/prism/pir/memory.cpp`, `vector_insert.cpp` |
| printf not in libc harnesses | Document translator-only scope **or** new models | `libc_format.cpp`, `docs/PIR.md` |
| iostreams / unordered / libc++ | New `models/cxx/` headers + harnesses | `docs/PIR.md` "Not modelled" |

## 2.8 AST lints (`astlint`)

| Gap | Next action | Primary paths |
|-----|-------------|---------------|
| ~287 regex-only classes | Port high-impact rules with `testdata/` plants | `checkers_core.cpp` → `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`STR-UNBOUNDED-COPY`**, **`API-GETS`** doctest in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`MEM-BCOPY`**, **`INT-TAUTOLOGY`** in `kPortedC` | `test_astlint.cpp` |
| Next | Pick from regex-only table (~280 left) | `checkers_core.cpp` → `astlint_checks.cpp` |
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

Grow **`agree-ext`** beyond 146 functions (`docs/PROOFS_REFINEMENT.md`):

1. **D118** — export/check `__prism_obj_size`, `__prism_memcpy` (`realloc` in `stdlib.cpp`)
2. **D136** — `PRISM_FILEK`, `__prism_read_range`, `__prism_havoc_bytes` (stdio models)
3. **`__prism_check` / `__prism_memset` / `__prism_fresh_cstr`** for inlined libc bodies

Verify: `prism-qa pir-lean-check` on `tests/pir` + `testdata`.

## C++ port / Python elimination ([CPP_PORT_PLAN.md](CPP_PORT_PLAN.md))

| Priority | Item |
|----------|------|
| 1 | **Done when**: Dockerfile without `python3` (CI BenchExec-only Python) |
| 2 | Close **`port/ai-gen-tools`** status on `main` |
| 3 | Phase 4 docs/assurance sweep (`prism-qa docs-check`) |
| 4 | Appendix A partials: checkers, rapid, GUI, LTL |
| 5 | `tests/cpp/test_thread.cpp` (appendix C) |

**Permanent:** `tools/svcomp/prism.py`; scanned-project Python linters; `third_party/` build scripts.

## Subagent run (2026-10-25)

Eight Composer 2.5 passes informed this backlog: libc/stdlib contracts, astlint
`STR-UNBOUNDED-COPY`, certified budgets, SV-COMP, pir-vs-bmc retirement gates,
Lean refinement D118/D136, CPP port appendix vs status block, `vector_insert` value sets.
