# MSVC / `__int128` portability (native Windows `prism_core`)

**Epic:** build `prism_core` and `prism` with MSVC (`cl`) on Windows without relying on
WSL. **Was:** GCC/Clang `__int128` in several `prism_core` TUs (no MSVC/`clang-cl` ABI type).
Phases 2–4 route those paths through `prism::i128` (`wide_int.hpp`).

**Audit base:** `main` @ `f377ba3d7` (2026-10-23). Native `__int128` uses in `prism_core`
are migrated to `prism::i128` (phases 2–4). Remaining substring hits are string-only
BMC/astlint tables or `wide_int.hpp` under `PRISM_HAS_INT128`.

**CI (phases 5a / 5b / 5c / 7):** green on `main` @ `9230cd9ac` (full `test_adapters` + launcher).
`fba44a6d9` adds full `test_sanitize.cpp` + `fake_cc.exe`; Windows smokes `clang-cl` and `cl`
passed the phase-7 subset on run `39963224802` (Z3 smokes use the same tests). Linux
`prism_tests` and the shrunk Python suite stay green.

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
| **conformance tree** | In `src/tools/qa/`; part of `prism_qa` on WIN32 (`run_tree` is the Job Object runner). |

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
| `src/prism/stages/interp.cpp` | 1 | `__int128` in scanned source only; host overflow uses `__builtin_*_overflow` on Clang/GCC and manual / `prism::i128` checks on MSVC `cl` (phase 5c). |
| `src/prism/astlint_checks.cpp` | 5 | Width tables / type names (includes `__int128` as string). |

`src/prism/astlint_libclang.cpp` references libclang `CXType_Int128` enums only (not
`__int128` in PRISM sources).

### QA tools (`native`; conformance tree builds on WIN with `proctree_win.cpp`, a Job Object `run_tree`)

| File | ~matches | Role |
|------|----------|------|
| `src/tools/qa/support/yaml_subset.hpp` | — | Migrated: `Node::as_int()` → `optional<prism::i128>` (phase 6 partial). |
| `src/tools/qa/support/yaml_subset.cpp` | — | Uses `parse_i128` / `format_i128` (phase 6 partial). |
| `src/tools/qa/support/task.hpp` / `task.cpp` | — | Migrated: `prism::i128`; built on all platforms (phase 6). |
| `src/tools/qa/support/replay.cpp` | — | Migrated: `prism::u128` / `i128_shl` (phase 6); portable temp dirs, builds on WIN (`run_tree` is real there; the UBSan+ASan toolchain replay is not exercised). |
| `src/tools/qa/support/pyrandom.hpp` | — | Migrated: `prism::i128` / `u128` (phase 6); header-only, used by unix replay/soundness. |

All of `prism_qa` (report, cli, engine, fetch, replay, conformance, gen_random, soundness)
now builds on Windows (`/utf-8`). Temp dirs come from `qa::make_unique_dir` (no `mkdtemp`);
`proctree.cpp` (POSIX sessions) is replaced by `proctree_win.cpp`: `run_tree` calls
`prism::detail::run` (`src/prism/proc.cpp`), whose Windows path starts the child suspended,
puts it in a **Job Object** (`KILL_ON_JOB_CLOSE`, optional per-process memory cap) and gives
it only its three standard handles (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). A timeout or
`RunSpec::stop` terminates the whole job; `RunSpec::kill_descendants` (set by `run_session`)
also ends what the child left behind. A child that cannot be started or put in the job is
`start_failed` / `failed` (Laws 1, 7); a `--no-*-check` flag is refused (Law 8). `which()` is
PATH + PATHEXT. The sanitizer (UBSan+ASan, bwrap) replay toolchain is still not exercised
on Windows.
### Tests (outside `src/` / `include/` but coupled)

| File | Notes |
|------|-------|
| `tests/cpp/test_svcomp.cpp` | `static_cast<__int128>(…)` in witness literal check. |
| `tests/cpp/test_main.cpp` | Solver integration tests run when `PRISM_HAS_Z3` (including Windows). |
| `tests/cpp/test_qa_conformance.cpp` | Migrated: `prism::i128`, portable `QaTmp` / `self_dir` / `fake_engine`; in the WIN subset (static slice). |

---

## 2. Portable helper (`include/prism/wide_int.hpp`)

**`prism::i128` / `prism::u128`** with `PRISM_HAS_INT128` fast path (Clang/GCC) and
`{hi, lo}` fallback for MSVC. Shared **`parse_i128`** / **`format_i128`**, bitwise
`u128` ops, and unit tests in `tests/cpp/test_wide_int.cpp`.

The fallback `i128` also provides `<<`, `>>` (arithmetic), `++`/`--`, `+=`, `/`, `%` (truncating),
bit ops and explicit integral / `double` casts, so generic QA code (`gen_random`, `replay`,
`pyrandom`) compiles unchanged on MSVC.

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
| **5b** (CI done) | Windows CI + Z3 | `windows-compile-smoke-z3`: Ninja + `clang-cl`, `PRISM_Z3=ON`, builds `prism_core` + `prism` + `prism-qa` + phase-7 `prism_tests` subset including `test_leanbb.cpp` (~44 min cold; cache `build-win-z3`; needs `z3gen_shim.bat`). |
| **5c** (CI done) | Native MSVC `cl` | `windows-compile-smoke-cl`: Ninja + `cl` (not `clang-cl`), `PRISM_Z3=OFF`, same targets/subset as 5a. `stages/interp.cpp` uses `prism::i128` / manual checks instead of `__builtin_*_overflow` (clang-cl only). Configure with default `CMAKE_BUILD_TYPE=Release` (Debug + `prism_core` `/O2` hits cl D8016 vs `/RTC1`). |
| **6** (partial) | QA on Windows | `yaml_subset` + `task` + `pyrandom` on `prism::i128` (done); `prism-qa` in `windows-compile-smoke`; conformance / soundness / replay compile on WIN; `proctree_win.cpp` + `proc.cpp` Job Object `run_tree` (commit B), `test_qa_conformance.cpp` release-gate / solver-kill / `run_tree` cases run in the subset (`fake_prism.exe` + `.lead` sidecar, `fake_session_probe` `spawn-sleep` / `spawn-bg` / `alloc`) |
| **7** (done) | Windows `prism_tests` subset | `PRISM_TESTS_WIN_SUBSET` in `CMakeLists.txt` (inventory: [§6](#6-acceptance-criteria)). WIN: full `test_adapters.cpp`, `test_sanitize.cpp` (`fake_cc`), `test_qa.cpp` (`fake_session_probe`). Static splits remain reference-only on WIN. `test_cli` / `test_pipeline` omit POSIX-only cases; `test_conc` skips without Z3. `test_pir_e2e` skips the full `tests/pir` table on `_WIN32` (MESSAGE). All four Windows smoke jobs build and run `prism_tests.exe`. |
| **8** (CI added) | Qt GUI on Windows | `windows-gui-qt`: Ninja + `cl`, Qt 6 via aqtinstall, `PRISM_QT=ON`, `PRISM_Z3=OFF`; `prism_gui_tests.exe` and the `gui model` doctest suite offscreen, plus the `--smoke-screenshot` smoke. See [§7](#7-remaining-work-backlog). |

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

Status on `main` @ `1dfe4eabc`: phases **5a**, **5b**, **5c**, and **7** below are **met** (four Windows smokes green on Job Object + conformance slice; Z3 jobs track the same subset).
Remaining epic work is listed in [§7](#7-remaining-work-backlog).

- **Phase 5a (CI smoke) — met:** `windows-compile-smoke` configures with `clang-cl` and builds
  `prism_core`, `prism`, `prism-qa`, and the phase-7 `prism_tests` subset with `PRISM_Z3=OFF`,
  then runs `prism_tests.exe` (see `.github/workflows/ci.yml`).
- **Phase 5c (native `cl`) — met:** `windows-compile-smoke-cl` mirrors 5a with
  `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`, `CMAKE_BUILD_TYPE=Release`, and the same
  subset run.
- **Phase 5b (CI + Z3) — met:** `windows-compile-smoke-z3` builds the same targets with Ninja +
  `clang-cl` and `PRISM_Z3=ON` (vendored Z3 via `cmake/z3.cmake` and `z3gen`; no Python on PATH),
  runs `prism_tests.exe` (solver / `leanbb` / certified paths; cert-chain cases no-op when
  CaDiCaL / Lean tools are absent). `z3gen_shim.bat` satisfies `FindPython3` during Z3 configure;
  cache key includes `cmake/z3-generated/tree_sha256` (`build-win-z3`; cold Z3 build ~44 min on
  `windows-latest` as of 2026-10-22).
- **Phase 7 (Windows `prism_tests` subset) — met:** all three Windows smoke jobs set
  `PRISM_TESTS=ON` and `PRISM_TESTS_WIN_SUBSET=ON` (see `CMakeLists.txt`). Configure fails on
  WIN32 if tests are on without the subset flag. Inventory:

  | Kind | Sources |
  |------|---------|
  | Doctest main | `tests/cpp/test_main.cpp` (shared with Unix; `test_main_win.cpp` retired) |
  | Adapter fakes on WIN32 | Full `test_adapters.cpp` + `fake_tool_launcher` (stdio forwarded to `cmd.exe`); `fake_cmd` / `help_then_cmd` / `sh_to_cmd` |
  | Sanitize fakes on WIN32 | Full `test_sanitize.cpp` + `fake_cc.exe` (PE compiler/runner with `.fakebody` sidecar) |
  | QA on WIN32 | Full `test_qa.cpp` + `fake_session_probe.exe` (incl. Job Object timeout / background-grandchild / memory-cap cases); fork and SIGTERM-to-runner cases POSIX-only |
  | Conformance on WIN32 | `test_qa_conformance.cpp`: suite shape, scorer rules, `PyRandom` / generator parity, plus the release gate (`fake_prism.exe` copied per status with a `NAME.exe.lead` sidecar), the solver-kill timeout and `run_tree` (via `fake_session_probe.exe`); only the label self-check (UBSan+ASan replay) reports `MESSAGE: NOTRUN` |
  | GUI fakes on WIN32 | `fake_prism_gui.exe` (`prism --gui` launcher, `test_gui_model.cpp`) in `prism_tests`; `fake_gui_prism.exe` (window run/done/fail, `test_gui.cpp`) in `prism_gui_tests` (`PRISM_QT=ON` only) |
  | Static splits (reference only on WIN) | `test_adapters_static.cpp`, `test_sanitize_static.cpp`, `test_qa_static.cpp` |
  | Portable doctest TUs | `test_wide_int`, `test_interval`, `test_svcomp`, `test_naming`, `test_polyglot`, `test_taxonomy`, `test_repolint`, `test_deps`, `test_cparse`, `test_sarif`, `test_astlint`, `test_ltl`, `test_contracts`, `test_bmc`, `test_checkers_cxx`, `test_checkers_c`, `test_gen_tables`, `test_bmc_svcomp`, `test_havoc`, `test_verdict_repo`, `test_diff`, `test_journal_confidence`, `test_thread_taint`, `test_wp_harness`, `test_concrete`, `test_concolic`, `test_lint_corpus`, `test_pir_mem`, `test_ai_assist`, `test_pbsd`, `test_pir3`, `test_pir_refinement`, `test_cli`, `test_pipeline`, `test_ai9`, `test_conc`, `test_cxx_models`, `test_fuse`, `test_gui_model`, `test_repair`, `test_pir_e2e` |
  | When `PRISM_HAS_Z3` | `test_leanbb`, `test_pir_budget`, `test_certified` |

  **Not in subset (by design):** other unistd / `fork` / socket-heavy TUs. `test_main.cpp` is the
  doctest main and its cases run on WIN32; BMC-driven cases there (goto / shift rules, fuzz-goal
  budget) are `NOTRUN` without `PRISM_HAS_Z3`, and the `SolverTmp` shell fakes use
  `fake_tool_launcher` + `.cmd` (`cmd_tool`) on WIN32.
  `test_pipeline` chmod/LTL case is POSIX-only; `test_cli` binary integration is POSIX-only.
  On WIN32, `test_pir_e2e` omits the large `tests/pir` verdict table (MESSAGE); Law 9 /
  `--fp-checks` / `--strict-aliasing` smoke cases still run when `clang`/`opt` are on PATH.
- **`i128_and` on MSVC fallback (`acc88121b`):** the non-`PRISM_HAS_INT128` path in
  `wide_int.hpp` now implements bitwise `&` via `i128_and` (used by SV-COMP debug IR and
  `test_wide_int`); fixes Windows subset link/compile after the portable 128-bit migration.
- **SV-COMP exec on WIN32:** `test_svcomp.cpp` gates replay/solve/pack cases that execute
  task code or shell stubs behind `svcomp_exec_portable()` (`NOTRUN` on `_WIN32`; Law 9).
- **`test_leanbb.cpp` on Windows:** no `unistd.h`; the file uses `std::filesystem` temp dirs
  only. It is in the phase-7 subset when `PRISM_HAS_Z3` (Z3 smoke job). Certified-mode cases
  early-return when `prism-bitblast` / `cake_lpr` / CaDiCaL are missing.
- **`test_certified.cpp` on Windows:** in the Z3 subset with `test_leanbb` / `test_pir_budget`.
  CaDiCaL / `cake_lpr` cases no-op when the cert chain is absent; `PRISM_CERT_CACHE_MAX` env
  cap checks, the orphan work-dir sweep (`fork`), and the resident-memory OOM checker test stay
  POSIX-only (`#ifndef _WIN32`). Fake `#!/bin/sh` checker wrappers run only when the real tools
  are installed (typically not on Windows CI).
- **WIN32 test parity notes:** `tm_setenv` / `tm_unsetenv` (`_putenv_s`) in `test_main.cpp` and selected TUs;
  `test_checkers_c` / `test_concolic` mention `fork` only inside scanned snippet strings.
  `test_diff` PATH-mutation cases are POSIX-only. `Config::which_adapter` on WIN32 accepts
  `--tool` paths without `.exe` when `tool.exe` exists (parity with `SearchPath`).
- Witness and regress integer formatting unchanged on golden strings in `tests/cpp/`.

---

## 7. Remaining work (backlog)

Epic closure for phases 5a / 5b / 5c / 7 does **not** imply full Windows parity with Linux
`prism_tests` or `prism-qa conformance`. Ordered backlog:

| Item | Notes |
|------|--------|
| **`test_main.cpp` on WIN32** | **Done:** replaces `test_main_win.cpp` in the subset. Fixes: open files closed before `remove_all`, binary-mode source writes (no CRLF), `generic_string()` paths in `compile_commands.json`, Z3 guards for BMC cases, `.cmd` fakes for the portfolio solver tests (Z3-on path compiled but only exercised by the Z3 smoke job). Local note: a Microsoft Store `python3.exe` stub on PATH makes the polyglot python-syntax test fail; remove it from PATH. |
| **Qt GUI on Windows (CI)** | **Added:** `windows-gui-qt` builds `prism`, `prism_gui`, `prism_gui_tests` and `prism_tests` with native `cl`, Qt 6.8.3 (`win64_msvc2022_64`, installed by `jurplel/install-qt-action`, i.e. aqtinstall, cached), `PRISM_QT=ON`, `PRISM_Z3=OFF`. It runs `prism_gui_tests.exe` and the `prism_tests --test-suite="gui model"` suite (both offscreen) and the optional `prism_gui.exe … --smoke-screenshot` smoke (PNG uploaded as an artifact). Qt installs in about 1–2 min with the cache, so the cmake-only fallback was not needed. Green on `main` @ `f5af2c33b` (run `39985974306`, all nine jobs). |
| **`test_gui_model.cpp` / `test_gui.cpp` on WIN32** | **Ported:** `self_dir()` (`GetModuleFileNameW`), `prism.exe` / `prism_gui.exe` sibling names, a `cmd.exe` `run_shell` (`_popen`, outer quotes) with a scrubbed `PATH` (System32 only). `find_prism_gui` runs on WIN32 (no execute bit: the "not executable" step is POSIX-only). `prism --gui` cases use `fake_prism_gui.exe` (records its argv to `NAME.args`, exits 7: the launcher's `CreateProcess` does not pass stdout through) and a non-PE `prism_gui.exe` for the spawn-failure `ERROR` case (`CreateProcess error`). Window tests: the `/bin/sh` fake `prism` becomes `fake_gui_prism.exe` with `.report` / `.code` sidecars, and the real-run case finds `prism.exe` beside `prism_gui_tests.exe`. Still POSIX-only: "prism_gui without a display" (Windows always has a window station). |
| **Full QA doctest TUs** | **Done** for CI subset: `test_qa.cpp`, adapters, sanitize, conformance static+process slice. Fork/`kill -9`/SIGTERM session cases stay POSIX. |
| **Phase 6 conformance on Windows** | `test_qa_conformance.cpp` in subset; label self-check still `NOTRUN` without UBSan+ASan toolchain. |
| **`windows-compile-smoke-cl-z3` (CI)** | Same four smokes as 5a/5b/5c; confirm on latest `main` after each push. |
| **Z3 build time on Windows** | Optional ccache / stronger caching for vendored Z3 on `windows-latest`. |

No Python-engine parity test is required for `stages/interp.cpp` overflow on the MSVC path
(concrete interpreter is C++-only in CI).
