# MSVC / `__int128` portability (native Windows `prism_core`)

**Epic:** build `prism_core` and `prism` with MSVC (`cl`) on Windows without relying on
WSL. **Was:** GCC/Clang `__int128` in several `prism_core` TUs (no MSVC/`clang-cl` ABI type).
Phases 2–4 route those paths through `prism::i128` (`wide_int.hpp`).

**Audit base:** `main` @ `f377ba3d7` (2026-10-23). Native `__int128` uses in `prism_core`
are migrated to `prism::i128` (phases 2–4). Remaining substring hits are string-only
BMC/astlint tables or `wide_int.hpp` under `PRISM_HAS_INT128`.

**Epic status:** phases 1–8 (5a / 5b / 5c included) are **done**. Phase 6 (QA on Windows)
is closed: the label self-check (UBSan+ASan replay) sample **passes** on the `clang-cl`
smoke, and is `NOTRUN` (logged, Law 1) wherever no clang links the sanitizers, which is
the case on the MSVC `cl` smoke. Latest green CI on `main` is `3b06ee7be`: run
[`40043699695`](https://github.com/odin-loki/PRISM/actions/runs/40043699695) passed all
ten jobs (four Windows smokes, Qt GUI, Linux `prism_tests`, shrunk Python suite,
`python-windows`, licence firewall, SARIF self-check). The fake-HTTP `repair` test runs on
WIN32 (Winsock fake llama-server) and the `python-windows` job is green (39 passed). That
run includes the Windows `regress --run` and `test_ai_assist` / `test_cxx_models` probe
changes. What is left is listed in [§7](#7-remaining-work-backlog).

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
PATH + PATHEXT. The sanitizer (UBSan+ASan) replay toolchain runs on Windows with clang only
(gcc is skipped in the probe, `-lm` is not passed, bwrap is absent so replay is unsandboxed); the clang-cl CI jobs put the clang ASan DLL dir on PATH. When
the probe cannot link UBSan+ASan the label self-check is `NOTRUN`.
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
| **6** (done) | QA on Windows | `yaml_subset` + `task` + `pyrandom` on `prism::i128` (done); `prism-qa` in `windows-compile-smoke`; conformance / soundness / replay compile on WIN; `proctree_win.cpp` + `proc.cpp` Job Object `run_tree` (commit B), `test_qa_conformance.cpp` release-gate / solver-kill / `run_tree` cases run in the subset (`fake_prism.exe` + `.lead` sidecar, `fake_session_probe` `spawn-sleep` / `spawn-bg` / `alloc`); label self-check sample passes on the `clang-cl` smoke (run `40038665356`), `NOTRUN` where no clang links UBSan+ASan |
| **7** (done) | Windows `prism_tests` subset | `PRISM_TESTS_WIN_SUBSET` in `CMakeLists.txt` (gates only the `fake_*` stand-ins; sources are the shared Linux list) (inventory: [§6](#6-acceptance-criteria)). WIN: full `test_adapters.cpp`, `test_sanitize.cpp` (`fake_cc`), `test_qa.cpp` (`fake_session_probe`). Static splits remain reference-only on WIN. `test_cli` (binary cases included) and `test_diff` run in full; `test_pipeline` omits only the POSIX `PATH`-guard esbmc-missing case; `test_conc` skips without Z3. `test_pir_e2e` runs the C-only subset of the `tests/pir` table on `_WIN32` when `clang` + `opt` are found, and prints `NOTRUN` (MESSAGE) otherwise ([§6](#6-acceptance-criteria)). All four Windows smoke jobs build and run `prism_tests.exe`. |
| **8** (done) | Qt GUI on Windows | `windows-gui-qt`: Ninja + `cl`, Qt 6 via aqtinstall, `PRISM_QT=ON`, `PRISM_Z3=OFF`; `prism_gui_tests.exe` and the `gui model` doctest suite offscreen, plus the `--smoke-screenshot` smoke. See [§7](#7-remaining-work-backlog). |

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

Status on `main` @ `3b06ee7be` (latest green, run [`40043699695`](https://github.com/odin-loki/PRISM/actions/runs/40043699695), all ten jobs success; the job-log details below were read from the earlier run on `1f59511ab`): phases **5a**, **5b**, **5c**, **6**, **7** and
**8** below are **met**. Evidence: run
[`40038665356`](https://github.com/odin-loki/PRISM/actions/runs/40038665356)
(`PRISM CI` @ `1f59511ab`) finished **success** on all ten jobs: `Windows compile smoke`
(`clang-cl`), `Windows compile smoke (MSVC cl)`, `Windows compile smoke (Z3)`,
`Windows compile smoke (MSVC cl, Z3)`, `Windows GUI (Qt 6, MSVC cl, offscreen)`,
`C++ engine (build + prism_tests)`, `Remaining pytest (shrunk suite)`,
`Remaining pytest (shrunk suite, Windows)`, `Licence firewall + manifest + SBOM`
and the SARIF self-check. Read from the job logs: the `clang-cl` smoke ran 722 doctest cases,
all passed, and logged no "label self-check sample NOTRUN" (the ASan DLL dir was put on PATH,
so the sample ran); the `cl` smoke logged that NOTRUN line, as designed. In the same run the
`repair` cases run on WIN32 (fake llama-server over Winsock; the verdict case is `NOTRUN`
in the `PRISM_Z3=OFF` smokes because there is no BMC verdict on the candidate), and
`python-windows` reported `39 passed` (the job is now gating; `continue-on-error` removed). The run includes
the shared `prism_tests` source list (`447e6bca2`), the WIN32 label self-check with clang
(`2ef3ba701`) and the ASan PATH step fix (`371010f81`). Remaining epic work is listed in [§7](#7-remaining-work-backlog).

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
- **Phase 8 (Qt GUI on Windows) — met:** `windows-gui-qt` (Ninja + `cl`, Qt 6.8.3
  `win64_msvc2022_64`, `PRISM_QT=ON`, `PRISM_Z3=OFF`) builds `prism`, `prism_gui`,
  `prism_gui_tests` and `prism_tests`, runs `prism_gui_tests.exe` and the `gui model` doctest
  suite offscreen, and the `prism_gui.exe … --smoke-screenshot` smoke. Green on all nine jobs
  @ `f5af2c33b` (run `39985974306`). Only "prism_gui without a display" stays POSIX-only
  (Windows always has a window station).
- **Phase 7 (Windows `prism_tests` subset) — met:** all three Windows smoke jobs set
  `PRISM_TESTS=ON` and `PRISM_TESTS_WIN_SUBSET=ON` (see `CMakeLists.txt`).

  **Epic note — the WIN "subset" inventory equals the Linux `prism_tests` sources.** There is
  now one shared `_prism_test_sources` list in `CMakeLists.txt` for every platform (49 TUs plus
  `test_leanbb` / `test_pir_budget` / `test_certified` when `PRISM_HAS_Z3`); platform differences
  live inside the test files (`#ifdef _WIN32`), not in the source list. `PRISM_TESTS_WIN_SUBSET`
  survives only as the WIN32 switch that builds the `fake_*` stand-in executables and defines the
  `PRISM_FAKE_*` paths. The old "Full prism_tests does not build on WIN32 yet" `FATAL_ERROR` is
  gone; WIN32 with tests on but the flag off now configures with a warning (the fake-dependent
  cases cannot find their stand-ins), so keep `-DPRISM_TESTS_WIN_SUBSET=ON` on Windows. The
  name "subset" is historical. Inventory:

  | Kind | Sources |
  |------|---------|
  | Doctest main | `tests/cpp/test_main.cpp` (shared with Unix; `test_main_win.cpp` retired) |
  | Adapter fakes on WIN32 | Full `test_adapters.cpp` + `fake_tool_launcher` (stdio forwarded to `cmd.exe`); `fake_cmd` / `help_then_cmd` / `sh_to_cmd` |
  | Sanitize fakes on WIN32 | Full `test_sanitize.cpp` + `fake_cc.exe` (PE compiler/runner with `.fakebody` sidecar) |
  | Certified on WIN32 | `test_certified.cpp` in full when `PRISM_HAS_Z3`, with `fake_session_probe.exe` (sweep) and `fake_cake_lpr.exe` (checker OOM) and `fake_mem_hog.exe` (resident-memory kill) stand-ins; no `NOTRUN` left |
  | QA on WIN32 | Full `test_qa.cpp` + `fake_session_probe.exe` (incl. Job Object timeout / background-grandchild / memory-cap cases); fork and SIGTERM-to-runner cases POSIX-only |
  | Conformance on WIN32 | `test_qa_conformance.cpp`: suite shape, scorer rules, `PyRandom` / generator parity, plus the release gate (`fake_prism.exe` copied per status with a `NAME.exe.lead` sidecar), the solver-kill timeout and `run_tree` (via `fake_session_probe.exe`); the label self-check (UBSan+ASan replay) runs with clang and is `NOTRUN` (`WARN_MESSAGE`) when no clang links the sanitizers |
  | GUI fakes on WIN32 | `fake_prism_gui.exe` (`prism --gui` launcher, `test_gui_model.cpp`) in `prism_tests`; `fake_gui_prism.exe` (window run/done/fail, `test_gui.cpp`) in `prism_gui_tests` (`PRISM_QT=ON` only) |
  | Static splits (reference only on WIN) | `test_adapters_static.cpp`, `test_sanitize_static.cpp`, `test_qa_static.cpp` |
  | Portable doctest TUs | `test_wide_int`, `test_interval`, `test_svcomp`, `test_naming`, `test_polyglot`, `test_taxonomy`, `test_repolint`, `test_deps`, `test_cparse`, `test_sarif`, `test_astlint`, `test_ltl`, `test_contracts`, `test_bmc`, `test_checkers_cxx`, `test_checkers_c`, `test_gen_tables`, `test_bmc_svcomp`, `test_havoc`, `test_verdict_repo`, `test_diff`, `test_journal_confidence`, `test_thread_taint`, `test_wp_harness`, `test_concrete`, `test_concolic`, `test_lint_corpus`, `test_pir_mem`, `test_ai_assist`, `test_pbsd`, `test_pir3`, `test_pir_refinement`, `test_cli`, `test_pipeline`, `test_ai9`, `test_conc`, `test_cxx_models`, `test_fuse`, `test_gui_model`, `test_repair`, `test_pir_e2e` |
  | When `PRISM_HAS_Z3` | `test_leanbb`, `test_pir_budget`, `test_certified` |

  **Not in subset (by design):** other unistd / `fork` / socket-heavy TUs. `test_main.cpp` is the
  doctest main and its cases run on WIN32; BMC-driven cases there (goto / shift rules, fuzz-goal
  budget) are `NOTRUN` without `PRISM_HAS_Z3`, and the shell fakes use `fake_tool_launcher` +
  `.cmd` (`SolverTmp::cmd_tool`, `win_cmd_tool`) on WIN32. `PathGuard` / `ToolsDirGuard` use
  `tm_setenv` (`_putenv_s`), so these cases run on WIN32 too: polyglot missing tools
  (`PATH` pointed at a nonexistent directory; `shell-syntax` is not asserted there because
  `SearchPath` finds the System32 WSL `bash.exe` first), `perl -c` / hostile tree held without
  `--allow-exec`, pinned adapters never taken from inside the scanned tree (fake `esbmc.exe`
  under `PRISM_TOOLS_DIR`), `tool_identity` + a fake pinned `cppcheck.exe`, and the polyglot
  "unparsed tool output is ERROR" case (cmd text; cmd splits `--output-format=concise` at `=`).
  The `test_pipeline` unreadable-directory / LTL case runs on WIN32 with an `icacls` deny-read
  ACL in place of `chmod` (lifted before cleanup; without `icacls`, or if the deny does not hide
  the directory, it says so with a `MESSAGE` and walks a readable tree). `test_cli` binary
  integration runs on WIN32: `prism.exe` is found next to `prism_tests.exe` with
  `GetModuleFileNameW`, started with `_popen` and the cmd.exe quoting, and checked for the same
  exit codes (2 on bad arguments, 1 for `--fail-on=defect`, 0 otherwise).
  On WIN32, `test_pir_e2e` runs the C-only subset of the `tests/pir` verdict table (the `.c` files and
  their rows; the C++ files need the libstdc++ model headers and `_GLIBCXX_ASSERTIONS`, so they print
  `NOTRUN: ... (C++) omitted on WIN32`), plus the Law 9 / `--fp-checks` / `--strict-aliasing` cases,
  when `clang` and `opt` are found. When either is missing every case prints `NOTRUN: pir e2e needs
  clang and opt on PATH (...)` (Law 1) and returns; the stock Windows LLVM install has no `opt.exe`.
  The two Z3 jobs (`windows-compile-smoke-z3`, `windows-compile-smoke-cl-z3`) therefore fetch the
  official LLVM 18.1.8 MSVC archive, unpack only `bin/clang.exe` (installed as `clang-18.exe`, which
  `find_frontend` prefers, so the runner's `clang.exe` and the ASan label self-check are unchanged),
  `bin/opt.exe` and `lib/clang/18/include` to `C:\llvm18` (cached as `win-llvm-18.1.8-pir-v1`,
  best effort), and put it on `PATH` before the tests run. `tools/gen_test_pir_e2e_cpp.py` generates
  the guard; edit the generator, not the `.cpp`.
- **`i128_and` on MSVC fallback (`acc88121b`):** the non-`PRISM_HAS_INT128` path in
  `wide_int.hpp` now implements bitwise `&` via `i128_and` (used by SV-COMP debug IR and
  `test_wide_int`); fixes Windows subset link/compile after the portable 128-bit migration.
- **SV-COMP exec on WIN32:** `test_svcomp.cpp` gates replay/solve/pack cases that execute
  task code or shell stubs behind `svcomp_exec_portable()` (`NOTRUN` on `_WIN32`; Law 9).
- **`test_leanbb.cpp` on Windows:** no `unistd.h`; the file uses `std::filesystem` temp dirs
  only. It is in the phase-7 subset when `PRISM_HAS_Z3` (Z3 smoke job). Certified-mode cases
  early-return when `prism-bitblast` / `cake_lpr` / CaDiCaL are missing.
- **`test_certified.cpp` on Windows:** in the Z3 subset with `test_leanbb` / `test_pir_budget`.
  CaDiCaL / `cake_lpr` cases no-op when the cert chain is absent. The former POSIX-only cases now
  run on WIN32 too:
  - `PRISM_CERT_CACHE_MAX` cap checks use `_putenv_s` (`cert_setenv` / `cert_unsetenv`).
  - The orphan work-dir sweep (was `fork`): `sweep_orphan_work_dirs()` in `certstore.cpp` now
    has a WIN32 implementation (`OpenProcess` + `WaitForSingleObject`; a process that cannot be
    inspected counts as alive, only `ERROR_INVALID_PARAMETER` or an exited process is dead), and
    `make_work_dir` in `portfolio.cpp` names work dirs with `GetCurrentProcessId()` instead of
    `0`, so a dead PRISM's `prism-solve-*` dirs are reclaimed. The test uses
    `fake_session_probe.exe` (`exit 0` for the dead pid, `sleep 120` for a live one, ended with
    `TerminateProcess`), the same stand-in `test_qa` uses for its Job Object cases.
  - The checker-out-of-memory case (was a `#!/bin/sh` script): `fake_cake_lpr.exe`
    (`tests/cpp/fake_cake_lpr.cpp`, copied to `cake_lpr.exe`) prints the CakeML heap-exhausted
    line and records its argv; `check_lrat` must report "ran out of memory (CakeML heap
    exhausted)" and pass `--CML_HEAP_SIZE=<mb>`. It needs no external tool, so it never
    no-ops.
  - The "killed past the resident-memory cap" half (`PRISM_CHECKER_MEM` / the `heap_mb` cap of
    any checker other than `cake_lpr`, also `prism-bitblast`): the Linux watchdog reads
    `/proc/<pid>/statm` (`solver/util.cpp`); the Windows counterpart is
    `RunSpec::rss_cap_bytes` in `proc.cpp`. The Job Object runner samples the child's working
    set (`K32GetProcessMemoryInfo`, about every 100 ms, root process like the Linux watchdog)
    and `TerminateJobObject`s the whole tree once it passes the cap (`RunOut::rss_exceeded`,
    `peak_rss`); `solver::detail::run` maps that to `Proc::mem_exceeded`, so `check_lrat`
    reports "ran out of memory (resident memory passed the cap of N MB ...)" and never a
    verdict. This is deliberately not `JOB_OBJECT_LIMIT_PROCESS_MEMORY` / `mem_limit_mb`: that
    caps *committed* memory (the child sees failed allocations and may exit with any status),
    which is the `RLIMIT_AS` analogue, not a resident-memory kill. The case runs
    `fake_mem_hog.exe` (`tests/cpp/fake_mem_hog.cpp`, copied to `lrat-check.exe`; touches
    400 MB, then sleeps 20 s) against a 100 MB cap and requires the kill well before 15 s, so
    a missed kill fails instead of passing. A child whose working set cannot be read is not
    capped (`peak_rss` stays 0); that is visible, not reported as under the cap. Checked
    locally by a standalone harness (mingw g++): killed at 404 MB peak under a 100 MB cap, not
    killed under a 1000 MB cap; the full `prism_tests` run is left to CI (Z3 build).
- **WIN32 test parity notes:** `tm_setenv` / `tm_unsetenv` (`_putenv_s`) in `test_main.cpp` and selected TUs;
  `test_checkers_c` / `test_concolic` mention `fork` only inside scanned snippet strings.
  `test_diff` PATH-mutation cases (no gcc/clang, `cl` alone) run on WIN32 through
  `_putenv_s` (a PE `cl.exe` copy of `fake_tool_launcher` stands in for MSVC). The `scope:`
  symlink case runs on WIN32: a file symlink needs Developer Mode / `SeCreateSymbolicLinkPrivilege`
  and otherwise prints `MESSAGE: NOTRUN` (count of the link to a file not asserted); the
  directory link falls back to a `mklink /J` junction, which needs no privilege, and checks that
  it is not followed. `Config::which_adapter` on WIN32 accepts
  `--tool` paths without `.exe` when `tool.exe` exists (parity with `SearchPath`).
  `Config::which` on WIN32 probes a `python*.exe` under `\WindowsApps\` with `--version`
  (cached per path); a Microsoft Store stub (exit 9009 / cannot start) is passed over for the
  next python on PATH, and if there is none the tool is missing (`NOTRUN`, Law 1), not a tool
  failure. Covered by `config: which skips the Microsoft Store python stub`. The Python engine
  has the same probe since `8d1f9721f` (`prism/config.py`: `which_tool` / `resolve_adapter`
  with `_is_store_python_stub`, same 9009 / cannot-start / timeout rules, cached per path;
  `tests/test_config.py` fakes the platform and the probe, so it runs on every OS). The
  Python engine's engine tests are not run on Windows CI, but the shrunk suite is: see
  `python-windows` in [§7](#7-remaining-work-backlog).
- Witness and regress integer formatting unchanged on golden strings in `tests/cpp/`.

---

## 7. Remaining work (backlog)

Phases 5a / 5b / 5c / 6 / 7 / 8 are closed, but that does **not** imply full Windows parity
with Linux. What is still open:

| Item | Notes |
|------|--------|
| **POSIX-only test cases** | Still `#ifndef _WIN32` or `NOTRUN` on WIN32: the process-tree block in `test_main.cpp` (`fork` / signals; on WIN32 it prints `MESSAGE:` pointing at the Job Object cases in `test_qa.cpp`, which cover the equivalent), the file-symlink half of the `scope:` case (needs Developer Mode or `SeCreateSymbolicLinkPrivilege`; `NOTRUN` message when it cannot be created, the junction half always runs), the C++ half of the `tests/pir` verdict table in `test_pir_e2e` (the C half runs where `opt` is found; `NOTRUN` otherwise), SV-COMP replay/solve/pack (execute task code, Law 9), and "prism_gui without a display". Each is a deliberate gap, not a silent skip: they print `MESSAGE: NOTRUN` or are absent from the Windows build. |
| **Label self-check (phase 6, closed)** | Passes on the `clang-cl` smoke; `NOTRUN` on the `cl` smoke (verified on CI, run `40038665356`). Runs on Windows only when `sanitizers_usable()` finds a clang that links UBSan+ASan (clang-cl CI jobs put the ASan DLL dir on PATH); otherwise `NOTRUN` (`WARN_MESSAGE`). The same probe gates `test_ai_assist` "generated tests fail on the bug and pass on the fix": `regress --run` builds `bin/<name>.exe`, links no `-lm`, and passes `UBSAN_OPTIONS` / `ASAN_OPTIONS` as a child environment overlay; the `sh run_tests.sh` and cmake/ctest halves stay POSIX-only. `test_cxx_models` needs libstdc++ (`<bits/c++config.h>`): a clang on the MSVC STL is `NOTRUN` (still the case on CI), since the model headers replace libstdc++ containers. |
| **Python suite on Windows (gating job)** | `python-windows` in `ci.yml` runs the same four files as "Remaining pytest" (`test_ai9`, `test_ai_assist`, `test_lints_fastpath`, `test_llm`) on `windows-latest` (Python 3.11) with `PRISM_BIN` set to the `prism.exe` that `windows-compile-smoke-cl` uploads as the `prism-windows` artifact (so it adds no build time). It is gating (`continue-on-error` removed after two green runs), and it skips nothing quietly (`-rs`; a missing `prism.exe` fails the smoke step). Differences from Linux: no `shellcheck` (that polyglot tool is `NOTRUN`), no `libegl` (PySide6 runs offscreen). Locally it was green (39 passed) on Windows with Python 3.14, and on CI in runs `40033600830` and `40038665356` (39 passed each, Python 3.11, no skips). **WinError 193 fix:** `tests/test_ai_assist.py` and `prism/gui.py` (`prism_cpp_binary`) accepted the Linux ELF `build/prism` on Windows because `os.access(X_OK)` is true for any file, then `subprocess` failed with `[WinError 193]`; both now look for `prism.exe` on Windows and require a PE (`MZ`) image, otherwise the binary is "not found" (`NOTRUN`, Law 1). Not run on Windows: the full `tests/test_*.py` set (the repo's own shrunk-suite rule). Promoted to gating after the two green runs above (`continue-on-error` dropped). The check is named `Remaining pytest (shrunk suite, Windows)` (the "optional" suffix was dropped in `47ba80020`; any branch-protection rule that matched the old name must be updated). |
| **Z3 build time on Windows** | Both Z3 jobs restore/save the build dir with a `restore-keys` prefix (`75aeb6299`), and `libz3` is saved before the PRISM targets. Measured job wall time (`gh run view`), with the `Build vendored Z3 (libz3)` step in brackets: run `40019517666` (`371010f81`, which already includes `75aeb6299`; the `cl` job log shows an exact-key hit, so this run was not a true cold build either, only the first one measured): `clang-cl` 56m54s (libz3 33m25s), `cl` 44m08s (libz3 23m12s). Run `40038665356` (`1f59511ab`, exact-key cache hit on both): `clang-cl` 43m51s (libz3 25m01s), `cl` 28m44s (libz3 13m48s). Run `40043699695` (`3b06ee7be`, `restore-keys` prefix hit, then a new cache saved): `clang-cl` 78m18s (libz3 50m30s; the slowest of the three, so runner variance is large), `cl` 34m13s (libz3 16m31s). **Compared with `40019517666`, the later runs are only partly faster (`cl` -15m and -10m; `clang-cl` -13m, then +21m), and no run is warm in the sense of "libz3 is skipped":** every run restored the ~557 MB (`clang-cl`) / ~221 MB (`cl`) archive in under 20 s, yet `libz3` still compiled for 14-50 min, so the warm-cache time (a no-op `libz3` step) is still not achieved. Likely cause, not yet confirmed: a fresh checkout gives every source a newer mtime than the restored objects, so Ninja rebuilds them; the next step is to read `ninja -d explain` output in the libz3 step. sccache is not used: `cl` needs `/Z7`, which changes the Release flags of both smokes. |

Closed: `test_main.cpp` in full on WIN32, GUI tests ported, full QA doctest TUs,
`windows-gui-qt`, all four Windows compile smokes, the `Config::which` Store-stub probe
(C++ and Python), and the WIN32 polyglot / tools-dir / diff / cli / scope / ltl cases.

No Python-engine parity test is required for `stages/interp.cpp` overflow on the MSVC path
(concrete interpreter is C++-only in CI).
