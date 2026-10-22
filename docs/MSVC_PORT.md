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
| **5a** (done) | MSVC CI smoke | `windows-compile-smoke`: Ninja + `clang-cl`, `PRISM_Z3=OFF`, build `prism_core` + `prism` + `prism-qa` + portable `prism_tests` subset (phase 7). Shims: `utc_tm`, `set_env`, `NOMINMAX`, `wide_int` / `leanbb`, `solver/util.cpp` `windows.h` + `#undef ERROR`. |
| **5b** (CI done) | Windows CI + Z3 | `windows-compile-smoke-z3`: Ninja + `clang-cl`, `PRISM_Z3=ON`, builds `prism_core` + `prism` + `prism-qa` + phase-7 `prism_tests` subset including `test_leanbb.cpp` (~44 min cold; cache `build-win-z3`; needs `z3gen_shim.bat`). Remaining: native `cl` without clang-cl. |
| **6** (partial) | QA on Windows | `yaml_subset` + `task` + `pyrandom` on `prism::i128` (done); `prism-qa` in `windows-compile-smoke`; conformance tree stays `if(UNIX)` |
| **7** (done) | Windows `prism_tests` subset | `PRISM_TESTS_WIN_SUBSET`: `test_main_win.cpp` plus portable doctest TUs (`test_wide_int`, `test_interval`, `test_svcomp`, `test_naming`, `test_polyglot`, `test_taxonomy`, `test_repolint`, `test_deps`, `test_cparse`, `test_sarif`, `test_astlint`, `test_ltl`, `test_contracts`, `test_bmc`; `test_leanbb` when `PRISM_HAS_Z3`). CI runs `prism_tests.exe` in `windows-compile-smoke` (`PRISM_Z3=OFF`) and `windows-compile-smoke-z3` (`PRISM_Z3=ON`). |

**Dependency order:** 2 → 3 (public API) → 4 (largest volume) → 5 → 6 → 7 (subset) → 5b (Z3 + full tests).

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
  `prism_core`, `prism`, `prism-qa`, and the phase-7 `prism_tests` subset with `PRISM_Z3=OFF`
  (see `.github/workflows/ci.yml`).
- **Phase 5b (CI):** `windows-compile-smoke-z3` builds `prism_core`, `prism`,
  `prism-qa`, and the phase-7 `prism_tests` subset with Ninja + `clang-cl` and
  `PRISM_Z3=ON` (vendored Z3 via `cmake/z3.cmake` and `z3gen`; no Python on PATH).
  Configure sets `PRISM_TESTS=ON` and `PRISM_TESTS_WIN_SUBSET=ON`; the job runs
  `prism_tests.exe` so solver / `leanbb` paths compile and execute against vendored Z3
  (cert-chain cases no-op when CaDiCaL / Lean tools are absent). Windows needs
  `z3gen_shim.bat` (not `.exe` batch text) so `FindPython3` can execute the shim during
  Z3 configure. Cache key includes `cmake/z3-generated/tree_sha256`. **Still open:** MSVC
  `cl` without clang-cl, full Windows `prism_tests` (unistd-heavy TUs beyond the subset),
  and ccache for Z3 on Windows runners (first vendored Z3 build ~44 min on `windows-latest`
  as of 2026-10-22; `build-win-z3` cache helps reruns).
- **Phase 7 (Windows `prism_tests` subset):** `windows-compile-smoke` and
  `windows-compile-smoke-z3` set `PRISM_TESTS=ON` and `PRISM_TESTS_WIN_SUBSET=ON` (see
  `CMakeLists.txt`). Configure fails on WIN32 if tests are on without the subset flag. The
  subset links `test_main_win.cpp` plus portable TUs (`test_wide_int`, `test_interval`,
  `test_svcomp`, `test_naming`, `test_polyglot`, `test_taxonomy`, `test_repolint`,
  `test_deps`, `test_cparse`, `test_sarif`, `test_astlint`, `test_ltl`, `test_contracts`,
  `test_bmc`; `test_leanbb` when `PRISM_HAS_Z3`); the monolithic `test_main.cpp` stays off
  WIN32 builds.
- **`i128_and` on MSVC fallback (`acc88121b`):** the non-`PRISM_HAS_INT128` path in
  `wide_int.hpp` now implements bitwise `&` via `i128_and` (used by SV-COMP debug IR and
  `test_wide_int`); fixes Windows subset link/compile after the portable 128-bit migration.
- **SV-COMP exec on WIN32:** `test_svcomp.cpp` gates replay/solve/pack cases that execute
  task code or shell stubs behind `svcomp_exec_portable()` (`NOTRUN` on `_WIN32`; Law 9).
- **`test_leanbb.cpp` on Windows:** no `unistd.h`; the file uses `std::filesystem` temp dirs
  only. It is in the phase-7 subset when `PRISM_HAS_Z3` (Z3 smoke job). Certified-mode cases
  early-return when `prism-bitblast` / `cake_lpr` / CaDiCaL are missing.
- **`prism_tests` full suite on Windows (still blocked):** default `PRISM_TESTS` on Linux/macOS
  is unchanged. On WIN32, several doctest TUs still `#include <unistd.h>` unconditionally
  (e.g. `tests/cpp/test_lint_corpus.cpp`, `tests/cpp/test_checkers_cxx.cpp`,
  `tests/cpp/test_pir_mem.cpp`). The monolithic `test_main.cpp` (solver integration and
  process-tree tests) is not linked on WIN32. Conformance QA remains behind `if(UNIX)` in
  `CMakeLists.txt`. Next step: guard or port unistd-heavy TUs and link `test_main.cpp` on
  WIN32.
- Witness and regress integer formatting unchanged on golden strings in `tests/cpp/`.
