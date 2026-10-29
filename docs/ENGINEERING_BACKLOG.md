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
| ~24 regex-only classes (2026-10-29: C++26 library types clang 18 + libstdc++ 14 cannot parse, CXX-ASSUME, and a few compile-time-checked CXX rules) | Port the rest that have a runtime defect on AST nodes, with `testdata/` plants | `checkers_*.cpp` → `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`STR-UNBOUNDED-COPY`**, **`API-GETS`** doctest in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-STRTOK`**, **`API-TMPNAM`**, **`API-MKTEMP`**, **`API-UMASK`** AST ports (+ prior batch) | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **STR-MISSING-NUL** / **MEM-MEMSET-SWAP** / **STR-STRNCPY-NUL** / **MEM-REALLOC-SELF** testdata doctests | `test_astlint.cpp` |
| Done (2026-10-25) | **`API-MKSTEMP`**, **`API-CHMOD-WORLD`**, **`API-SETUID`** AST ports in `kPortedC` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`API-SYSTEM`**, **`API-SIGNAL`**, **`API-GETENV-NULL`**, **`API-STRDUP-NULL`**, **`PTR-UNCHECKED-ALLOC`** AST + testdata doctests | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-25) | **`INT-SHIFT-UB`**, **`PTR-NULL-DEREF`**, **`MEM-STACK-ESCAPE`** testdata doctests; **`return buf`** after `strip_casts` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-29) | Batch 13: **`API-EXEC`** (non-literal exec* path), **`API-PIPE`** (pipe/pipe2 result untested while fds[] is read), **`CXX-LAMBDA-DANGLE`** (returned lambda, direct / via std::function / via a local, capturing a local or by-value parameter by reference), **`CXX-ENABLE-SHARED`** (shared_from_this() in a ctor/dtor or on a local object; enable_shared_from_this as a non-public base) on AST nodes; `testdata/planted/enable_shared_ast.cpp` | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-29) | Batch 14: **`CXX-ATOMIC-FENCE`** (fence as the only sync around a plain global/static store), **`CXX-ENDIAN`** (std::endian, or a local holding it, as an index with no little/big compare), **`CXX-FORWARD-LIKE`** (index-token family), **`CXX-ERROR-CATEGORY`** (local category never compared / asked for name or message). Batches 13–14: zero new rows on `testdata/fp`, `conformance/`, `tp/`; every planted row supersedes its regex row; `src/prism` lint gate with `build/` unchanged at 40 | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-29) | Batch 15: **`API-PRECONDITION`** (comment above a scalar function says `requires n > 0` / `n must be positive`, and the named *parameter* is never tested in a condition, comparison, `!` or `assert`) and **`INTENT`** (comment says `returns x+1` / increment, and `return v;` returns a variable that is never `++`, `+=`, `+`-ed or reassigned). The preamble is read from the raw file lines above the function, as the regex `comment_preamble` does; non-template functions only | `astlint_checks.cpp`, `test_astlint.cpp` |
| Done (2026-10-29) | Self-lint false positives: **`CXX-VECTOR-INDEX`** guards (size() through a cast or a local holding it, find() positions, `!c.empty()`, literal index below a `vector<T> v{a, b}` element count read from source because libclang drops the list), **`CXX-USE-AFTER-MOVE`** (scope/path-aware; re-declaration or assignment resets), **`CTRL-FALLTHROUGH`** on `case Op::Add:`, **`CXX-MOVE-CONST`** declaration order, **`INT-BOOL-AS-BIT`** cast targets, **`CXX-ENABLE-SHARED`** members of an `enable_shared_from_this` class; `testdata/fp/{vector_guards,move_reuse,scoped_case_labels}.cpp` silent on both front ends | `astlint_checks.cpp`, `checkers_core.cpp`, `checkers_cxx.cpp`, `test_astlint.cpp`, `test_checkers_cxx.cpp` |
| Next | Regex-only list: `comm` of the class strings in `checkers_*.cpp` against `astlint*.cpp` + `build/generated/prism/astlint_discard.inc`. Left after batch 15: CXX-TUPLE-GET (std::get<I> is range-checked at compile time), CXX-ASSUME (clang 18 drops `[[assume]]`), and the C++26 types (generator, mdspan, expected, flat_map, inplace_vector, hive, indirect, function_ref, copyable_function, rcu, hazard_pointer, linalg, reflection, contracts, embed, text_encoding, sync_wait). The `core_regex_batch()` rules are already ported | `checkers_*.cpp` → `astlint_checks.cpp` |
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
