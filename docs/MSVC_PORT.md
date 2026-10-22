# MSVC / `__int128` portability (native Windows `prism_core`)

**Epic:** build `prism_core` and `prism` with MSVC (`cl`) on Windows without relying on
WSL. **Was:** GCC/Clang `__int128` in several `prism_core` TUs (no MSVC/`clang-cl` ABI type).
Phases 2–4 route those paths through `prism::i128` (`wide_int.hpp`).

**Audit base:** `main` @ `f0c21bd2e` (2026-10-22). Native `__int128` uses in `prism_core`
are migrated to `prism::i128` (phases 2–4). Remaining substring hits are string-only
BMC/astlint tables or `wide_int.hpp` under `PRISM_HAS_INT128`.

**Related:** `docs/ROADMAP_STATUS.md` D7 (WSL primary today); `CMakeLists.txt` rejects
MinGW and already sets MSVC flags on `prism_core`. `tests/cpp/test_main.cpp` solver integration tests are gated on `PRISM_HAS_Z3` only
(wide_int migration removed the old `_WIN32` / `__int128` skip).

---

## 1. Inventory by subsystem

Legend:

| Kind | Meaning |
|------|---------|
| **native** | Code uses `__int128` / `unsigned __int128` as a C++ type (MSVC blocker). |
| **string** | Regex or keyword tables detect `__int128` in user source only (not a blocker). |
| **unix-only** | In `src/tools/qa/`; omitted from `prism_qa` when `WIN32` (not in Windows `prism_core` link). |

### Interval (`native` — `prism_core`)

| File | ~matches | Role |
|------|----------|------|
| `src/prism/interval.cpp` | 24 | Interval arithmetic: `clip`, overflow checks, `+`/`-`/`*`/shifts/div on widened bounds. |

### PIR (`native` — `prism_core`)

| File | ~matches | Role |
|------|----------|------|
| `src/prism/pir/pir.cpp` | 8 | Wide signed/unsigned mul for constant folding / overflow checks. |
| `src/prism/pir/translate_mem.cpp` | 14 | Object size and GEP constant-offset accumulation in `__int128`. |

### SV-COMP (`native` — `prism_core` + public header)

| File | ~matches | Role |
|------|----------|------|
| `include/prism/svcomp_witness.hpp` | 3 | `Num::i` and `of_int` / `int_text` API surface. |
| `src/prism/svcomp/witness.cpp` | 4 | Decimal formatting of 128-bit integers. |
| `src/prism/svcomp/replay.cpp` | 7 | Parse witness / counterexample integers (up to 128-bit). |
| `src/prism/svcomp/debug_ir.cpp` | 8 | JSON big-int parsing for debug IR. |

### Solver (`native` — `prism_core`)

| File | ~matches | Role |
|------|----------|------|
| `src/prism/solver/leanbb.cpp` | — | Migrated to `prism::wide_int.hpp` (phase 4). |

### AI / regress (`native` — `prism_core`)

| File | ~matches | Role |
|------|----------|------|
| `src/prism/ai/regress.cpp` | — | Migrated to `prism::wide_int.hpp` (phase 4). |

### BMC / astlint / interp (`string` only — not an MSVC type blocker)

| File | ~matches | Role |
|------|----------|------|
| `src/prism/bmc.cpp` | 2 | Unencoded-syntax detection in scanned code. |
| `src/prism/bmc_encoder.inc` | 2 | Type name match for 128-bit model gap. |
| `src/prism/bmc_unenc.inc` | 1 | Same. |
| `src/prism/stages/interp.cpp` | 1 | Same. |
| `src/prism/astlint_checks.cpp` | 5 | Width tables / type names (includes `__int128` as string). |

`src/prism/astlint_libclang.cpp` references libclang `CXType_Int128` enums only (not
`__int128` in PRISM sources).

### QA tools (`native`, **unix-only** CMake gate)

| File | ~matches | Role |
|------|----------|------|
| `src/tools/qa/support/yaml_subset.hpp` | — | Migrated: `Node::as_int()` → `optional<prism::i128>` (phase 6 partial). |
| `src/tools/qa/support/yaml_subset.cpp` | — | Uses `parse_i128` / `format_i128` (phase 6 partial). |
| `src/tools/qa/support/task.hpp` / `task.cpp` | — | Migrated: `prism::i128`; built on all platforms (phase 6). |
| `src/tools/qa/support/replay.cpp` | — | Migrated: `prism::u128` / `i128_shl` (phase 6); still **unix-only** (`fork`, `unistd`). |
| `src/tools/qa/support/pyrandom.hpp` | — | Migrated: `prism::i128` / `u128` (phase 6); header-only, used by unix replay/soundness. |

`yaml_subset.cpp` and `task.cpp` are always in `prism_qa` (Windows can link `prism-qa` for
docs-check / triage / etc.). Conformance (`proctree`, `replay`, `conformance.cpp`, …) stays
inside `if(UNIX)` until process-tree and sanitizer replay are ported.

### Tests (outside `src/` / `include/` but coupled)

| File | Notes |
|------|-------|
| `tests/cpp/test_svcomp.cpp` | `static_cast<__int128>(…)` in witness literal check. |
| `tests/cpp/test_main.cpp` | Solver integration tests run when `PRISM_HAS_Z3` (including Windows). |
| `tests/cpp/test_qa_conformance.cpp` | `using i128 = __int128` (Unix QA). |

---

## 2. Portable helper (`include/prism/wide_int.hpp`)

**`prism::i128` / `prism::u128`** with `PRISM_HAS_INT128` fast path (Clang/GCC) and
`{hi, lo}` fallback for MSVC. Shared **`parse_i128`** / **`format_i128`**, bitwise
`u128` ops, and unit tests in `tests/cpp/test_wide_int.cpp`.

**Still on `__int128` (unix-only QA, phase 6):**

- `src/tools/qa/support/pyrandom.hpp`, `task.hpp`, `replay.cpp`

**Duplicated ad-hoc helpers (candidates to fold later):**

- `parse_int` / `as_int` style parsers elsewhere in QA (svcomp/regress use `parse_i128`)

---

## 3. Strategy options

### A. Windows CI / dev with **clang++** (not `clang-cl`)

- Build with LLVM `clang++` targeting Windows (same as many `-stdlib=libc++` setups) where
  `__int128` remains available.
- **Pros:** Near-zero source churn; fastest path to “binary on Windows.”
- **Cons:** Not a native **MSVC** (`cl`) build; ABI/toolchain differs from Visual Studio
  defaults; does not satisfy “MSVC `prism_core`” if that means `cl` specifically.

### B. **Portable 128-bit type** in `include/prism/` (recommended for `cl`)

- Add a small header-only or `.cpp` pair, e.g. `prism::wide_int` / `prism::i128`, with:
  - `int128` / `uint128` storage as `{ uint64_t lo, hi; }` or compiler `__int128` when
    `PRISM_HAS_INT128` (Clang/GCC).
  - Operators needed by interval / PIR / svcomp / leanbb / regress (compare, add, sub,
    mul, shift, sign extend, parse/format decimal).
- **Pros:** Single `prism_core` source tree; real MSVC support; matches supply-chain
  discipline (no new linked third_party if header-only).
- **Cons:** ~800–1 200 LOC touched; must preserve exact parse/format semantics for
  witnesses and regress artefacts; performance regression possible in interval hot paths
  (mitigate with `PRISM_HAS_INT128` fast path).

### C. **`_BitInt(128)`** (C23)

- MSVC has been adding `_BitInt` support; availability and codegen quality vary by toolset
  version.
- **Pros:** Standard spelling; no manual wide arithmetic if the compiler implements it.
- **Cons:** PRISM is C++23; mixed `_BitInt` / template code is awkward; still need a
  fallback typedef for older MSVC and for parity with Clang/GCC builds unless we standardize
  on `_BitInt` everywhere.

### D. **Split / disable** wide paths on Windows

- Stub SV-COMP 128-bit witnesses, interval wide mul, leanbb wide paths as `NOTRUN` or
  narrower checks on `_WIN32`.
- **Pros:** Smallest diff.
- **Cons:** Violates parity goals; poor fit for “one product, two engines” honesty law
  (stages must say what they could not check).

**Recommendation:** **B** for production MSVC (`cl`), with optional **A** as an interim dev
shortcut. Keep **D** only for explicitly documented optional components, not core stages.

---

## 4. Phased plan (coding)

| Phase | Scope | Deliverable |
|-------|--------|-------------|
| **1** (done) | Audit + this doc | File list, strategies, estimates |
| **2** (done) | `include/prism/wide_int.hpp` (+ `.cpp` if needed) | `PRISM_HAS_INT128`, shared `parse_i128` / `format_i128`, unit tests in `tests/cpp/test_wide_int.cpp` |
| **3** (done) | SV-COMP surface | `svcomp_witness.hpp` `Num::i` → portable type; migrate `witness.cpp`, `replay.cpp`, `debug_ir.cpp`; fix `test_svcomp.cpp` |
| **4** (done) | Analysis core | `interval.cpp`, `pir/pir.cpp`, `translate_mem.cpp`, `solver/leanbb.cpp`, `ai/regress.cpp` |
| **5a** (done) | MSVC CI smoke | `windows-compile-smoke`: Ninja + `clang-cl`, `PRISM_Z3=OFF`, `PRISM_TESTS=OFF`, build `prism_core` + `prism`. Shims: `utc_tm`, `set_env`, `NOMINMAX`, `wide_int` / `leanbb`, `solver/util.cpp` `windows.h` + `#undef ERROR`. |
| **5b** (follow-up) | Native MSVC + Z3 | `cl` + `PRISM_Z3=ON`, `prism_tests`, lift `#if !defined(_WIN32)` solver guards in tests. |
| **6** (partial) | QA on Windows | `yaml_subset` + `task` + `pyrandom` on `prism::i128` (done); `prism-qa` in `windows-compile-smoke`; conformance tree stays `if(UNIX)` |

**Dependency order:** 2 → 3 (public API) → 4 (largest volume) → 5 → 6.

---

## 5. LOC estimate (order of magnitude)

| Area | New helper | Migrate / touch |
|------|------------|-----------------|
| `wide_int` module + tests | 250–400 | — |
| SV-COMP + header | 50–100 | 200–350 |
| Interval | — | 120–180 |
| PIR | — | 150–220 |
| Solver `leanbb` | — | 60–100 |
| AI `regress` | — | 150–250 |
| Tests / CMake | — | 40–80 |
| QA (phase 6 only) | reuse helper | 200–300 |
| **Total (phases 2–5)** | **~300–400** | **~800–1 200** |

String-only BMC/astlint files: **0** type migration expected.

---

## 6. Acceptance criteria

- **Phase 5a (CI smoke):** `windows-compile-smoke` configures with `clang-cl` and builds
  `prism_core` and `prism` with `PRISM_Z3=OFF` and `PRISM_TESTS=OFF` (see `.github/workflows/ci.yml`).
- **Phase 5b (follow-up):** same targets with **MSVC `cl`** on Windows x64 and
  `PRISM_Z3=ON` (parity with Linux CI vendored Z3).
- **`prism_tests` on Windows (blocked):** `PRISM_TESTS=ON` does not compile yet: several
  doctest TUs `#include <unistd.h>` unconditionally (e.g. `tests/cpp/test_lint_corpus.cpp`,
  `tests/cpp/test_checkers_cxx.cpp`). Solver integration tests stay behind
  `#if !defined(_WIN32)` in `test_main.cpp` / `test_leanbb.cpp`. Conformance QA remains
  behind `if(UNIX)` in `CMakeLists.txt` (conformance scorer only; `yaml_subset`/`task` are
  in the Windows `prism_qa` subset). Next step: guard or port those includes, then
  enable a Windows subset (`test_wide_int.cpp`, `test_svcomp.cpp`, `test_interval.cpp`).
- Witness and regress integer formatting unchanged on golden strings in `tests/cpp/`.
