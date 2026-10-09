# Plan: PRISM on C++23 only

**Goal (owner, 2026-09-30):** everything runs on C++23. No Python: the Python
engine (`prism/*.py`) is deleted, and every tool, script, test and CI step
that uses Python is ported to C++23 (CMake already builds with
`CMAKE_CXX_STANDARD 23`).

**Status (2026-10-09): C++ integration on `main`; phase-5 gate next.**

- **Merged on tree:** `prism-deps` (replaces `scripts/fetch_deps.py`,
  `licence_check.py`, `sbom.py`), unified **`prism-qa`** (`conformance`,
  `soundness`, dev QA commands), **`prism_docs_check`**, Qt GUI parity
  (`gui_model`, stages table, journal poll), native JSON/TOML in polyglot,
  `prism svcomp` in C++, table-driven **`tests/cpp/test_bmc.cpp`** and
  **`tests/cpp/test_concolic.cpp`** (maintained by `tools/gen_test_concolic_cpp.py`),
  and the bulk of phase-2 doctest ports (`tests/cpp/*`).
- **`tests/test_core.py`:** deleted — `TestLaws` / `TestBMC` / `TestLints` were
  already doctest-only; the last `TestClassify` cparse cases now live in
  `tests/cpp/test_main.cpp` (`cparse: testdata function kinds`).
- **Pytest:** **30** tracked files under `tests/test_*.py` (AI, SARIF, PIR
  refinement, naming, supply-chain parity helpers, etc.); the phase-5 allow-list
  is enforced in **`tests/cpp/test_deps.cpp`** (`deps: tracked .py files match
  the phase-5 allow-list`) on every `prism_tests` run.
- **CI:** `conformance.yml` / `self-check.yml` call `prism-qa`; supply-chain
  job builds `prism-deps`; `docs.yml` uses `prism_docs_check`.
- **Still open:** phase 5 (delete `prism/*.py` and the remaining pytest suite),
  phase 6 (Z3 without Python), `port/ai-gen-tools`, owner-only items in
  `ROADMAP_STATUS.md`.
- Appendix map is from audit `911b9f94f`; many rows are now **DONE** on C++
  even where the table still says partial — treat this status block as current.

## `prism/*.py` delete checklist (grep vs `src/prism`, 2026-10-09)

**Gate today:** `k_python_engine_deleted = false` in `tests/cpp/test_deps.cpp`.
**Pytest:** 29–30 files under `tests/test_*.py` still import `prism` (SARIF
moving to `tests/cpp/test_sarif.cpp`; not phase-5 minimal). **Do not delete
`prism/` yet.**

Legend: **C++** = runtime owner exists under `src/prism/` or `src/gui/`;
**partial** = C++ runs the stage but appendix gaps or pytest still pins Python;
**obsolete** = C++ is the reference (Python only ctypes/tests).

| `prism/*.py` | C++ owner (primary) | Ready to delete? |
|---|---|---|
| `__init__.py` | `capi.cpp`, `models.hpp`, `pipeline.hpp` | yes (API surface only) |
| `__main__.py` | `main.cpp`, `cli.cpp` | partial — CLI flag/exit parity |
| `adapters.py` | `adapters.cpp`, `config.cpp` | yes |
| `adapters_extra.py` | `adapters.cpp`, `stages/fuse.cpp` | partial — spatch/semgrep/cocci path bugs |
| `afl.py` | `stages/fuse.cpp` | partial — AFL env/cwd/sandbox extras |
| `agent.py` | `stages/hypothesize.cpp`, `fuse.cpp`, `execute_cex.cpp` | partial — `dafny_specs`, interpreter extras |
| `ai.py` | `stages/llm.cpp`, `ai/*.cpp` | partial — bindings/GGUF limits; pytest `test_llm.py` |
| `bmc.py` | `bmc.cpp`, `bmc_*.inc` | yes — `tools/gen_prism.py` still reads `.py` until removed |
| `checkers.py` | `checkers_*.cpp` | partial — branch/guard drift (phase 1) |
| `concolic.py` | `stages/concolic.cpp`, `bmc.cpp` | yes |
| `concrete.py` | `stages/interp.cpp`, `common.cpp` | partial — i64 args, `pack_args` test helper |
| `confidence.py` | `pipeline.cpp`, `verdict/verdict.cpp` | partial — no exported `score()` for tests |
| `config.py` | `config.cpp`, `threads.hpp` | yes |
| `contracts.py` | `stages/contracts.cpp` | partial — ACSL case-folding |
| `cparse.py` | `cparse.cpp` | yes |
| `diff.py` | `stages/diff.cpp` | yes |
| `fuse.py` | `stages/fuse.cpp` | yes |
| `fuzz.py` | `stages/fuse.cpp`, `common.cpp`, `interp.cpp` | partial — caches/parallel fuzz runs |
| `gui.py` | `src/gui/*`, `gui_model.cpp` | partial — Qt GUI missing journal/stages table |
| `harness.py` | `stages/harness_bmc.cpp`, `ai/harness.cpp` | yes |
| `inline.py` | `inline.cpp` | yes |
| `interval.py` | `interval.cpp` | yes |
| `journal.py` | `journal.cpp` | yes |
| `laws.py` | `laws.cpp`, `verdict.hpp` | yes — keep doctest parity for sets |
| `ltl.py` | `stages/ltl.cpp` | partial — FSM extraction algorithm |
| `models.py` | `models.cpp`, `journal.cpp` | partial — `extra` typed as strings in C++ |
| `muttest.py` | `stages/rapid.cpp` | partial — Law-9 / gcc scoring paths |
| `pbsd.py` | `adapters.cpp` (`run_pbsd_lints`) | partial — ParanoidBSD Python scanners |
| `pipeline.py` | `pipeline.cpp` | partial — LLM Dafny hooks, classify cache |
| `polyglot.py` | `polyglot.cpp`, `scope.cpp` | partial — **still runs embedded Python** for json-syntax |
| `rapid.py` | `stages/rapid.cpp` | partial — gcc fallback / tiny interpreter |
| `sandbox.py` | `sandbox.cpp` | yes — doctest ports of `test_exec_safety.py` open |
| `sanitize.py` | `adapters.cpp` | partial — parallel compile, triple parse |
| `sarif.py` | `sarif.cpp` | yes — engine module only; SARIF tests → `tests/cpp/test_sarif.cpp` |
| `scope.py` | `scope.cpp` | yes |
| `shipdocs.py` | `shipdocs.cpp` | yes |
| `simdmut.py` | `havoc.cpp`, `hash.cpp`, `simd.hpp` | **obsolete** — delete with `prism_native` C ABI |
| `taint.py` | `stages/taint.cpp` | yes |
| `taxonomy.py` | `taxonomy.cpp` | yes |
| `thread.py` | `stages/thread.cpp` | yes |
| `wp.py` | `stages/wp.cpp` | yes |

**Bundled data (not a `.py` row but blocks tree delete):** `prism/cocci/*.cocci`
— C++ `cocci_rules()` still resolves `prism/cocci` or `share/prism/cocci`;
move to `rules/cocci/` (or embed) before phase 5.

### Deletion blockers (summary)

1. **Pytest surface:** ~29 `tests/test_*.py` files (engine parity, GUI, PIR,
   fuzz, certified, …) — target end state is naming + SARIF golden (+ thin AI
   stubs if any), not reached.
2. **Runtime Python:** `polyglot.cpp` json-syntax check via `SYNTAX_HELPER`
   Python embed (phase 4).
3. **Partial stages:** checkers, ltl, pbsd, rapid, agent/ai/contracts/gui,
   polyglot resolve (`~` / pinned tools).
4. **Co-packaged rules:** `prism/cocci/` path hard-coded in adapters + tests.
5. **Generators:** `tools/gen_prism.py` / taxonomy bootstrap still mention
   `prism/bmc.py` until generators are deleted or repointed.
6. **Gate:** flip `k_python_engine_deleted` only after (1)–(5) and CI green
   without `python -m pytest` on the engine.

### Follow-up commit plan (execute only when blockers cleared)

Single integration commit (or stacked PR) on green `prism_tests` + CI:

1. Relocate `prism/cocci/` → installed `share/prism/cocci` (or CMake embed);
   update `adapters.cpp` search paths and doctest.
2. Native json/tomL syntax in `polyglot.cpp`; remove `SYNTAX_HELPER` exec.
3. Port or drop remaining pytest: parity → golden in `tests/cpp/`; delete
   engine-oracle tests; keep `test_naming.py` logic as doctest until `.py` gone.
4. `git rm -r prism/` `tests/**/*.py` `pyproject.toml` `requirements.txt`;
   remove `prism_native` / `simdmut` ctypes paths; delete obsolete `tools/gen_*.py`.
5. `tests/cpp/test_deps.cpp`: `k_python_engine_deleted = true`.
6. `CLAUDE.md` / workflows: one engine, no `python -m prism`, pytest job removed.

**This run:** pytest is not minimal; **no `prism/` delete executed.**

## Size

| part | files | lines | where it goes |
|---|---|---|---|
| Python engine `prism/*.py` | 41 | 50,264 | deleted; C++ engine already owns every stage (gaps below first) |
| tools + scripts | 37 | 9,189 | `prism` subcommands, `prism-qa`, `prism-deps`, `prism_ai`, CMake |
| Python tests `tests/**/*.py` | 96 | 40,082 | doctest in `tests/cpp/` or CTest drivers; parity-only tests deleted |
| C++ today | — | 87,290 | stays; grows by the ports |

## What cannot become C++ (the only exceptions)

1. **BenchExec tool-info** `tools/svcomp/prism.py` (78 lines). BenchExec
   imports tool-info modules as Python classes (`benchexec.tools.prism`).
   That is BenchExec's plugin interface, not PRISM code. It moves upstream
   into BenchExec for SV-COMP. Until then it stays in `tools/svcomp/` as the
   single allow-listed `.py` file. PRISM never runs it.
2. **Linters for scanned Python projects** (`ruff`, `pyflakes`, `mypy`,
   `yamllint` rows in `src/prism/polyglot.cpp`). They are external tools for
   the user's code, in the same way `eslint` or `clippy` are. PRISM does not
   ship them and reports `NOTRUN` when they are missing (Law 1). The
   `pytest` output format of `prism regress` stays for the same reason.
3. **`third_party/`** is never edited. llama.cpp's OpenCL and WebGPU back
   ends call `find_package(Python3)`, so CMake forces
   `GGML_OPENCL`/`GGML_WEBGPU` OFF. Z3 is handled in phase 6.

The phase-5 gate is enforced in C++ doctest
(`tests/cpp/test_deps.cpp`, case `deps: tracked .py files match the phase-5
allow-list`), which runs with every `prism_tests` invocation (including CI).
No separate Python job is required for this check. While the Python engine and
pytest suite still exist, `k_python_engine_deleted` in that test stays `false`:
tracked `.py` under `prism/`, `tests/`, `scripts/`, and `tools/` (plus all of
`third_party/`) are allowed. After phase 5 deletes those trees, flip
`k_python_engine_deleted` to `true`; the test then requires
`git ls-files '*.py'` outside `third_party/` to be exactly
`tools/svcomp/prism.py`. A tracked `.py` or `python` call in CI workflows
other than the BenchExec smoke (below) should still fail review.

## What we give up, and what replaces it

The Python engine is one of the five testing layers today: the "frozen
oracle" (roadmap D8). Every engine change is compared against an
independent implementation. Deleting it removes that comparison. Other
checks cover the same ground:

- `bmc.cpp` (hand encoder) against `pir` (Clang/LLVM): an independent C++
  encoder pair, already run by `tools/pir_vs_bmc.py` (becomes
  `prism-qa pir-vs-bmc`).
- Lean: the verdict lattice, bit-blaster and refinement certificates are
  checked by the Lean kernel, not by either engine.
- Compiled execution: random programs (`prism-qa soundness`) and
  counterexample replay compare verdicts with real runs.
- Golden files: before Python is deleted, freeze its report/SARIF output on
  `testdata/` as golden files that the C++ tests compare against.
  Differences found while freezing are decided one by one (phase 1).

## Phases

Each phase ends green (`prism_tests`, the pytest suite that remains, and
conformance with 0 wrong proofs) and is pushed on its own. Python is
deleted only in phase 5, after every test it carried has a C++ home.

### Phase 1: close the gaps where C++ is behind Python

The audit found places where the C++ engine, the primary engine, does
**less** than Python. Deleting Python first would lose behaviour. Each fix
lands with a doctest.

- **checkers** (`src/prism/checkers_*.cpp`): all 596 Python rule ids exist
  in C++, but branches and guards differ.
  - Missing "assigned then used unchecked" API branches: API-FORK,
    GETADDRINFO, KQUEUE, SHMGET/SEMGET/MSGGET, REALLOCARRAY/REALLOCF/VALLOC.
  - About 20 C++ lint branches missing: ADDRESSOF, AS-CONST, BIT-CEIL,
    CMP-LESS in_range, FORWARD-LIKE, CONST-ITERATOR, EMBED, GENERATOR
    return type, KILL-DEPENDENCY, QUICK-EXIT dtor, SET-TERMINATE, WEAK-PTR,
    UNREACHABLE.
  - About 20 guards missing, which adds false alarms. Examples:
    ERROR-CODE (34 C++-only hits in `src/`), THIS-THREAD, CONTRACTS,
    ENDIAN, and the flat_*/unordered container exclusions.
  - Dispatch: STR-STRNCPY-NUL and STR-SNPRINTF run on C++ files.
  - MEM-COPY-LEN: numeric sizes are rejected. Decided: superseded, C++
    stays identifiers only. A literal length (`memcpy(code, classbits, 32)`)
    is a constant, not an unchecked length, and the real-code audit (the
    zlib false alarms) asked for identifiers only. The frozen Python engine
    still accepts literals; no corpus file shows the difference. Locked by
    `tests/cpp/test_checkers_c.cpp` "MEM-COPY-LEN: identifier lengths only".
  - Wording drift in messages. 32 regex globals are declared for branches
    that were never ported.
  - Decide the intended behaviour for each difference: take the stricter
    guard unless it hides a real bug.
  - Decisions taken (the C++ engine differs from `prism/checkers.py` on
    purpose; each is locked in `tests/cpp/test_checkers_cxx.cpp`, so the
    golden-file freeze must not flag them):
    - CXX-SET-FIND, CXX-MULTIMAP-FIND, CXX-MULTISET-FIND and CXX-MAP-AT do
      not skip a function that also names a flat_*/unordered_*/multi*
      container. The declaration regexes are word-bounded, so a flagged
      find()/at() is on a real std::set/map; the Python skip only hid bugs.
    - CXX-VIRTUAL-IN-CTOR reads comment- and string-blanked text: a
      "virtual" in a comment or literal declares nothing (Python reads raw
      text and reported 28 such calls in `astlint_checks.cpp`).
    - CXX-SYSTEM-ERROR needs the word `system_error`: a
      `filesystem_error` catch is not one (Python gates on the substring).
      A mention inside a string or raw-string literal is blanked first and
      does not count (both engines agree on that).
    - MEM-COPY-LEN reports only a variable length: a literal length such as
      `memcpy(code, classbits, 32)` is fixed at compile time and is not the
      unchecked length this rule is about. A literal length larger than a
      local destination is reported by the warnings stage
      (`-Wfortify-source`: "'memcpy' will always overflow"; NOTRUN without
      a compiler).
    - CXX-UNINIT-MEMBER: a constructor-body `mem = x` initialises the
      member, `mem == x` does not (Python matches `mem\s*=` and so also
      accepts a comparison). Python keeps the old regex until phase 5.
    - STR-STRNCPY-NUL / STR-SNPRINTF: the regex rules in `checkers_core`
      run on C sources only, as in Python. The Clang-AST lint
      (`astlint_checks.cpp`) still reports STR-STRNCPY-NUL on C++ files:
      it sees the real local array and the missing NUL store, which is the
      same bug in C++.
- **cparse**: `split_params` rejoins qualified types with spaces, so
  CXX-VECTOR-INDEX misses `const std::vector<T>&`. A non-UTF-8 source can
  crash the JSON dump; decode with replacement.
- **ltl**: `extract_fsm` is not ported (a correctness gap in the primary
  engine). Done in C++, and deliberately stricter than `prism/ltl.py`: arms
  are split at bracket depth 0 only, an arm (or `default:`) that can leave
  without writing state keeps it, and a write the reader cannot name
  refuses the machine (NOTRUN). Python PROVES `G (s == IDLE -> X (s == RUN))`
  on a nested `switch (ev)` that C++ FAILS; do not freeze the Python verdict.
- **contracts**: ACSL/comment parsing is case-sensitive in C++ and
  caseless in Python. Add a caseless `prism::Regex` flag. Done (`(?i)`).
  Intentional divergence (D126): C++ encodes linear decreases measures, so
  `countdown_complex` (`n - i`) is FAILED in C++ and ERROR in Python.
- **rapid / muttest**: the gcc fallback is Python-only. Port it through the
  sandbox (Law 9), or drop it and correct `EXEC_STAGES`.
- **adapters_extra**: spatch drops rows and semgrep mishandles whitespace.
  Both are regressions in the C++ port.
- **fuzz**: `bytes_from_cex` overflow and `decode_args` narrowing. Also
  add the memo, the build cache and parallel runs. Intentional divergence:
  C++ `bytes_from_cex` decodes the solver's `#x` / `#b` literals, so
  `seeds_from_bmc` yields seeds where Python yields none; golden files
  must pin the C++ seeds.
- **afl / sandbox**: one process runner with environment overrides. The
  AFL environment is currently passed with `setenv`.
- **pbsd**: the ParanoidBSD verify-script import path is Python by nature.
  Record that it is dropped; C++ checkers cover the rules.
- **ai / agent**: add `PRISM_N_GPU_LAYERS` (the llama-cpp-python path's GPU
  layers) to `NativeLlama::load`, and the agent helpers the audit lists.
- **GUI** (`src/gui/`, Qt 6): it is not a superset of the PySide6 GUI. The
  stage table with live status and the other rows in appendix A go in, with
  `tests/cpp/test_gui.cpp` (offscreen, `PRISM_QT=ON`).
- **CLI**: move argument parsing into a testable
  `parse_cli(std::span<const char*>)` in `prism_core` (`--flag=value`
  forms, the flags only Python accepted).
- **polyglot**: the C++ stage still shells out to `python3` for JSON/TOML
  syntax (`SYNTAX_HELPER`, `polyglot.cpp:78`). Replace it with
  `nlohmann::json::parse` and a C++ TOML reader.

**Intentional C++-only divergences.** Phase 1 fixes gaps in C++ only
(`prism/*.py` is frozen until phase 5 deletes it), so these behaviours differ
from the Python engine on purpose. A parity test that trips on them should
lock the C++ behaviour, not restore the old one:

- afl-fuzz is found through `Config::which_adapter` (`--tool`, then the
  pinned `aflplusplus` build, then PATH; D60). `prism/afl.py afl_available`
  still looks on PATH only.
- `bitwuzla` has a `vendor_dir_for` entry in `src/prism/config.cpp` (D61) but
  no `VENDOR_DIR` entry in `prism/config.py`; `tests/cpp/test_deps.cpp`
  allows exactly that row.
- `solver::find_tool` takes `--tool`, then the pinned build under
  `PRISM_TOOLS_DIR` (never another commit), then PATH.
- `binary=compile-failed` from `fuzz_function` is kept on the `fuse` row, so a
  CLEAN that only the concrete oracle produced says so (Law 7).
- The process runner reports an argv[0] it cannot execute as "could not
  start" (`failed`), refuses `--no-*-check` for every caller (Law 8), and the
  AFL environment is an overlay on the child, not `setenv` on PRISM.

### Phase 2: port the tests

96 files: 44 test Python-engine behaviour, 34 are mixed, 10 read C++
sources, 8 test tools, 2 drive the C++ binary and 1 is a repo lint. The
destination of each file is in appendix C.

- Behaviour tests become table-driven doctest files. Examples:
  `test_bmc.cpp`, `test_concolic.cpp` (164 cases), `test_concrete.cpp`,
  `test_fuse.cpp`, `test_adapters.cpp`, `test_lint_corpus.cpp` (every
  `testdata/` plant with its expected rule and status), `test_thread.cpp`
  (C++ has no thread coverage today), `test_ltl.cpp` and `test_journal.cpp`.
- Tests that read C++ sources to lock parity are deleted with the Python
  engine: there is nothing left to be in parity with. Any rule they carried
  that is not about parity becomes a doctest.
- Cross-engine SARIF comparisons become golden-file checks against the
  frozen output.
- Repo lints (`test_naming.py`: no retired project name; no tracked file over 10 MB)
  become a doctest over `git ls-files`.

Python-engine gaps left by ported tests (the Python engine is frozen and
goes in phase 5, so these are recorded, not fixed there):

- `prism/journal.py` `read_functions` skips a malformed `functions.json`
  entry and returns the rest, so `--resume` skips classify and never
  analyses the dropped function. C++ returns no functions and a note
  (`functions.json: N malformed entries; classify rerun`) and reruns classify.
- `prism/contracts.py` and `prism/wp.py` match returns with
  `\breturn\s+([^;]+);`, which misses `return(x);` and `return;`. Those
  paths get no ensures assert, so a contract they break is PROVED-ASSUMING.
  C++ matches every return, and an empty return in a value-returning
  function is ERROR (doctests "wp and contracts: `return(expr);` is
  checked" and "contracts: an empty return ... is ERROR").
- `tests/test_verdict.py` was deleted; nothing now checks `prism/laws.py`
  against `proofs/verdict_tables.json` or against the C++ vocabulary. The
  doctests check `include/prism/laws.hpp` only.
- `report.md` writes a confidence scope with no data as `**0**` in C++ and
  `**0.0**` in Python. The doctest locks the C++ form.

### Phase 3: port the tools

| today | C++23 home |
|---|---|
| `scripts/fetch_deps.py`, `sbom.py`, `licence_check.py` | `prism-deps` (std-only, no Z3): `list`, `linked`, `tool NAME`, `tree-digest`, `sbom`, `licence-check` |
| `tools/conformance.py` (release gate), `csmith_soundness.py` | `prism conformance [--self-check\|--certified\|--suite\|--fetch-juliet]`, `prism conformance random` |
| `tools/svcomp/prism_svcomp.py`, `witness.py`, `run_subset.py`, `package_archive.py` | `prism svcomp TASK --prop P` (witness 2.0 via `ordered_json` + a block-YAML writer), `prism svcomp score`, archive via CPack |
| `pir_vs_bmc`, `pir_lean_check`, `llvm_sem_vs_lli`, `solver_bench`, `libc_model_bounds`, `triage_selfscan` | `prism-qa <cmd>` (dev binary, not shipped) |
| `tools/proctree.py` | `run_session()` next to `detail::run_process` |
| `tools/prism_prove.py` | folded into `prism prove` |
| `tools/assurance_check.py`, docs checks | `prism-qa assurance-check`, `docs-check` |
| `tools/prism_ai/*` (GBDT trainer, scheduler replay, predict, measure) | trainer in `src/prism/solver/gbdt.cpp` next to the evaluator; `prism_ai` offline tool; model stored as `predict_default.json`, turned into `.inc` by CMake |
| `tools/gen_ai_grammars.py`, `gen_astlint_discard.py` | CMake configure-time generation / one constexpr table |
| `tools/gen_prism.py`, `gen_cxx.cpp.py`, `patch_cxx.py`, `fuzz_self/fuzz_py.py`, `scripts/python_plan_done.py`, `exclude_large_files.py`, `smoke_core.*` | deleted (their inputs are the Python engine, or they are obsolete) |
| `tools/fuzz_self/make_corpus.py`, `run_cpp.sh` | `prism_fuzz_corpus` + CMake target `fuzz-self-run` |
| `proofs/**/check.sh` Python snippets | `lean_audit scan/axioms` (std-only) |
| `tests/cpp/test_certified.cpp` Python memory hog | `prism_test_hog` helper executable |
| `prism/cocci/*.cocci` | move to `rules/cocci/`, installed next to the binary |

### Phase 4: CI, Docker, docs

- `ci.yml`: delete the `python` job; its coverage is now in `prism_tests`.
  The supply-chain job builds only `prism-deps`. The SV-COMP packager uses
  CPack.
- `conformance.yml`, `self-check.yml`, `release.yml`, `proofs*.yml`,
  `docs.yml` and the `Dockerfile` call the C++ commands. No
  `setup-python`, no `pip`.
- Rewrite `README.md`, `docs/USER_GUIDE.md` and `docs/assurance/*`: every
  cited `tests/test_*.py` points at its doctest case, and every
  `tools/*.py` at its C++ command.

### Phase 5: delete Python

- Remove `prism/`, `tests/**/*.py`, `pyproject.toml`, `requirements.txt`,
  the Python ignores, the `prism_native` C ABI library (its only user was
  `prism/simdmut.py`) and every Python `__pycache__`.
- `CLAUDE.md`: "one product, two engines" becomes one engine. Laws and
  "Adding a check" point at C++ files only. The parity rules are removed.
- Flip `k_python_engine_deleted` to `true` in
  `tests/cpp/test_deps.cpp` (see the gate description at the top of this
  document).

### Phase 6: build without Python (vendored Z3)

Z3's own CMake requires Python 3 and generates 41 files (API logging
macros, parameter and tactic registration, and others). `third_party/` is
never edited, so PRISM adds `cmake/z3.cmake`, its own build description of
the pinned Z3 sources. The generated files come from one of two places,
chosen when this phase starts:

- (a) checked in under `cmake/z3-generated/` with the Z3 tree digest they
  were made from. CMake fails if the digest changes.
- (b) a small C++ generator that reproduces them.

Until this phase lands, the build still needs a Python interpreter for Z3
alone. That is recorded here, not hidden.

## Done when

- `git ls-files '*.py'` lists only `tools/svcomp/prism.py`.
- A clean Ubuntu runner with no Python installed builds PRISM and passes
  `prism_tests`, `prism conformance` (0 wrong proofs, counts no lower than
  `docs/ROADMAP_STATUS.md`), the SV-COMP subset (0 incorrect, score ≥ 203)
  and the Lean rechecks.

## Appendix: per-file map (audit of `911b9f94f`)

Sizes: S < 1 day, M 1–3 days, L about a week, XL more. The "gaps" column
lists the first items only; each module has a longer list in the audit
record.

### A. Python engine modules

| module | C++ owner | status | size | C++ gaps to close first |
|---|---|---|---|---|
| `prism/__init__.py` | `include/prism/pipeline.hpp (PRISM_VERSION)`, `include/prism/models.hpp`, `src/prism/capi.cpp` | ported | S | There is no single source for the version. PRISM_VERSION = "0.1.0" is hard-coded in pipeline.hpp:35 and mirrors prism/__init__.py __version__. pyproject.toml …; The re-exported API (Finding, FunctionInfo, StageResult, RunReport, Pipeline, run_pipeline) maps to models.hpp and run_pipeline. C++ has no Pipeline class; … |
| `prism/__main__.py` | `src/prism/main.cpp`, `include/prism/pipeline.hpp (PRISM_VERSION, STAGE_ORDER, exit_code)` | ported | S | The flag sets match. Python flags (path, --out, --no-llm, --gui, --stage, --skip, --unwind, --jobs/-j, --fuzz-budget, --fuzz-iters, --repair-rounds, --resume, …; --flag=value is not supported. The C++ loop compares whole argv tokens, so `--fail-on=bogus` (and so `--fail-on=defect`) is silently ignored with exit 0. I …; Unknown flags are silently ignored (`--bogus-flag` runs). Python exits 2 with 'unrecognized arguments'.; A missing flag value is silently empty, because next() returns an empty string. `prism --stage` with no value runs the pipeline with every stage 'skipped' and … (+8 more) |
| `prism/adapters.py` | `src/prism/adapters.cpp (run_cppcheck/run_cppcheck_unstamped, run_esbmc/_unstamped, run_dafny/_unstamped, run_compiler, cc_name, norm_diag, rel_to_root, compiler_key, tool_unusable, notrun, refuse_disabled_checks)`, `src/prism/config.cpp (Config::which_adapter, stamp_tool_sha, adapter_install)` | ported | S | Nothing functional is missing. adapters.py:216 run_pbsd(cfg, scope) has no callers (the pipeline calls pbsd.run_pbsd_lints directly), so it is dead code and …; No C++ tests cover how run_esbmc and run_dafny map tool output to a verdict: VERIFICATION SUCCESSFUL gives PROVED, PROVED_UNBOUNDED with INDUCTION plus …; Install hints still point at Python. adapter_install() in config.cpp returns "python scripts/fetch_deps.py --tool X (pinned in third_party/MANIFEST.toml)", so …; Small message drift. When cppcheck cannot start, Python says "cppcheck unusable: {exc}" but C++ (r.failed) says "cppcheck at PATH is not cppcheck (not a … |
| `prism/adapters_extra.py` | `src/prism/adapters.cpp (run_optional_tools, OptionalTool table, probe_exe, probe_looks_missing, is_fake_adapter, frama_c_probe_present, help_ok, dispatch_optional, run_clang_tidy, run_cbmc, run_spatch, cocci_rules, cocci_has_script, parse_spatch_hits, run_semgrep, parse_semgrep, extract_json_object, run_infer, run_frama_c, run_klee, run_strix, strix_specs, libfuzzer_probe)`, `src/prism/stages/fuse.cpp (run_libfuzzer, libfuzzer_harness_source, compile_libfuzzer, libfuzzer_flag_rejected, kLibfuzzerInstall)` | ported | S | BUG in run_spatch (adapters.cpp, end of the function): the C++ version drops rows it should keep. Without --allow-exec, .cocci rules with …; BUG in extract_json_object: Python returns "{}" when the output is only whitespace (blob.strip() is empty), which gives UNKNOWN "semgrep ran; no matches". C++ …; The bundled Coccinelle rules are stored inside the Python package directory, prism/cocci/. There are 8: getenv_null, memcpy_self, realloc_self, shift_bit31, …; run_libfuzzer rows lack extra["sandbox"] (Python sets extra={engine, exe, sandbox}). When clang is missing, libfuzzer_probe says "clang not found (config, … (+2 more) |
| `prism/afl.py` | `src/prism/stages/fuse.cpp (afl_fuzz_which, afl_harness_source, compile_afl_harness, run_afl_fuzz, env_setdefault; called from fuse_one when PRISM_AFL=1)` | ported | S | The environment is set the wrong way. Python copies os.environ, adds AFL_NO_UI, AFL_SKIP_CPUFREQ and AFL_NO_AFFINITY to the copy, and passes it to run_binary. …; The AFL run has no working directory set. Python uses cwd=work; C++ run_argv gets no cwd.; AFL CLEAN, CRASH and ERROR rows lack extra["sandbox"]. Python sets extra={"engine":"afl","sandbox":sandbox_kind()}.; Both engines look for afl-fuzz on PATH only (afl_available / Config{}.which({"afl-fuzz","afl-fuzz.exe"})). The optional stage finds the pinned … (+1 more) |
| `prism/agent.py` | `src/prism/stages/hypothesize.cpp (hypothesize)`, `src/prism/stages/fuse.cpp (fuzz4all_seeds, fuzz4all_autoprompt_text, fuzz4all_mutate_interesting, fuzz4all_combine, chatfuzz_mutants)`, `src/prism/stages/execute_cex.cpp (sandbox_verdict, interpreter_loop, execute_cex)` | partial | M | `dafny_specs` is not ported. Python's contracts stage appends dafny_specs(engine, SCALAR/VOID functions, budget 3) when cfg.llm (pipeline.py:383-390). Those …; interpreter_loop findings carry fewer extras in C++. The CLEAN finding lacks extra.sandbox, and the CRASH finding lacks extra.code and extra.sandbox …; Output truncation keeps the start instead of the end. C++ uses substr(0,400) for interpreter stdout/stderr and substr(0,2000) in sandbox_run; Python keeps the …; sandbox_run drops partial output on run timeout in C++. Python returns ex.stdout/ex.stderr from TimeoutExpired. (+7 more) |
| `prism/ai.py` | `src/prism/stages/llm.hpp`, `src/prism/stages/llm.cpp`, `src/prism/stages/fuse.cpp (lines ~1126-1320: Fuzz4All prompts/helpers)` | partial | S | SYSTEM_DAFNY prompt constant has no C++ copy. It is only used by agent.dafny_specs, which was also not ported.; The llama-cpp-python backend knobs have no C++ equivalent. `_llama_n_gpu_layers()` reads the PRISM_N_GPU_LAYERS env var (default -1 = all layers on GPU) and …; The native llama.cpp path has smaller limits than Python. C++ uses n_ctx = 2048 (llm.cpp:111) and kMaxNew = 256 generated tokens (llm.cpp:160). Python uses …; The native backend accepts any GGUF that exists. Python `_try_llama_cpp_bindings` requires the GGUF to be larger than 1 MB; C++ bind() only checks … (+6 more) |
| `prism/bmc.py` | `/home/user/prism/src/prism/bmc.cpp`, `/home/user/prism/src/prism/bmc_encoder.inc`, `/home/user/prism/src/prism/bmc_unenc.inc` | ported | M | No encoder feature, flag or verdict path exists only in Python. The C++ side is a superset: k_induction_strengthened (Houdini, extra.invariants and friends), …; Public API gap. Python exports bmc_function(fn, unwind, try_unbounded, enums, allow_local_pointers, incremental), k_induction, extract_enums, extract_macros …; The single source of bmc_unenc.inc is broken. tools/gen_prism.py gen_unenc() lifts gates from prism/bmc.py but only recognises `re.search`, and bmc.py now …; bmc_unenc.inc is #included as text into 4 TUs (bmc.cpp, stages/fuse.cpp:34, stages/concolic.cpp:9, interval.cpp:1108), so the 575 regex gates are compiled 4 … (+6 more) |
| `prism/checkers.py` | `src/prism/checkers_dispatch.cpp`, `src/prism/checkers_core.cpp`, `src/prism/checkers_api.cpp` | partial | L | RULE-ID DIFF: none missing. Python emits 596 rule ids (595 dashed plus INTENT). All 596 are emitted, as non-comment literals, in checkers_*.cpp. C++ has 8 …; API checks: the 'assigned then used unchecked' branch is not ported. C++ has only lint_discarded (the discarded-return branch) for API-FORK (_FORK_ASSIGN + …; Dispatch gating: Python runs _str_strncpy_nul and _str_snprintf only when the suffix is not .cpp/.cc/.cxx/.ii. checkers_core.cpp calls str_strncpy_nul and …; MEM-COPY-LEN: Python accepts a size matching `^\w+$`, which includes numeric literals such as memcpy(code, classbits, 32). C++ mem_copy_len requires … (+11 more) |
| `prism/concolic.py` | `src/prism/stages/concolic.cpp`, `src/prism/bmc.cpp solve_fork_flip (Z3 branch flip)`, `src/prism/stages/interp.cpp eval_cond` | ported | S | extra.args and extra.tried are missing on the ERROR and 'skip-pointer' NEEDS-HARNESS rows. Python attaches {args, tried}; C++ attaches only tried, or nothing …; Seed and flip values are 32-bit only in both engines (i32). This is not a gap, but it inherits the concrete.py int Args limitation.; The gate helpers has_self_call, has_unencoded_cxx, has_unencoded_float, has_unencoded_throw and has_unencoded_setjmp are local copies in concolic.cpp rather … |
| `prism/concrete.py` | `src/prism/stages/interp.cpp`, `src/prism/stages/interp.hpp`, `src/prism/stages/common.cpp (decode_args, interesting_seeds, param_nbytes)` | partial | S | 64-bit argument values: C++ St ctor and execute() take std::map<std::string,int> args, and common.cpp decode_args does `args[name] = sz >= 8 ? …; pack_args(fn, args) (inverse of decode_args): no C++ equivalent. Only tests/test_concrete.py uses it; port it as a test helper.; lru_cache memoisation of _brace/_paren/_stmt/_tok/_assign_plan/_stmt_kind/_parse_expr_pure: C++ re-lexes on every loop iteration. This affects speed only, not …; ExecResult.steps and MAX_STEPS=10_000 match. C++ adds float_unencoded() and eval_cond(), which Python keeps in concolic._eval_cond. |
| `prism/confidence.py` | `src/prism/pipeline.cpp (apply_confidence)`, `src/prism/verdict/verdict.cpp (score_counts)` | ported | S | There is no pure score(report) that returns (v,a,r,c) without mutating. Python tests use confidence.score (test_confidence.py, test_taxonomy.py, …; _resolves differs. Python needs a truthy extra['oracle'] or extra['read']. C++ uses extra.contains('oracle'), so an empty-string value counts as resolving.; Rounding differs. Python round(x,4) rounds half to even on the binary value; C++ uses std::round(x*1e4)/1e4, which rounds half away from zero, so a … |
| `prism/config.py` | `src/prism/config.cpp`, `include/prism/config.hpp`, `include/prism/threads.hpp (parallel_for/clamp_jobs)` | ported | S | Every install hint names Python. adapter_install() returns "python scripts/fetch_deps.py --tool <comp> (pinned in third_party/MANIFEST.toml)" …; load_manifest, repo_root and pinned_commit read MANIFEST.toml at runtime with tomllib. C++ bakes the pins in at configure time through a CMake regex parser …; Config::which uses fs::exists on each PATH entry, so a non-executable file or a directory of the right name counts as found. Python shutil.which requires …; which_adapter does not expanduser the `--tool` path (Python Path(expl).expanduser()). (+3 more) |
| `prism/contracts.py` | `src/prism/stages/contracts.cpp (prove_contracts, bmc_with_assume, prove_with_contract)`, `src/prism/stages/common.cpp (parse_comments, parse_acsl_body, acsl_preamble, extract_acsl_blocks, locate_source)` | partial | S | Case-insensitive clause parsing is missing. Python _COMMENT_CLAUSE and _ACSL_KEYWORD use re.I. C++ Regex (PCRE2 with UTF\|UCP only, no CASELESS) has no (?i) …; Decreases baseline differs: Python calls bmc_function_with_assume(..., decreases=None), so invariant defaults to None. C++ calls bmc_with_assume(fn, unwind, …; _SOURCE_CACHE (mtime/size-keyed source cache, max 64 entries): C++ read_fn_source re-reads the file on every call. This affects speed only.; extra values are strings in C++ ("true"/"false", "" for None). Python stores bool/None. See the report-shape note. |
| `prism/cparse.py` | `/home/user/prism/src/prism/cparse.cpp`, `/home/user/prism/include/prism/cparse.hpp` | ported | S | There is no text-level entry point for Python's extract_functions_from_text(text, rel, stripped=None). C++ parse_text is in the anonymous namespace and only …; Input decoding. Python reads with read_text(encoding='utf-8', errors='replace'), so bodies are always valid UTF-8. C++ read_file keeps the raw bytes, so a …; split_params. Python computes typ = raw[:m.start()] using offsets taken from the '*'-spaced copy, which is a bug: `int *out` gives typ 'int *ou', and `char …; kind_of: Python removes 'const'/'volatile' as substrings (t.replace) before the SCALAR_WORDS check; C++ only maps tabs to spaces. This is an edge case for … (+2 more) |
| `prism/cparse.py (params)` | `src/prism/cparse.cpp`, `include/prism/cparse.hpp` | partial | S | split_params in src/prism/cparse.cpp:152 differs from _split_params in prism/cparse.py:351. When a param has const/volatile/restrict/register, C++ … |
| `prism/diff.py` | `src/prism/stages/diff.cpp` | ported | none | extra.sandbox = sandbox_kind() on the FAILED (disagree) finding is missing in C++.; Evidence slice differs: Python keeps the last 800 chars of stderr; C++ rr.err.substr(0, 800) keeps the first 800.; C_TYPE_SIZE key normalisation: Python uses " ".join(typ.split()), collapsing internal whitespace; C++ c_type_nbytes_key only strips the ends.; The compile step runs outside the sandbox in both engines (only the harness binary is wrapped), so this is not a gap. |
| `prism/fuse.py` | `src/prism/stages/fuse.cpp (run_fuse, fuse_one, branch_goals, numbered_goals, seeds_from_bmc, fuzz_function, run_afl_fuzz, run_libfuzzer, fuzz4all_*/chatfuzz_mutants)` | ported | none | When the source file cannot be found, Python _read_source(fn, root) falls back to `signature + body`. C++ uses read_fn_source(fn), which returns "", so …; run_fuse signature: Python accepts any engine object (duck-typed `available`); C++ takes `bool llm` and builds LlamaEngine(Config{}) itself. This is an API … |
| `prism/fuzz.py` | `src/prism/stages/fuse.cpp (fuzz_function, bytes_from_cex; afl_harness_source is the stdin harness, which Python calls harness_source; compile_afl_harness is Python's _compile)`, `src/prism/stages/common.cpp (param_nbytes, kCTypeSize, decode_args, interesting_seeds)`, `src/prism/stages/interp.cpp (execute)` | ported | M | No build cache. Python keeps _EXE_CACHE, keyed by compiler, PATH, source name, function name, harness text and source text, so the second FuSeBMC round reuses …; No parallel binary runs. Python's _binary_run runs the harness min(4, cpu_count) at a time through a ThreadPoolExecutor while keeping results in input order. …; No memo for unencoded_syntax_reason. Python keeps _SYNTAX_MEMO, keyed by file, signature, body and a sha1 of fn.file, with an engine sentinel that is …; BUG in bytes_from_cex: C++ uses std::stoi(v, nullptr, 0). Any value above INT_MAX, such as "x=4294967295" or "x=0xFFFFFFFF" (common in unsigned CBMC/BMC … (+3 more) |
| `prism/gui.py` | `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`, `src/gui/main.cpp` | partial | M | There is no stages table. Python has a QTableWidget with columns stage/status/records/seconds/note, filled from report.stages and live from the journal. The …; There is no live progress polling. Python's QTimer (400 ms, gui.py:357) runs journal.read_stages(out) (_poll_journal) and logs '<stage> <status> (<records>)' …; There is no 'Resume last report' checkbox (Python resume_ck sets Config.resume). C++ supports it only as a --resume CLI passthrough.; There is no 'optional' skip checkbox. Python skip_from_checks(fuzz, repair, optional) has three boxes; C++ has skip fuzz and skip repair, with optional … (+9 more) |
| `prism/harness.py` | `src/prism/stages/harness_bmc.cpp`, `src/prism/ai/harness.cpp (ai::drafted_harness_bmc, C++-only superset)` | ported | none | No functional gap. pointee_type strips const/volatile/restrict only as whole whitespace-separated words, where Python uses a \b regex. They are equivalent …; The spec comes from parse_comments, so harness inherits the case-sensitivity gap listed under contracts.py. |
| `prism/inline.py` | `/home/user/prism/src/prism/inline.cpp`, `/home/user/prism/include/prism/stages.hpp` | ported | S | Nothing functional is missing. _KW, _MODELLED_CALLS, _DECL_TYPE, _callee_ret_type, _inlineable_callee (static, reach_error/__VERIFIER_error excluded, …; Python-only tests to port: tests/test_inline.py (9). |
| `prism/interval.py` | `src/prism/interval.cpp`, `src/prism/bmc_unenc.inc (unencoded_syntax_reason, #included)` | ported | S | Edge case: in `int x =` with an empty initialiser, Python _decl evaluates "" (which _interval_tok maps to "0") and gets R(0,0). C++ decl() sees init.empty() …; Literal overflow: C++ std::stoll throws ParseFail("nud ...") on hex literals above INT64_MAX. Python int() accepts them before the `n > INT_MAX` wrap.; No functional gap otherwise. Every ParseFail reason (switch, VLA unencoded, array decl, unmodelled declaration, do without while, 64-bit literal unencoded, … |
| `prism/journal.py` | `src/prism/journal.cpp`, `include/prism/journal.hpp` | ported | S | journal_read_functions stops at the first malformed element and returns an empty list, because one try wraps the whole loop. Python skips non-dict and bad …; progress.json formatting differs: C++ pretty-prints (dump(2)), Python writes compact JSON. The GUI tails this file.; journal_append_stage has dead code: the `reportish` and `extra_findings` locals are unused, and it serializes a whole RunReport just to extract one stage.; The STAGES_JSONL, PROGRESS_JSON and FUNCTIONS_JSON constants are hard-coded strings in C++. |
| `prism/laws.py` | `include/prism/laws.hpp`, `src/prism/laws.cpp`, `include/prism/verdict.hpp` | ported | none | The named Python sets FORMAL_VERDICTS, FUZZ_VERDICTS and LLM_VERDICTS have no C++ equivalent. They are used only by Python tests (test_cuda.py:158, …; refuse_merge with strings outside the vocabulary: C++ returns early, while Python can still raise REFUSE_NOTRUN_CLEAN if one side is NOTRUN. This edge case is … |
| `prism/ltl.py` | `src/prism/stages/ltl.cpp` | partial | M | extract_fsm is the old algorithm. C++ scans `case X:` and `state = X` over the whole body and requires cases.size() >= 2.; Missing _SWITCH_STATE / _switch_state_bodies: C++ does not limit extraction to the brace-matched body of `switch (state)` / `switch (p->state)` / `switch …; Missing _STATE_ASG parenthesised destinations: `state = (DEST)` becomes a self-loop in C++.; Missing _ARM_LAB default handling: C++ has no `default:` arm destinations for unmatched states, and a function with one named case plus default is not … (+6 more) |
| `prism/models.py` | `/home/user/prism/include/prism/models.hpp`, `/home/user/prism/src/prism/models.cpp`, `/home/user/prism/src/prism/journal.cpp` | partial | S | Finding.extra is dict[str, Any] in Python (JSON bool/int/list/dict) and std::map<std::string, std::string> in C++. Every value is written as a JSON string, …; Serialisation robustness. Python json.dumps (ensure_ascii) cannot fail. C++ RunReport::dumps() uses j.dump(2) with the strict UTF-8 handler, so it throws and …; Load tolerance. Python from_dict coerces missing or null fields (float(x or 0.0), int(records or len(findings)), bool(static)) and accepts "line":"3". C++ …; Object-level API. Python has RunReport.from_dict/to_dict, finding_from_dict, stage_from_dict, Finding.to_dict, StageResult.to_dict and FunctionInfo.to_dict. … (+3 more) |
| `prism/muttest.py` | `src/prism/stages/rapid.cpp run_muttest, iter_mutations` | partial | S | _held_by_law9: C++ has no NOTRUN with install/reason/exec=NOTRUN when a mutant evaluation is held back by --allow-exec, because the C++ run_plan has no gcc …; Scoring uses only the concrete interpreter, so a mutant body interp.cpp cannot parse gives ERROR 'cannot score mutant' in C++. Python would fall back to gcc …; Evidence: C++ mutated.substr(0,400), Python mutated.strip()[:400].; The C++ compiler_missing lambda reports NOTRUN 'install gcc or clang' when gcc is absent from PATH, although C++ never uses gcc here. |
| `prism/pbsd.py` | `src/prism/adapters.cpp (run_pbsd_lints, pbsd_looked_root, pbsd_tree_present, pbsd_c_paths, pbsd_dedupe, pbsd_not_run, prism_portable, pbsd_heavy_notrun, pbsd_ported_json, pbsd_invoked_json, kPbsdVerifyScanners, kPbsdPortableCls, kPbsdTreeCls, kPbsdHeavy)`, `src/prism/checkers_core.cpp (the C++ lints that produce the restaged classes)` | partial | M | Python imports and runs the ParanoidBSD tree's own Python scanners: _load_verify plus _RUNNERS for realloc_self.scan, onesided_index.scan, …; Classes and fields the imported scanners emit that C++ does not: onesided_index results of any kind other than ONESIDED become MEM-OOB-READ, which C++ never …; extra.invoked and extra.ported mean different things. In Python, invoked lists module names that imported and ran, and ported lists modules that failed to …; Once no code from the tree executes, the C++ --allow-exec gate on pbsd protects nothing: without the flag it returns sandbox::exec_notrun("pbsd", "pbsd … (+2 more) |
| `prism/pipeline.py` | `src/prism/pipeline.cpp`, `include/prism/pipeline.hpp` | ported | M | The contracts stage has no LLM Dafny-spec hypotheses. Python contracts() calls agent.dafny_specs(engine, SCALAR/VOID functions, budget 3), which uses the …; Report shape: typed extra values. Finding.extra is map<string,string> in C++, so unify writes `gaps` as a CSV string where Python writes a JSON list, classify …; The inventory/classify parse cache is missing. Python keeps a `parsed` dict so every source is parsed once. C++ calls extract_functions twice per file, once …; The ltl spec walk uses std::filesystem::recursive_directory_iterator(src_root) with no error_code and no skip_permission_denied. An unreadable subdirectory … (+4 more) |
| `prism/polyglot.py` | `src/prism/polyglot.cpp (checks() table, kLangExts, kCFamilyExts, kTextOnlyExts, kBuiltinScans, SYNTAX_HELPER, walk_files, is_known_source, is_text_file, iter_polyglot_sources, iter_text_files, builtin_scan, resolve, expand, parse_output, unexplained_output, invocations, HelperFile, run_check, run_polyglot)`, `src/prism/scope.cpp (scope::skip_dir)` | partial | M | C++ still runs Python at scan time. The python-syntax check (tool "prism-syntax", polyglot.cpp:149-154) writes the embedded Python SYNTAX_HELPER …; To port: check JSON syntax natively with nlohmann::json::parse (use parse_error::byte to get line and column; strip the utf-8 BOM the way the helper's …; Table parity is locked only by Python: tests/test_polyglot.py (37 tests) pins CHECKS, BUILTIN_SCANS, LANG_EXTS, the SYNTAX_HELPER text, benign and executes. …; C++ resolve() (polyglot.cpp:425) does not expand ~ in cfg.tools and does not check the pinned ~/.prism/tools build. Python resolve_adapter does both. No … |
| `prism/rapid.py` | `src/prism/stages/rapid.cpp (run_rapid, plan_trials, run_plan, finding_from_plan, shrink_counterexample, contract_kind_finding)` | partial | M | No gcc/clang fallback harness: _execute_gcc and _harness_source are missing. They build a scanf/printf main around the function body, compile with `-O0 …; No Law 9 gate on that fallback. _eval_status maps sandbox.EXEC_FLAG in the error to NOTRUN with install=EXEC_INSTALL, reason=EXEC_REASON, exec=NOTRUN. C++ …; No tiny-interpreter last resort: _interpret / _exec_block / _parse_stmt / _LOCAL_DECL / _LOCAL_ASSIGN / _eval_c_expr (Python-ast evaluator). When interp.cpp …; The 'cannot evaluate {fn}: ...' message prefix is not applied to interpreter errors in C++, which returns the raw ParseFail text. (+3 more) |
| `prism/sandbox.py` | `src/prism/sandbox.cpp, include/prism/sandbox.hpp (exec_message, exec_notrun, allowed, set_allowed, Policy RAII, kind, bwrap_argv, wrap_argv, Limits, limits_for, apply_child_limits, LIMIT_AS_BYTES/LIMIT_NOFILE/LIMIT_FSIZE_BYTES, quote_windows_arg, windows_command_line, is_batch_file, batch_command_line)`, `src/prism/adapters.cpp run_argv, src/prism/stages/common.cpp run_argv, src/prism/proc.hpp detail::run_process (the process runners)` | ported | S | There is no single equivalent of run_binary. Python has one API: run_binary(argv, scratch, timeout, input, text, cwd, env, limit_as, merge_stderr). C++ has …; sandbox.stamp(findings, kind) has no C++ counterpart, but nothing in Python calls it either, so it is obsolete.; Python exec_notrun takes file, function, line and strength keyword arguments; C++ callers set f.file, f.function and f.line afterwards. This works but is …; Tests: test_exec_safety.py (23) and the sandbox parts of test_afl, test_diff, test_rapid, test_muttest and test_execute_compile need doctest versions. C++ has … |
| `prism/sanitize.py` | `src/prism/adapters.cpp (run_sanitize, probe_sanitizer, is_mingw, dumpmachine, has_sanitizer_lib, sanitizer_lib_names, compile_and_run_san, run_sanitizer_on_paths, sanitizer_hit, sanitizer_runtime_unusable, wrapper_main, no_opt_in_message, kOptInHint, marked_run, opted_in_callable, has_run_marker, run_word, comment_line)`, `include/prism/stages.hpp (marked_run, opted_in_callable)` | ported | S | No parallelism. Python run_sanitize sends every (sanitizer, file) compile-and-run through ordered_map on cfg.jobs threads and keeps the serial output order. …; Files are parsed three times. Python builds the `callables` cache once, so each .c is parsed once for ASan, UBSan and TSan together. C++ calls …; Python's _run_sanitizer_on_paths helper is not called by run_sanitize and is obsolete.; Tests: test_sanitize.py (21), test_prism_sanitize.py (7) and the sanitize parts of test_exec_safety.py and test_py_parallel_order.py need doctest versions. … |
| `prism/sarif.py` | `src/prism/sarif.cpp`, `include/prism/pipeline.hpp (to_sarif, write_sarif, exit_code)` | ported | S | There is nothing functional to port. The named constants SARIF_SCHEMA, INFO_URI, FAIL_ON, DEFECT_STATUSES, GAP_STATUSES and NON_BLOCKING_SEVERITIES are inline … |
| `prism/scope.py` | `src/prism/scope.cpp`, `include/prism/scope.hpp` | ported | none | _count_sources counts symlinked files through os.walk filenames. C++ count_sources skips every symlink, so the counts in the 'skipped X/ (N source files)' …; skipped_path in C++ tests rel.native().starts_with(".."), which also treats a real child directory named '..foo' as outside the root. |
| `prism/shipdocs.py` | `src/prism/shipdocs.cpp`, `include/prism/shipdocs.hpp`, `CMakeLists.txt (docs/*.md -> generated prism/shipped_docs.inc)` | ported | none | The _MISSING fallback text for a missing docs file is not needed. C++ embeds docs/TRUSTED_BASE.md and VERDICTS.md at configure time, and a missing file is a … |
| `prism/simdmut.py` | `src/prism/havoc.cpp (havoc: splitmix64, INTERESTING_8/16/32 overlays, CUDA prism_cuda_havoc with CPU fallback)`, `src/prism/hash.cpp (coverage_hash, xsimd lanes)`, `include/prism/simd.hpp` | obsolete | none | Nothing functional: the C++ code is the reference, and Python's _py_hash and havoc are fallbacks of the same algorithm.; Deleting the module leaves this to clean up: src/prism/capi.cpp (and its entry in prism_core's source list, CMakeLists.txt:150); add_library(prism_native …; tests/test_simdmut.py (23) and the ctypes parts of tests/test_cuda.py need replacing. Only the golden values for the havoc overlays and coverage_hash are … |
| `prism/taint.py` | `src/prism/stages/taint.cpp` | ported | none | No functional gap. One nuance: the C++ take() strips leading `&` and `*` from the tainted argument of every source; Python strips `*` only for scanf. C++ is … |
| `prism/taxonomy.py` | `src/prism/taxonomy.cpp`, `include/prism/taxonomy.hpp` | ported | S | coverage_row(available) is a capability map used only in a test assertion that it is not used (test_gui.py:428). It is obsolete.; The taxonomy.json shape differs. For an unhit class Python writes `best: null` and C++ writes `""`. C++ nlohmann::json also sorts keys, where Python keeps …; refuse_llm_cover and only_llm_hits are C++ (and gui.py _only_llm_hits) additions. They are not missing. |
| `prism/thread.py` | `src/prism/stages/thread.cpp` | ported | none | Source lookup: Python _read_tu_text tries every function of the file (fn.file, cwd, package root, testdata/<name>) until one reads; C++ calls …; extra.writers is a JSON-encoded string in C++ and a list in Python (report-shape note).; The writes_global regex does not escape the global name (Python uses re.escape). This is harmless for C identifiers. |
| `prism/wp.py` | `src/prism/stages/wp.cpp` | ported | none | Edge case: Python keeps an empty stripped return expression in wp_returns/vcs; C++ skips it (`if (g.empty()) continue`).; The spec comes from parse_comments, so wp inherits the case-sensitivity gap listed under contracts.py. |

### B. Tools, scripts and Python call sites

| path | size | C++23 target |
|---|---|---|
| `tools/conformance.py` | XL | Make it `prism-qa conformance`, a new separate QA executable (not shipped in the release archive), as src/tools/qa/conformance.cpp plus a support library src/tools/qa/support/ containing: yaml_subset.{hpp,cpp}, task.{hpp,cpp} (Task/load_task/discover), … |
| `tools/csmith_soundness.py` | L | Make it `prism-qa soundness`, as src/tools/qa/soundness.cpp with generators in src/tools/qa/gen_random.cpp (Gen/PtrGen/LoopGen). It reuses the conformance support library (Task, run_prism, scalar_params, input_grid, run_sanitized, replay, list_stages, bwrap … |
| `tools/pir_vs_bmc.py` | S | Make it `prism-qa pir-vs-bmc [TREE] [--bin] [--report] [--jobs] [--json]`, as src/tools/qa/pir_vs_bmc.cpp. Read report.json with nlohmann, or with RunReport::load from prism_core. The run goes through the proctree runner. |
| `tools/pir_lean_check.py` | S | Make the driver `prism-qa pir-lean-check [TREE...] [--bin] [--checker] [--jobs] [--json] [--keep]`, as src/tools/qa/pir_lean_check.cpp, and reuse ai::find_lake() (include/prism/ai_proof.hpp:137) to locate lake. The checker itself, … |
| `tools/llvm_sem_vs_lli.py` | M | Make it `prism-qa llvm-sem-vs-lli [FILE.c...] [--bin] [--vectors] [--seed] [--pairs]`, as src/tools/qa/llvm_sem_vs_lli.cpp. Get the IR from pir::find_frontend + pir::lower_to_ir (include/prism/pir.hpp:529/533) instead of private clang/opt flags. llvm_eval … |
| `tools/solver_bench.py` | M | Make it `prism-qa solver-bench`, as src/tools/qa/solver_bench.cpp. It reuses task discovery from the conformance support library, and prism --pir-vcs/--solve-smt2 (pir::unit_vcs_json and solve_smt2_json in src/prism/pir/bench.cpp) stay as they are. Keep one … |
| `tools/libc_model_bounds.py` | S | Make it `prism-qa libc-bounds`, as src/tools/qa/libc_bounds.cpp, using the proctree runner and nlohmann for report.json. |
| `tools/triage_selfscan.py` | S | Make it `prism-qa triage-selfscan OUT/`, as src/tools/qa/triage_selfscan.cpp (nlohmann). Do not fold it into `prism triage`: that name is already the AI triage subcommand (src/prism/main.cpp:211, ai::triage over report.json). Port … |
| `tools/proctree.py` | M | Fold it into C++ as a library function. Either add `run_session()` to src/prism/proc.hpp and adapters.cpp next to detail::run_process, with argv, env overrides, cwd, stdin data or /dev/null, separate stdout and stderr, a timeout and an optional RLIMIT_AS in … |
| `tools/prism_prove.py` | S | Fold it into the existing `prism prove` subcommand (src/prism/ai/proof_search.cpp prove_main at :1013; lean_sorry_theorems at :255 already implements the per-file sorry scan and `prism prove FILE --list`). Add `prism prove --all [ROOTS...] [--list]`, which … |
| `tools/assurance_check.py` | S | Make it a standalone, dependency-free tool target: src/tools/assurance_check.cpp built as `prism_assurance_check` (std only, or PCRE2 through prism::Regex), or `prism-qa assurance-check`. Keep it buildable without Z3 or prism_core, so that docs.yml stays a … |
| `tools/svcomp/prism_svcomp.py` | XL | A `prism svcomp` subcommand, dispatched in src/prism/main.cpp next to prove/regress/ask/draft/triage, in src/prism/cli_svcomp.cpp plus src/prism/svcomp/ and include/prism/svcomp.hpp. Files: property.cpp (parse_property, SPEC_PATTERNS, width_dependent_code, … |
| `tools/svcomp/witness.py` | M | src/prism/svcomp/witness.cpp with include/prism/svcomp_witness.hpp, part of the `prism svcomp` subcommand. Build the document as nlohmann::ordered_json and write a small block-YAML emitter that keeps the _scalar quoting rules. sha256 comes from … |
| `tools/svcomp/prism.py` | none | MUST STAY Python: this is an external interface. BenchExec imports tool-info modules as Python classes (benchexec.tools.<name>.Tool, a BaseTool2 subclass, loaded with importlib; tool=".prism" means module `prism` on PYTHONPATH). SV-COMP expects the module … |
| `tools/svcomp/run_subset.py` | M | `prism svcomp score` subcommand in src/prism/cli_svcomp.cpp; or, if dev-only tools should stay out of the shipped binary, a src/tools/svcomp_score.cpp binary linking prism_core. It invokes `prism svcomp` per task as a child process in its own session, uses a … |
| `tools/svcomp/package_archive.py` | S | Either `prism svcomp pack --out F` in src/prism/cli_svcomp.cpp or a CMake target. The subcommand stages its own executable (/proc/self/exe) plus the tool-info module, README, fm-tools.yml and LICENSE, writes MANIFEST.json/dependencies.json with nlohmann, and … |
| `tools/svcomp/prism-subset.xml` | none | Keep it: this is BenchExec input data, not Python. Update only the header comment, which currently says the tool directory holds prism_svcomp.py, witness.py and the prism binary; after the port it holds only `prism`. |
| `tools/svcomp/fm-tools.yml` | none | Keep it as data. When filling it in for upload, name the upstreamed tool-info module (benchexec.tools.prism) and list runtime packages (clang/llvm for opt, bubblewrap), not python3. Check the field names against the current fm-tools schema. |
| `tools/svcomp/README.md` | none | Keep it as documentation and rewrite it for the C++ layout: `prism svcomp` is the executable; Python is needed only by BenchExec to load the tool-info module; `prism svcomp pack` (or the CMake target svcomp-archive) builds the archive. |
| `tools/prism_ai/__init__.py` | none | Delete it. Also delete the stale tools/prism_ai/__pycache__/*.pyc and tools/svcomp/__pycache__/*.pyc; they are build litter. |
| `tools/prism_ai/gbdt.py` | M | Move the trainer into src/prism/solver/gbdt.cpp next to the evaluator (predict.cpp's eval_tree/eval_target). The trainer and the loader then share one tree representation and one JSON schema: fit, fit_censored, to_json, and a Tree struct. It is used by the … |
| `tools/prism_ai/sched.py` | L | `prism_ai replay` in a separate offline tool binary under src/tools/prism_ai/ (sched.cpp), linking prism_core and built with a PRISM_TOOLS option. Better still, factor the ordering and head-start policy out of portfolio.cpp (lines 327 rule_estimate, 347 … |
| `tools/prism_ai/predict.py` | M | `prism_ai predict` in src/tools/prism_ai/predict.cpp, using solver/gbdt.cpp and the replay above. Take feature names directly from solver::predict::query_feature_names()/function_feature_names(), which removes the drift test. Write model JSON with nlohmann; … |
| `src/prism/solver/predict_default.inc` | S | Check in the model as src/prism/solver/predict_default.json. CMake reads it at configure time with file(READ) and writes ${CMAKE_BINARY_DIR}/generated/prism/predict_default.inc, chunked with string(SUBSTRING) at 12000 characters for MSVC's 16 KB literal … |
| `tools/prism_ai/measure.py` | S | `prism_ai measure {triage,ask,regress,draft}` in src/tools/prism_ai/measure.cpp. It reads RunReport::load and triage.json. For ask, it can call the ai ask query path in-process instead of spawning `prism ask`, or keep the subprocess to measure the real CLI. |
| `tools/prism_ai/sched_e2e.py` | S | `prism_ai e2e` in src/tools/prism_ai/e2e.cpp. Keep the child processes: fresh processes and per-pass environment variables are the point of the measurement. Use detail::run_process or a posix_spawn variant that takes an explicit environment, since … |
| `tools/prism_ai/sample_pairs.py` | S | `prism_ai sample-pairs` in src/tools/prism_ai/measure.cpp. |
| `tools/fuzz_self/make_corpus.py` | S | A small tool src/tools/fuzz_corpus/main.cpp producing `prism_fuzz_corpus OUT [--prism BIN] [--limit N]`, built under PRISM_FUZZ. It cannot live in prism_fuzz_self because libFuzzer owns main. Find clang/opt through pir::find_frontend, call … |
| `tools/fuzz_self/fuzz_py.py` | none | Delete it as obsolete: it only exercises prism/*.py, which the port removes. The C++ libFuzzer target tests/fuzz/fuzz_cparse.cpp already covers extract_functions, parse_gaps, pir::ir::parse_module, RunReport::load and … |
| `tools/fuzz_self/run_cpp.sh` | S | Replace it with a CMake custom target `fuzz-self-run` under PRISM_FUZZ. The target depends on prism_fuzz_self and prism_fuzz_corpus, runs the corpus tool and then the fuzzer with the same flags, and takes FUZZ_SECONDS as a cache variable. This drops both the … |
| `tools/gen_ai_grammars.py` | S | Fold it into CMake, following the configure-time generation of manifest_pins.hpp (CMakeLists.txt:41-81). Read each grammars/<name>.gbnf with file(READ), add it to CMAKE_CONFIGURE_DEPENDS, fail with message(FATAL_ERROR) if the text contains `)GBNF"`, and … |
| `tools/gen_astlint_discard.py` | M | Fold it into existing C++ code by inverting the source of truth. Make the discard family one constexpr table, for example src/prism/discard_rules.inc with rows {class, message, callee-names..., zero-arg}. checkers_*.cpp would build its line regexes from the … |
| `tools/gen_prism.py` | none | Delete it as obsolete, and make bmc_unenc.inc (1787 lines) and taxonomy.cpp (777 lines) the hand-maintained C++ source of truth. Its inputs, prism/bmc.py and prism/taxonomy.py, are deleted by the port. It is also already stale: I called gen_unenc() and … |
| `tools/gen_cxx.cpp.py` | none | Delete it as obsolete. src/prism/checkers_cxx.cpp (5234 lines) has been hand-maintained since the bootstrap, and prism/checkers.py goes away with the Python engine. |
| `tools/patch_cxx.py` | none | Delete it as obsolete. The current checkers_cxx.cpp has zero occurrences of py_or, str_index, group_of or the sub_of needle, so the shims were refactored out and the script has nothing left to patch. |
| `scripts/fetch_deps.py` | L | New standalone binary `prism-deps` (src/tools/prism_deps/{main,manifest,toml,tar,sha}.cpp) with subcommands `list`, `linked [--refetch]`, `tool NAME... [--no-build] [--tools-dir]` and `tree-digest DIR`, keeping the same flags and exit codes (0/1/2/3). It … |
| `scripts/sbom.py` | M | `prism-deps sbom [--manifest P] [--version V] [-o FILE]` in the same std-only prism-deps binary (src/tools/prism_deps/sbom.cpp). TestSbom becomes doctest cases. |
| `scripts/licence_check.py` | S | `prism-deps licence-check [--manifest P]` in the std-only prism-deps binary (src/tools/prism_deps/licence.cpp). The classify() table becomes a doctest table. |
| `scripts/python_plan_done.py` | none | delete (obsolete once the Python engine is gone; scripts/smoke_plan_done.py is its C++ twin) |
| `scripts/smoke_core.py` | S | Delete. If the coverage is wanted, add CTest cases (add_test running prism on those six testdata files) or doctest cases that assert each plant's status. Duplicate of scripts/smoke_core.sh. |
| `scripts/smoke_core.sh` | none | Delete together with smoke_core.py, or replace both with asserted CTest/doctest cases. |
| `scripts/smoke_plan_done.py` | S | Fold into tests/cpp as a doctest (e.g. TEST_CASE "plan done: abs_ok PROVED-UNBOUNDED; oob_write CRASH; missing ESBMC NOTRUN") through the in-process pipeline API, or as a CTest case. Then delete the script. |
| `scripts/exclude_large_files.py` | none | Delete (obsolete now that third_party holds only six pinned libraries). If a guard is wanted, add a doctest that fails on any tracked file over 10 MB outside third_party (via git ls-files). |
| `scripts/rewrite_history.sh` | S | Keep it as bash (it is not Python code), but replace git-filter-repo, which is a Python program installed with pip, by `git filter-branch --index-filter 'git rm -r -q --cached --ignore-unmatch <paths>' --prune-empty --tag-name-filter cat -- --all`, then … |
| `.github/workflows/ci.yml:11-31 (supply-chain job)` | S | Remove setup-python. Install clang-18 and ninja, build only the std-only prism-deps target (a PRISM_DEPS_ONLY option or a separate small CMake project, so Z3 is not configured), then run `prism-deps licence-check`, `prism-deps linked`, `prism-deps linked … |
| `.github/workflows/ci.yml:55-56 (SV-COMP archive packager smoke)` | M | After prism_svcomp.py and witness.py become a `prism svcomp` subcommand, produce the archive with CPack (install() rules plus `cpack -G TGZ`), with MANIFEST.json and dependencies.json emitted by `prism svcomp --archive-manifest`. Alternatively a `prism-dev … |
| `.github/workflows/ci.yml:78-103 (python job)` | XL | Delete the job. Its coverage moves into prism_tests (tests/cpp doctest), which the cpp job already runs. The ruff, mypy and yamllint parsers stay covered by /bin/sh stubs, as tests/cpp/test_main.cpp:4654-4680 already does. Engine-parity tests become … |
| `.github/workflows/conformance.yml:48-86 (conformance job, release gate)` | XL | `prism conformance` subcommand (src/prism/cli_conformance.cpp) with --self-check, --out, -j, --mem-limit-mb, --suite, --fetch-juliet and --certified. Scorer tests go to tests/cpp/test_conformance.cpp. It needs a YAML-subset reader for the SV-COMP … |
| `.github/workflows/conformance.yml:88-124 (random-programs job, nightly)` | L | `prism conformance random -n N --seed S [--generator inhouse\|csmith] --out DIR` (the in-house generator ported to C++23 in src/prism/cli_conformance_random.cpp) or a separate src/tools/prism_soundness.cpp. |
| `.github/workflows/conformance.yml:126-158 (juliet job, nightly)` | M | `prism conformance --fetch-juliet DIR` and `--suite DIR`. Zip extraction via the host `unzip` after the sha256 check (no zlib or libzip is vendored). |
| `.github/workflows/docs.yml:16-28` | M | `prism-dev assurance-check` and `prism-dev docs-check` (src/tools/prism_dev.cpp, std-only, no Z3), or doctest cases in tests/cpp/test_docs.cpp that parse `prism --help`. The docs job then builds only that small target. |
| `.github/workflows/proofs-recheck.yml:77-80 (+ paths filter :26, :32)` | S | Build the std-only prism-deps first (clang/cmake step), then run `prism-deps tool lean4export nanoda`. Point the paths filter at src/tools/prism_deps/**. |
| `.github/workflows/release.yml:27-30` | S | Build prism-deps and run `prism-deps licence-check` and `prism-deps linked`, or drop these two steps because the Dockerfile repeats both checks inside the reproducible build. |
| `.github/workflows/self-check.yml:26-55` | S | Drop setup-python, pip and the pytest step (prism_tests covers them). Replace the conformance step with `build-san/prism conformance -j 2 --mem-limit-mb 0`. |
| `Dockerfile:34,48-49,70` | S | `cmake --build /build --target prism-deps && /build/prism-deps licence-check && /build/prism-deps linked`, then the full build, then `/build/prism-deps sbom --version ${PRISM_VERSION} -o /out/prism.cdx.json`. Remove python3 from apt only once Z3 no longer … |
| `CMakeLists.txt:122-130 -> third_party/z3/CMakeLists.txt:162 (vendored Z3 build)` | XL | MUST STAY Python as things stand; there is no upstream Python-free Z3 build. Options: (a) PRISM-owned cmake/z3_sources.cmake that lists the Z3 component sources, never edits third_party, and runs a C++23 generator src/tools/z3gen.cpp replacing pyg2hpp, the … |
| `src/prism/polyglot.cpp:79-116,149-154,569,644 (python-syntax check / SYNTAX_HELPER)` | M | Move JSON and TOML into C++ builtins in polyglot.cpp. JSON via nlohmann::json::parse, turning the error byte offset into line and column. TOML via a full TOML 1.0 reader: extend the prism-deps reader, or add toml++ as a seventh linked library through the … |
| `src/prism/polyglot.cpp:155-170,227-230 (ruff / pyflakes / mypy / yamllint rows)` | none | MUST STAY as external tools. They are the linters of the language being scanned, like eslint or clippy; PRISM does not ship them and reports NOTRUN when they are missing. No port. Only CI stops installing them, and the doctest stubs keep the output parsers … |
| `src/prism/config.cpp:134-140 (adapter_install) + src/prism/main.cpp:304 (help text)` | S | Change the string to 'prism-deps tool <component> (pinned in third_party/MANIFEST.toml)' and update test_main.cpp:4550. |
| `tests/cpp/test_certified.cpp:939-949` | S | Replace the hog with C++. Either add add_executable(prism_test_hog tests/cpp/hog.cpp), which mallocs and touches N MB then sleeps, and pass its path to the test via a compile definition, or re-exec prism_tests with a hidden --hog flag. |
| `proofs/check.sh:21-33,50-62` | S | A std-only single-file C++23 tool, src/tools/lean_audit.cpp, with subcommands `scan --words W,.. [--toplevel axiom,unsafe] PATHS` and `axioms LOG --allowed propext,Classical.choice,Quot.sound`. The script compiles it with `${CXX:-c++} -std=c++23 -O2` (no … |
| `proofs/semantics/check.sh:17-35` | S | `lean_audit scan --words sorry,admit,native_decide,bv_decide,implemented_by,extern --toplevel axiom,unsafe PrismSem.lean PrismSem/*.lean Audit.lean` |
| `proofs/refinement/check.sh:22-55` | S | `lean_audit scan ...` and `lean_audit axioms axioms.txt` |
| `proofs/refinement/recheck.sh:51-64 (+ hint text :23, :79)` | S | `lean_audit scan --words sorry,native_decide,bv_decide --toplevel axiom,unsafe "$proj"`. Change the hint to `prism-deps tool lean4export nanoda`. |
| `tools/fuzz_self/run_cpp.sh:22` | S | `prism-dev fuzz-corpus OUT [--prism BIN] [--limit N]` in src/tools/prism_dev.cpp, or a --make-corpus mode of a helper next to tests/fuzz/fuzz_cparse.cpp. Drop the `python -m prism` fallback. Replace the pyproject.toml seed with proofs/lakefile.toml. |
| `tools/svcomp/prism-subset.xml:6-7,12 (BenchExec tool-info tools/svcomp/prism.py; …` | L | Port prism_svcomp.py and witness.py to a `prism svcomp TASK.c --prop P [--data-model ILP32\|LP64]` subcommand (src/prism/cli_svcomp.cpp) that prints PRISM-SVCOMP-RESULT and writes witness 2.0 YAML. The tool-info module prism.py MUST STAY Python, because … |
| `tools/svcomp/fm-tools.yml:52` | none | Change the text to name the C++ packager (CPack TGZ or prism-dev svcomp-pack). No code. |
| `pyproject.toml` | none | Delete with prism/*.py. First move prism/cocci/*.cocci out of the Python package (see the src/prism/adapters.cpp:1048-1070 entry). |
| `src/prism/adapters.cpp:1048-1070 (cocci_rules: <ancestor>/prism/cocci)` | S | Move the rules to a non-Python location (e.g. rules/cocci/ or share/prism/cocci/, installed next to the binary) or embed them at configure time the way shipped_docs.inc is generated (CMakeLists.txt:84-96). Update the search in adapters.cpp:1060 in the same … |
| `requirements.txt` | none | Delete. The GUI is src/gui (prism_gui, Qt 6) and Z3 is vendored and linked. |
| `docs/USER_GUIDE.md:50-56,153,371-374,398,412` | S | Delete the Python engine section. Rewrite the commands as `prism conformance [--self-check\|--certified]`, `prism conformance random -n 300` and `prism-deps tool cadical cake_lpr`. The generator line points at the C++ port of gen_astlint_discard (a CMake … |
| `README.md, CLAUDE.md, NOTICE, docs/*.md (other docs)` | M | Rewrite each doc in the same PR that deletes the script it names. CLAUDE.md goes from 'One product, two engines' to one engine, with the laws that cite prism/laws.py and prism/sandbox.py re-pointed at include/prism/laws.hpp and src/prism/sandbox.cpp. … |
| `.gitignore:7-12,22` | none | Remove the Python ignores after the last .py file is gone (leftover __pycache__ directories exist today under scripts/, tools/, tests/ and prism/). Update the comment to name prism-deps. |
| `src/prism/ai/regress.cpp:270-311,490-515,648 (pytest output framework)` | none | Stays. It is an output format for users' Python projects, not PRISM's own Python. No port. |
| `src/prism/capi.cpp + CMakeLists.txt:339-344,403-407 (prism_native SHARED target)` | S | delete: remove the prism_native target, capi.cpp and the PRISM_EXPORTS/PRISM_CAPI macros once prism/simdmut.py is gone. havoc.cpp/hash.cpp stay in prism_core. Before deleting, confirm no external consumer (for example an AFL custom mutator) dlopens … |
| `prism/ai.py:_llama_n_gpu_layers / _try_llama_cpp_bindings (PRISM_N_GPU_LAYERS, …` | S | fold into src/prism/stages/llm.cpp NativeLlama::load: read PRISM_N_GPU_LAYERS, or better a Config field plus a CLI flag, when PRISM_CUDA/GGML_CUDA is ON; default 0 when CUDA is off. Add it to the CLAUDE.md env list (a PRISM_* name, so the naming test passes). |
| `.github/workflows/proofs.yml:43 and .github/workflows/proofs-semantics.yml:46` | S | no workflow change once check.sh stops using python3. Replace the Python snippets with a Lean exe (proofs already builds Main.lean, which can emit and diff the JSON) or a small `prism verdict-tables --check` subcommand. Note that proofs.yml sparse-checks out … |
| `tests/cpp/test_main.cpp:4550 (and other C++ tests that pin Python text)` | S | update in the same commit as the config.cpp/main.cpp:304 hint change to the new fetch command (for example `prism deps --tool esbmc` or a `prism-fetch` binary). |
| `.dockerignore:7-10` | S | delete these lines after the Python removal (harmless if kept). Also remove the stray untracked __pycache__/ dirs in the working tree (prism/, tools/, tools/svcomp/, tools/prism_ai/, tests/). |
| `docs/assurance/*.md (ASSURANCE_CASE.md 13, EVIDENCE.md 35, DO-330.md 3, DO-333.md 2, …` | M | doc rewrite: map each cited tests/test_*.py to its doctest TEST_CASE name in tests/cpp/*.cpp and each tools/*.py to its C++ subcommand. Do this in the same change as the assurance_check port (for example `prism assurance-check`), so the check verifies the … |
| `Python-engine parity comments across src/, include/, tests/cpp/, proofs/, CMakeLists.txt` | M | doc sweep in the final commit: re-point each reference to the C++ owner (for example 'locked by TEST_CASE "..."' in place of 'tests/test_ai.py'). Generated-file headers (grammars*.inc, predict_default.inc, astlint_discard.inc) must name the C++ generator … |
| `tests/data/selfscan_min, tests/data/ask_questions*.jsonl, tests/cxx_models/*.cpp (test …` | S | keep the data, re-home the readers: the doctest ports of test_triage_selfscan and test_cxx_models, and the C++ port of measure.py ('ask' mode), must load these paths. Add the held-out set to the ported measure command so the 12/12 claim in docs/AI.md stays … |
| `third_party/llama.cpp/ggml/src/ggml-opencl/CMakeLists.txt, ggml-webgpu/CMakeLists.txt …` | none | MUST STAY non-C++ (third_party is never edited). Add set(GGML_OPENCL OFF) and set(GGML_WEBGPU OFF CACHE BOOL "" FORCE) in CMakeLists.txt so a Python-free build cannot be broken by a cache flip. Document that users must obtain GGUF files pre-converted, since … |
| `src/prism/scope.cpp:15, src/prism/ai/ask.cpp:120, src/prism/polyglot.cpp:44 (Python as a …` | none | MUST STAY (product feature: scanning user Python code). A 'no Python' grep gate in CI must allow-list these files, along with ai/regress.cpp's pytest emitter and polyglot's python-syntax, python-lint and python-types rows. Otherwise the gate forces removing … |

### C. Python tests

| test file | kind | size | destination |
|---|---|---|---|
| `tests/__init__.py` | python-engine-behaviour | none | delete (no Python tests remain) |
| `tests/conformance/test_conformance_suite.py` | tool | L | port tools/conformance.py to a C++ executable (prism-conformance) with tests/cpp/test_conformance.cpp for convert/classify/parse_cex/gate/sample; corpus-shape checks -> C++ repo-lint; drop the … |
| `tests/test_adapters.py` | python-engine-behaviour | L | new tests/cpp/test_adapters.cpp: fake-exe shell scripts in a temp dir fed to prism::run_cbmc (adapters.cpp:915), run_spatch (1108), run_semgrep (1268), run_infer (1340), run_optional_tools (2142), … |
| `tests/test_afl.py` | python-engine-behaviour | S | merge into tests/cpp/test_fuse.cpp with test_prism_afl.py cases (fake afl-fuzz script) |
| `tests/test_afl_flag.py` | python-engine-behaviour | S | tests/cpp/test_fuse.cpp: setenv PRISM_AFL with empty PATH and assert NOTRUN/engine!=afl |
| `tests/test_ai.py` | drives-cpp-binary | M | ctest-registered C++ driver running build/prism with an in-process C++ fake llama-server; grammar-verbatim check to C++ repo-lint |
| `tests/test_ai9.py` | mixed | L | tests/cpp/test_ai9.cpp for unit parts; ctest-registered C++ driver (needs a C++ fake llama-server test double) running build/prism prove/review; grammar-verbatim and no-sorry become C++ repo-lint … |
| `tests/test_ai_assist.py` | mixed | L | trainer tools/prism_ai -> C++ tool (src/tools/prism_ai/*.cpp) with doctests in tests/cpp/test_ai_assist.cpp; grammars_assist.inc generated at build time by CMake (removes lock) or repo-lint; e2e CLI … |
| `tests/test_assurance_check.py` | tool | S | port tools/assurance_check.py to a C++ tool (tools/assurance_check.cpp) and register a ctest that runs it on the repo plus a temp tree with a missing artefact |
| `tests/test_astlint_table.py` | tool | S | source of truth moves to C++ (checkers_*.cpp) once prism/checkers.py is deleted: delete the generator check (or port generator to C++); add a doctest that every astlint_discard.inc class is in … |
| `tests/test_bmc.py` | python-engine-behaviour | XL | tests/cpp/test_bmc.cpp (new doctest file): table-driven {file, function, expected status, allowed cls set, must-not-status set} over prism::run_bmc(load_fn(...), 8) plus k-induction extras; port the … |
| `tests/test_bmc_goto_shift.py` | mixed | none | delete (parity with deleted Python engine; C++ doctests already cover) |
| `tests/test_bmc_reach_error.py` | mixed | none | delete (parity with deleted engine; doctests cover) |
| `tests/test_bmc_soundness.py` | python-engine-behaviour | S | delete (duplicated in test_main.cpp); add enum-uncomputed/octal and object-macro cases to test_main.cpp bmc soundness section |
| `tests/test_cbmc.py` | mixed | S | tests/cpp/test_adapters.cpp with fake cbmc against prism::run_cbmc (adapters.cpp:915) |
| `tests/test_certified.py` | mixed | S | tests/cpp/test_certified.cpp for remaining e2e (test_certified_run/test_plain_run_never_certifies via run_pipeline); doc-anchor checks to a C++ repo-lint (prism_repolint doctest reading docs/*.md); … |
| `tests/test_clang_tidy.py` | mixed | S | tests/cpp/test_adapters.cpp: prism::run_clang_tidy (adapters.cpp:836) with fake-exe stubs |
| `tests/test_cocci.py` | python-engine-behaviour | S | relocate prism/cocci/*.cocci (C++ cocci_rules hardcodes <base>/prism/cocci) and add doctest on prism::cocci_rules in tests/cpp/test_adapters.cpp |
| `tests/test_codeql.py` | mixed | S | C++ repo-lint (no 'codeql' in src/prism, taxonomy, polyglot tables; MANIFEST reason) + doctest that run_optional_tools never spawns a PATH codeql |
| `tests/test_compiler.py` | python-engine-behaviour | M | tests/cpp/test_main.cpp 'warnings:' cases with fake gcc/clang scripts against prism::run_compiler (adapters.cpp:1750): unmatched exit, timeout, c++ std, same-binary dedup, no -w/-Wno-* |
| `tests/test_conc.py` | mixed | S | tests/cpp/test_conc.cpp: add table-driven run over tests/conc and stage-order/EXEC_STAGES/origin checks; delete Python NOTRUN-row test |
| `tests/test_concolic.py` | python-engine-behaviour | XL | tests/cpp/test_concolic.cpp (new doctest file) calling prism::run_concolic from src/prism/stages/concolic.cpp over testdata/*.c\|*.cpp; use a table-driven TEST_CASE (plant file, function, expected … |
| `tests/test_concrete.py` | python-engine-behaviour | L | new tests/cpp/test_concrete.cpp against src/prism/stages/interp.cpp + fuse.cpp using testdata/*.c planted functions |
| `tests/test_confidence.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp confidence section: add the 5 missing cases |
| `tests/test_contracts.py` | python-engine-behaviour | S | tests/cpp/test_contracts.cpp for star/call decreases and void contracts; rest delete |
| `tests/test_core.py::TestBMC` | python-engine-behaviour | S | tests/cpp/test_main.cpp, in the #ifdef PRISM_HAS_Z3 block near line 679. Add TEST_CASE 'bmc proves saturate' and TEST_CASE 'bmc POINTER null_branch is NEEDS-HARNESS'. Add … |
| `tests/test_core.py::TestClassify` | python-engine-behaviour | S | Add to tests/cpp/test_main.cpp next to 'parser forms and PARSE-GAP', using the existing load_fn(file, name) helper with explicit file names (add_overflow.c, null_branch.c, shift_ub.c, … |
| `tests/test_core.py::TestLaws` | python-engine-behaviour | none | Delete: it duplicates tests/cpp/test_main.cpp:117. |
| `tests/test_core.py::TestLints` | python-engine-behaviour | L | New doctest file tests/cpp/test_lint_corpus.cpp, added to add_executable(prism_tests ...) at CMakeLists.txt:475-480 (sources are listed there by hand, not globbed). Make it table-driven: struct … |
| `tests/test_cppcheck.py` | mixed | M | tests/cpp/test_adapters.cpp: call prism::run_cppcheck with a fake-exe script (shell stub in a temp dir emitting XML / exit codes) instead of mock.patch(subprocess); drop the source-read half … |
| `tests/test_cuda.py` | mixed | S | move INTERESTING tables to one shared header included by mutate.cu and havoc.cpp (removes the lock); CMake WARNING check -> repo-lint; coverage GAP cases -> doctest on taxonomy coverage in … |
| `tests/test_cxx_models.py` | tool | S | ctest-registered test (CMake add_test building each program twice and a small C++ driver comparing outputs), skip when clang++ sanitizer runtimes missing |
| `tests/test_dafny.py` | mixed | S | tests/cpp/test_adapters.cpp with fake dafny against prism::run_dafny (adapters.cpp:2102) |
| `tests/test_diff.py` | python-engine-behaviour | M | tests/cpp/test_diff.cpp against src/prism/stages/diff.cpp (pairing/disagree/timeout; compiler-missing via scoped PATH) |
| `tests/test_docs_cli.py` | mixed | M | ctest-registered C++ driver (prism_repolint) that runs build/prism --help and build/prism prove --help and scans docs/*.md + src/include for anchors; drop test_python_engine |
| `tests/test_esbmc.py` | python-engine-behaviour | M | tests/cpp/test_adapters.cpp using fake esbmc shell scripts on a temp PATH/PRISM_TOOLS_DIR |
| `tests/test_exec_safety.py` | mixed | M | parity text checks deleted; add to test_main.cpp 'sandbox:' cases: real bwrap jail write-outside-scratch and rlimit enforcement (skip without bwrap), planted manifest refusal, mypy-plugin sentinel, … |
| `tests/test_execute_compile.py` | python-engine-behaviour | M | tests/cpp/test_main.cpp execute/rlef section (or new test_execute.cpp) against prism::sandbox_run (stages/llm.cpp:272), interpreter_loop (stages/execute_cex.cpp:23), rlef_repair (stages/rlef.cpp:64) … |
| `tests/test_execute_posix.py` | reads-cpp-source | S | delete (source greps; behaviour already in test_main.cpp); add a behavioural binary-fuzz-ordering case to test_concrete.cpp if wanted |
| `tests/test_false_positives.py` | mixed | S | tests/cpp/test_main.cpp false-positive section; add cparse head-shape/kind tables to a tests/cpp/test_cparse.cpp; delete test_same_findings (parity with deleted engine). Relaxed on purpose until deletion: the C++-only fixed files (`CPP_ONLY`) are left out of the Python corpus checks and the engine comparison, and PTR-CHAIN-NULL is compared on the first row per access (C++ reports one row per access, Python one per line); tests/cpp/test_checkers_c.cpp and the test_main false-positive corpus test hold the C++ expectations |
| `tests/test_framac.py` | mixed | S | tests/cpp/test_adapters.cpp: prism::run_frama_c (adapters.cpp:1418) with fake-exe stubs |
| `tests/test_fuse.py` | mixed | L | tests/cpp/test_fuse.cpp calling branch_goals/run_fuse from src/prism/stages/fuse.cpp and the autoprompt helpers in src/prism/stages/llm.cpp (need exposing in an internal header); mock LLM via a fake … |
| `tests/test_fuzz_findings.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp: F2 via prism::RunReport::load (used in pipeline.cpp:235) on the five bad texts, F3 via journal_read_stages; F4 delete (C++ has no runtime manifest loader: pins are baked in … |
| `tests/test_fuzz_perf_regressions.py` | python-engine-behaviour | S | tests/cpp/test_concrete.cpp: int64 division/remainder exactness and 32-bit truncation in stages/interp.cpp and rapid.cpp; delete Python memo/cache tests |
| `tests/test_gui.py` | mixed | L | new tests/cpp/test_gui.cpp (QT_QPA_PLATFORM=offscreen, built only when PRISM_QT=ON; factor finding-row/confidence-label helpers out of MainWindow into a Qt-free unit so they test headless); delete … |
| `tests/test_harness.py` | mixed | S | tests/cpp/test_main.cpp harness section: add plusplus overflow, uninit read, requires-copy PROVED-ASSUMING cases via prism::run_harness_bmc |
| `tests/test_infer.py` | python-engine-behaviour | S | tests/cpp/test_adapters.cpp: prism::run_infer (adapters.cpp:1340) with fake-exe stubs |
| `tests/test_inline.py` | python-engine-behaviour | S | new 'inline:' doctests in tests/cpp/test_main.cpp against prism::inline_static (inline.cpp:460) on testdata/inline_add.c |
| `tests/test_interval.py` | python-engine-behaviour | M | tests/cpp/test_interval.cpp table-driven over testdata plants via prism::run_interval |
| `tests/test_jobs.py` | python-engine-behaviour | S | tests/cpp/test_cli.cpp: factor main.cpp argv parsing into a testable parse_args (or drive build/prism --jobs 3 via a ctest driver that checks report.json config) and a doctest that … |
| `tests/test_journal.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp journal section: add pipeline-resume cases against run_pipeline |
| `tests/test_klee_adapter.py` | python-engine-behaviour | M | tests/cpp/test_adapters.cpp with a fake klee script writing .err/.ktest into klee-out |
| `tests/test_libfuzzer.py` | mixed | M | tests/cpp/test_adapters.cpp: prism::libfuzzer_probe(Config) (adapters.cpp:970) with PATH pointed at fake clang stubs; campaign cases via the fuzz stage with --allow-exec off/on |
| `tests/test_lints_fastpath.py` | python-engine-behaviour | S | strip/match_brace fuzz, sorted names and the interval va_arg gate are in tests/cpp/test_lint_corpus.cpp ("lint fastpath: ..."). The _required_literal and PYTHONHASHSEED cases test only the Python engine: they stay until phase 5 and are deleted with prism/checkers.py |
| `tests/test_llm.py` | python-engine-behaviour | M | tests/cpp/test_llm.cpp against src/prism/stages/llm.cpp + hypothesize.cpp with a fake local HTTP server (or engine seam) for error/timeout cases |
| `tests/test_ltl.py` | mixed | S | tests/cpp/test_main.cpp ltl section (or split tests/cpp/test_ltl.cpp): add the FSM-extraction and comments_only.ltl cases; drop source-read test |
| `tests/test_mined.py` | python-engine-behaviour | S | tests/cpp/test_contracts.cpp + tests/cpp/test_ltl.cpp for the gaps (ACSL no-leak, seeds from cex, branch goals in stages/concolic.cpp); rest delete as duplicated |
| `tests/test_muttest.py` | python-engine-behaviour | S | add 'muttest survived mutant is FAILED' doctest to test_main.cpp; rest delete |
| `tests/test_naming.py` | repo-lint | S | C++ repo-lint ctest (tests/cpp/repo_lint.cpp) walking the tree with std::filesystem + std::regex icase; drop test_python_package_is_prism |
| `tests/test_optional_honesty.py` | python-engine-behaviour | S | tests/cpp/test_config.cpp (scoped PRISM_TOOLS_DIR temp tree; non-exe source file rejected; clang-tidy unpinned; precedence) |
| `tests/test_pbsd.py` | mixed | M | tests/cpp/test_main.cpp pbsd section: add capacity.c lint, per-heavy-binary-missing NOTRUN with fake tree, uaf/lock/format plants; hardcoded-path check -> repo-lint; source-parity checks deleted |
| `tests/test_pipeline.py` | python-engine-behaviour | M | tests/cpp/test_main.cpp pipeline section for LLM-stage READS, missing esbmc, unify confidence 0; planted-bug corpus -> ctest driver running build/prism on testdata |
| `tests/test_pir.py` | mixed | M | ctest-registered C++ driver running build/prism --stage pir on tests/pir and checking report.json expectations (tests/cpp/e2e_pir.cpp); delete test_stage_after_bmc_in_both_engines and … |
| `tests/test_pir_refinement.py` | reads-cpp-source | M | C++ repo-lint doctest (tests/cpp/test_repolint.cpp) reading the .cpp and .lean texts with std::regex; fixture run as a ctest driver invoking build/prism + pir_lean_check (tools/pir_lean_check.py … |
| `tests/test_polyglot.py` | mixed | M | tests/cpp/test_polyglot.cpp (feed canned tool output to the polyglot.cpp parsers); delete the 9 table-parity tests |
| `tests/test_prism_adapters.py` | reads-cpp-source | M | tests/cpp/test_adapters.cpp as behaviour tests with fake tool scripts (replaces the source greps); delete Python-parity is_fake_adapter check |
| `tests/test_prism_afl.py` | mixed | M | tests/cpp/test_fuse.cpp (set PRISM_AFL/PRISM_LIBFUZZER with scoped env + empty PATH/PRISM_TOOLS_DIR, assert NOTRUN and extra.afl); delete the source-grep checks |
| `tests/test_prism_compiler.py` | reads-cpp-source | S | tests/cpp/test_adapters.cpp behavioural run_compiler cases with fake gcc/clang stubs (dedupe via symlink, -Wno refusal, rc mapping) |
| `tests/test_prism_gui.py` | mixed | M | Python --gui/PySide cases deleted; factor main.cpp gui spawn (find prism_gui, strip --gui) into a testable function with doctests in tests/cpp/test_main.cpp; MainWindow/main.cpp contracts -> Qt Test … |
| `tests/test_prism_jobs.py` | reads-cpp-source | S | doctest on prism::clamp_jobs and CLI parse in test_main.cpp; jthread/CMake checks -> repo-lint |
| `tests/test_prism_klee.py` | mixed | S | tests/cpp/test_adapters.cpp with fake klee writing klee-out dumps against prism::run_klee (adapters.cpp:1508) |
| `tests/test_prism_optional.py` | reads-cpp-source | S | tests/cpp/test_adapters.cpp as behavioural tests: prism::probe_exe (adapters.cpp:434) and run_optional_tools with fake-exe stubs (unknown option, silent help, 127 text) |
| `tests/test_prism_sanitize.py` | reads-cpp-source | S | delete; covered by behavioural tests/cpp/test_sanitize.cpp (see test_sanitize.py) |
| `tests/test_proctree.py` | tool | S | delete with the Python tools; ported conformance/svcomp C++ tools reuse the engine's process runner, add a session-kill doctest in test_conformance.cpp |
| `tests/test_proofs_float_conc.py` | reads-cpp-source | S | C++ repo-lint doctest (tests/cpp/test_repolint.cpp) doing the same text/regex comparisons |
| `tests/test_proofs_loopcut.py` | reads-cpp-source | S | C++ repo-lint ctest (tests/cpp/repo_lint.cpp) doing the same text checks over proofs/ and src/prism/pir/ |
| `tests/test_py_parallel_order.py` | python-engine-behaviour | S | delete Python ordered_map cases; add doctest that run_compiler / run_lints output is identical for jobs=1 and jobs=4 (parallel_for in include/prism/threads.hpp) |
| `tests/test_py_type_regressions.py` | python-engine-behaviour | S | delete the Finding tuple and sibling_guard cases (Python typing artefacts, no C++ equivalent). Port only the infer-scratch case to tests/cpp/test_adapters.cpp: write a fake POSIX 'infer' script into … |
| `tests/test_rapid.py` | mixed | S | tests/cpp/test_main.cpp rapid/muttest section: add the missing mutation cases; drop signature/source-read tests |
| `tests/test_repair_verdict.py` | drives-cpp-binary | M | ctest-registered C++ driver with C++ fake llama-server running build/prism --stage rlef_repair, or doctest in test_ai_assist.cpp asserting extra.patch_verdict |
| `tests/test_sanitize.py` | python-engine-behaviour | M | tests/cpp/test_sanitize.cpp (needs an injectable process-runner seam in adapters.cpp run_sanitize, or fake cc scripts in a temp PATH) |
| `tests/test_sarif.py` | mixed | M | tests/cpp/test_sarif.cpp for to_sarif/exit_code matrix; a ctest driver running build/prism --fail-on defect on testdata for the CLI exit code; delete engine-parity cases … |
| `tests/test_scope.py` | mixed | S | extend the existing TEST_CASE in tests/cpp/test_main.cpp (add third_party row, extra map, report.md substring); delete the source-grep test_both_engines_wire_it (parity with deleted engine; a … |
| `tests/test_semgrep.py` | python-engine-behaviour | M | tests/cpp/test_adapters.cpp (new) with fake semgrep against prism::run_semgrep (adapters.cpp:1268) |
| `tests/test_simdmut.py` | mixed | S | tests/cpp/test_havoc.cpp (table values vs AFL constants, overlay LE clipping, length preservation, havoc.cpp vs mutate.cu table text as a repo-lint); delete ctypes loader / capi binding / … |
| `tests/test_solver.py` | reads-cpp-source | S | C++ repo-lint check (new tests/cpp/test_repo_lint.cpp or tools/repo_lint.cpp registered in ctest) |
| `tests/test_spatch.py` | mixed | S | tests/cpp/test_adapters.cpp against prism::run_spatch (adapters.cpp:1108) and cocci_rules (1048). cocci_rules searches <base>/prism/cocci, so the .cocci rules must move out of prism/ (e.g. … |
| `tests/test_stage_order.py` | mixed | S | tests/cpp/test_pipeline.cpp: run_pipeline with a trivial tree and CHECK report.stages names == STAGE_ORDER (behavioural replacement for the stage("...") regex); plus a ctest-registered driver … |
| `tests/test_strix.py` | python-engine-behaviour | S | tests/cpp/test_adapters.cpp with fake strix against prism::run_strix (adapters.cpp:1615) |
| `tests/test_supply_chain.py` | tool | L | after porting scripts/fetch_deps.py, licence_check.py, sbom.py to C++ tools (prism-fetch-deps, prism-licence-check, prism-sbom): tests/cpp/test_supply_chain.cpp for manifest parsing, digest, SBOM … |
| `tests/test_svcomp.py` | tool | XL | port wrapper to C++ (e.g. `prism svcomp` subcommand or src/tools/prism_svcomp.cpp + witness writer) with tests/cpp/test_svcomp.cpp; the 2 e2e cases become a ctest driver running build/prism on … |
| `tests/test_taint.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp (or new tests/cpp/test_taint.cpp): add TEST_CASE on taint_sink.c: run -> FAILED TAINT-SINK, stage=='taint', strength==STRENGTH_FINDS; ok -> no hits; no hit has CLEAN/PROVED … |
| `tests/test_taxonomy.py` | python-engine-behaviour | M | tests/cpp/test_taxonomy.cpp (build Finding vectors, call taxonomy/confidence directly) |
| `tests/test_thrd_unenc.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp: add thrd_* table over the testdata thrd plant with bmc + thread stage (src/prism/stages/thread.cpp) |
| `tests/test_thread.py` | python-engine-behaviour | S | tests/cpp/test_thread.cpp against src/prism/stages/thread.cpp on the same testdata files |
| `tests/test_triage_selfscan.py` | tool | S | port tools/triage_selfscan.py to a C++ tool (e.g. prism selfscan-triage subcommand or tools/triage_selfscan.cpp) and register a ctest running it on tests/data/selfscan_min checking the same output … |
| `tests/test_unencoded_contract.py` | python-engine-behaviour | S | add strcpy/strcat/sprintf-not-unencoded and strcat oracle crash doctests against prism::unencoded_syntax_reason (interval.cpp:402); rest delete |
| `tests/test_verdict.py` | mixed | S | delete Python-side and parity cases; move lean-no-sorry and lean-toolchain-pinned into a C++ repo-lint ctest |
| `tests/test_wp.py` | python-engine-behaviour | S | tests/cpp/test_main.cpp wp section: add rewrite + failed_ensures cases |
### D. Open items from the gap audit (178 after merging duplicates)

From now on, fixes land in C++ only. Where the Python engine has the same
bug, it is deleted in phase 5 rather than fixed. Sorted with the possible
wrong proof first, then by size.

| # | item | size | where |
|---|---|---|---|
| D1 | bmc models assume_abort_if_not by name even when the unit defines it with a body that does not abort, which gives wrong PROVED verdicts | S | src/prism/bmc_encoder.inc:2414 (model_call); prism/bmc.py:2380 (_model_call). Not … |
| D2 | PTR-UNCHECKED-ALLOC false alarm when the NULL check is inside a function-like macro (tinyexpr CHECK_NULL) | S | docs/EVALUATION.md:193 ('FP, **open** (the check is inside a function-like macro the … |
| D3 | CTRL-FALLTHROUGH false alarm on a case arm that ends in a nested switch whose every arm returns | S | docs/EVALUATION.md:191 ('tinyexpr.c:882/896 CTRL-FALLTHROUGH ... FP, **open**') |
| D4 | STR-MISSING-NUL false alarm when the terminator is written after the memcpy (tinyexpr smoke.c) | S | docs/EVALUATION.md:189 and :241-243 ('STR-MISSING-NUL on a later terminator' listed open) |
| D5 | MEM-CAPACITY-FIRST false alarm when the failure arm frees the whole struct (zlib examples/zran.c) | S | docs/EVALUATION.md:201 ('pbsd examples/zran.c:110 MEM-CAPACITY-FIRST ... the failure arm … |
| D6 | taint TAINT-SINK false alarm on memcpy(dst, tainted, len) when dst = malloc(len + 1) and len = strlen(tainted) (zlib examples/gun.c) | S | docs/EVALUATION.md:202 ('taint examples/gun.c:689 memcpy(outname, *argv, len) ... FP, … |
| D7 | Trained unwind (bound) prediction is never used: pir does not call predict::unwind_for | S | docs/ROADMAP_STATUS.md:111 (9.3: 'Unwind prediction measured and **off** ... not wired … |
| D8 | bmc assert parsing uses a greedy regex, so any block with two or more assert()/__VERIFIER_assert statements is a front-end ERROR | S | docs/SVCOMP.md:401-402 (MADWiFi-encode_ie_ok is a bmc ERROR 'as before this branch'); … |
| D9 | The pipeline inlines before run_bmc, so bmc nondet call positions and loop positions (for witnesses) are lost in any function that got an inlined call | S | docs/SVCOMP.md:104-110 and 619-623 ('tagged into the call's name before inlining'); … |
| D10 | main(int argc, char *argv[]) is NEEDS-HARNESS in bmc even when the body never uses argv | S | docs/SVCOMP.md:397 (large_const, heapsort NEEDS-HARNESS); src/prism/bmc.cpp:3300-3306 … |
| D11 | SV-COMP replay stubs lose precision for 64-bit values and fail on nan, inf, -0.0 and __VERIFIER_nondet_unsigned | S | docs/SVCOMP.md:619-631 (item 1: nondet gaps, floating-point nondet not exercised); … |
| D12 | function_return witness constants for floating-point nondet values are not exact, and nan/inf give invalid ACSL | S | docs/SVCOMP.md:629-630; tools/svcomp/witness.py:130-136 (_acsl_literal), 159-166 |
| D13 | bmc nondet call columns and loop-keyword columns are off after a block comment on the same line | S | docs/SVCOMP.md:625-628; src/prism/cparse.cpp:759-777 (strip_comments_keep_lines drops … |
| D14 | pir's shift-base check mixes negative-base and overflow into one property, so a negative-base shift blocks no-overflow answers | S | docs/SVCOMP.md:681-685 and 145-152; src/prism/pir/translate.cpp:936; … |
| D15 | bmc does not model printf (or puts/putchar), so the signedintegeroverflow-regression mains are NEEDS-HARNESS in bmc | S | src/prism/bmc_encoder.inc:2432-2440 (only the Juliet print helpers are modelled); … |
| D16 | package_archive.py writes an incomplete MANIFEST.json and records the packer's Python version as the minimum | S | docs/SVCOMP.md:656-660; tools/svcomp/package_archive.py:110-119, 62-64 |
| D17 | fm-tools.yml draft does not follow the fm-tools schema, and the tool-info module's project_url() returns None | S | docs/SVCOMP.md:649-656; tools/svcomp/fm-tools.yml:1-22; tools/svcomp/prism.py:46-47 |
| D18 | PRISM_FUNCTION_BUDGET is reset between check_function's two unwind attempts (up to about 2x the budget) | S | docs/PIR.md:1362-1371; src/prism/pir/encode.cpp:1386-1414 |
| D19 | k-induction step queries ignore the remaining function budget | S | docs/PIR.md:1362-1371 ("Each VC query also gets at most what is left"); … |
| D20 | Houdini's final query and per-query floors overrun the function budget | S | docs/PIR.md:1362-1371 and 1113-1116 ("plus at least three for the final query"); … |
| D21 | apply_memory_policy re-checks run without the function budget, the Houdini run budget, the cache or the worker core share | S | docs/PIR.md:1362-1371; src/prism/pir/stage_mem.cpp:194, 214, 237; … |
| D22 | A validated counterexample is discarded as TIMEOUT when the budget runs out during the group-halving search | S | docs/PIR.md:1362-1371; src/prism/pir/encode.cpp:1731-1741, 1766-1770 |
| D23 | A spent budget turns an established BOUNDED into TIMEOUT, contrary to the docs and to the Houdini budget | S | docs/PIR.md:1365-1366 ("once it is spent before every VC is answered the function is … |
| D24 | Budget-capped VC timeouts are reported as a plain UNKNOWN, and cap_timeout changes SolveOptions that other code reuses | S | docs/PIR.md:1367-1371; src/prism/pir/encode.cpp:1433-1436, 1466-1475, 1536, 1804-1810 |
| D25 | Budget env var parsing and messages: invalid values are ignored silently, fractions are truncated, the header comment is wrong, and no test locks the env-var list | S | include/prism/pir.hpp:495-498; src/prism/pir/stage.cpp:882-888; … |
| D26 | C units skip fix-irreducible, so goto into a loop and Duff's device give UNENCODED: irreducible control flow | S | docs/PIR.md:30-36, 170-171; src/prism/pir/stage.cpp:515-517 |
| D27 | pir_vcs (--pir-vcs) silently returns no VCs on encoding failure and does not link the library models | S | docs/SOLVERS.md:282-287; src/prism/pir/encode.cpp:2037-2072; src/prism/pir/bench.cpp:22-65 |
| D28 | fma and libm on half precision are UNENCODED | S | docs/PIR.md:168; src/prism/pir/translate_fp.cpp:297-305 |
| D29 | Taxonomy is out of date with pir: UB-POISON is missing from both engines, and pir's memory classes are not credited | S | src/prism/taxonomy.cpp:714-718 (comment 'No memory model in PIR'); … |
| D30 | STR-NULL-MEMBER misses the real CVE-2023-50472 code: a member null test after the call suppresses it | S | docs/EVALUATION.md:209 (claims STR-NULL-MEMBER finds CVE-2023-50472); … |
| D31 | The three new CVE lints drift between the engines (whitespace in `! acc` and `a -> b`, literal memcpy lengths) | S | CLAUDE.md engine parity; prism/checkers.py:13628-13663, 13740-13786 vs … |
| D32 | PTR-CHAIN-NULL and MEM-COPY-LEN take time quadratic in function length (re-joined prefix text, C++ recompiles 8 regexes on each match) | S | docs/FUZZ_SELF.md F1 (a crafted input must not make a run take minutes); … |
| D33 | The three new lints have only the one twin pair as tests: no testdata/ planted pair, no test_core unit test, no testdata_fp negatives | S | CLAUDE.md 'Adding a check' / testdata planted-bug corpus; tests/test_core.py:333 … |
| D34 | SARIF rules carry only the class id: no taxonomy name or CWE (new lints included) | S | prism/sarif.py:69-70; src/prism/sarif.cpp:103-104 |
| D35 | Open false alarm: MEM-CAPACITY-FIRST when the realloc failure arm frees the whole object (lints and pbsd, both engines) | S | docs/EVALUATION.md:201 (zlib examples/zran.c:110, FP open) |
| D36 | Fuzz reports CLEAN for functions the concrete interpreter cannot parse (ExecResult.error ignored; Law 7) | S | docs/EVALUATION.md:236-240 (interpreter front-end gaps surface as ERROR, not silent); … |
| D37 | The Python engine's fuzz stage lacks the C++ perf fixes: no dedupe of inputs already run, no skip of unchanged-seed rounds | S | docs/EVALUATION.md:117 (fuzz fix: an input already run is not run again; a round with … |
| D38 | PARSE-GAP: macros between the return type/`*` and the name (zlib get_crc_table, gz_strwinerror) | S | docs/EVALUATION.md:88-89 ('+1 honest gap: const z_crc_t FAR * ZEXPORT get_crc_table()'); … |
| D39 | F1 hardening: full_match and about 30 other match-at-offset-0 helpers still use unanchored PCRE2 searches; no F1 regression test; self-fuzz not in CI | S | docs/FUZZ_SELF.md F1 'full_match has the same unanchored-search shape and should get the … |
| D40 | pir/conc lower C with strict -std=c17, which hides POSIX/XSI declarations (pthread_rwlock_t) and drops a compile_commands `-std=gnu*` | S | docs/CONCURRENCY.md:305-307 (pthread_rwlock_* reported NEEDS-HARNESS); … |
| D41 | BMC counterexample format differs between engines; C++ execute stage silently drops every bmc counterexample | S | CLAUDE.md:34-36 (same report shape); CLAUDE.md:47 (Law 7, nothing skipped quietly); … |
| D42 | Python CLI rejects C++ pipeline flags and treats C++ subcommands as a scan path | S | docs/ROADMAP_STATUS.md:36 (D8: Python lists C++-only features, records NOTRUN); … |
| D43 | A non-existent scan PATH is an 'ok' run in both engines (exit 0 under --fail-on defect) | S | CLAUDE.md:47 (Law 7); CLAUDE.md:44 (Law 1, missing is NOTRUN never clean) |
| D44 | contracts stage differs: Python runs LLM Dafny-spec drafting, C++ does not | S | CLAUDE.md:34-36 (parity, same report shape); prism/pipeline.py:383-391 |
| D45 | C++-only triage.json and repair explanation leave no NOTRUN trace in the Python engine | S | docs/ROADMAP_STATUS.md:36 (D8: Python records one NOTRUN row); docs/AI.md:440-453 … |
| D46 | report.json finding.extra value types differ between engines | S | CLAUDE.md:34-36 (same report shape) |
| D47 | Env var inventory: vars read but not in CLAUDE.md's list, a non-PRISM_ var read, and no test locking the list | S | CLAUDE.md:12-20 ('Env vars are PRISM_* only' + list) |
| D48 | Python Config reads PRISM_MODEL / PRISM_LLAMA_SERVER / OLLAMA_HOST once at import time | S | prism/config.py:290-293 vs src/prism/config.cpp:53-63 (default_config reads at call time) |
| D49 | fuse round early-exit exists in C++ only, so CLEAN rows report different round counts | S | docs/ROADMAP_STATUS.md:36 (fixes land in both); src/prism/stages/fuse.cpp:910-914 |
| D50 | Polyglot parity test does not lock per_file / ok_rcs / timeout / cwd_marker / unconfigured / languages / kind | S | CLAUDE.md:62-64 (tests/test_polyglot.py fails if the tables drift) |
| D51 | clang-tidy diagnostics are not parsed into file/line/rule (both engines) | S | CLAUDE.md:62-64 (output parsing is one regex with named groups file line col sev rule msg) |
| D52 | Refinement checker fixtures are stale vs the current C++ translator; add a freshness check and regenerate | S | docs/PROOFS_REFINEMENT.md:494-500 (check.sh runs the checker on fixtures/: "real C++ … |
| D53 | C++: make the poison-phi-edge UB-POISON check edge-specific (currently fires on every exit of the predecessor) | S | docs/PROOFS_REFINEMENT.md:625-636 (finding 1: fixed in translate.cpp; "The Lean … |
| D54 | C++: llvm.lifetime.end on a non-stack object (coroutine frame) frees the whole object | S | docs/PROOFS_REFINEMENT.md:727-729 (finding 7: "Still open: llvm.lifetime.end on a … |
| D55 | tools/pir_lean_check.py silently ignores functions the pir stage did not export (time budget) | S | docs/PROOFS_REFINEMENT.md:464-466 ("a run can leave a few files unexported ... the … |
| D56 | PrismSem: prove the "free variables only" lemma linking Sat over total Env to SMT models | S | docs/PROOFS_SEMANTICS.md:298-301 (gap 3: "It is routine and not yet written") |
| D57 | Ship prism-bitblast/prism-lrat-check with releases, then require Lean's LRAT checker on the Z3-tactics certified path | S | docs/TRUSTED_BASE.md:232 (T5 roadmap target: "Lean's checker required on this path too … |
| D58 | Lock the k-induction step's premises (havoc of every loop-assigned variable) in both engines; the doc describes a design that no longer runs | S | docs/PROOFS_TECHNIQUES.md:575-591 (k-induction gap: facts 1-3 "were not established here") |
| D59 | C++ fuzz stage builds its LLM engine from a blank Config{}, so PRISM_LLAMA_SERVER / OLLAMA_HOST / PRISM_GGUF / PRISM_MODEL are ignored for Fuzz4All/ChatFuzz seeding | S | src/prism/stages/fuse.cpp:1331-1332 |
| D60 | AFL++ is found on PATH only in both engines: --tool afl-fuzz=PATH and the pinned fetch_deps build (~/.prism/tools/aflplusplus/<commit>/bin) are never searched | S | src/prism/stages/fuse.cpp:481-483; prism/afl.py:18-24; CLAUDE.md 'adapters search before … |
| D61 | bitwuzla is pinned in MANIFEST.toml (stages=["bitwuzla"]) but has no VENDOR_DIR entry in either engine | S | CLAUDE.md 'New tool: add a manifest row, and a VENDOR_DIR entry in prism/config.py plus … |
| D62 | C++ CLI ignores unknown flags and --flag=value forms, drops a malformed --tool, crashes on bad numbers, and runs nothing for --stage "" (Python runs everything) | S | src/prism/main.cpp:238-340 |
| D63 | Unknown stage names in --stage/--skip are accepted silently in both engines | S | prism/__main__.py:103-104; src/prism/main.cpp:266-267; Law 7 |
| D64 | Python CLI drifts from the C++ CLI: different fuzz/repair defaults, and the C++-only flags are rejected instead of being recorded for parity | S | prism/__main__.py:40-43,52-63; include/prism/config.hpp:27-31 |
| D65 | Qt GUI drops the pir/review flags it is launched with and can take a flag's value as the scan path | S | src/gui/MainWindow.cpp:32-38 (takes_value), 52-106 (parse_cli_launch) |
| D66 | Python GUI writes to prism-out/ (not prism-out-gui/) and ignores every CLI flag except PATH and --allow-exec | S | CLAUDE.md 'Output directory is prism-out/ (GUI: prism-out-gui/)'; … |
| D67 | Comment stripper drops the /* and */ delimiters: it joins tokens (int/**/x -> intx) and shifts columns (the SV-COMP bmc nondet column bug) | S | docs/SVCOMP.md:619-628 (bmc call-site column off after a block comment); … |
| D68 | C23/C++14 digit separators (1'000, 0x10'00) are taken as character literals: the parser reads comments as code and truncates function bodies silently, and the bmc … | S | prism/cparse.py:222-229 (_STRIP_TOKEN char-literal alternative, re.S); … |
| D69 | --timeout ('solver seconds per query') is ignored by bmc, wp, contracts and harness: Z3 is hard-coded to 8 s (2 s for concolic flips) in both engines | S | docs/USER_GUIDE.md:97; src/prism/bmc_encoder.inc:163; prism/bmc.py:304 |
| D70 | SV-COMP wrapper: valid-memsafety refutations ignore pir's MEM-UAF, MEM-DOUBLE-FREE, MEM-INVALID-FREE and PTR-INVALID-DEREF although the replay maps them | S | tools/svcomp/prism_svcomp.py:105-107; docs/SVCOMP.md:639-641 |
| D71 | pir: thread_local globals are UNENCODED (NEEDS-HARNESS) although pir's single-thread semantics could treat them as ordinary globals | S | src/prism/pir/translate_mem.cpp:439; docs/PIR.md:174 |
| D72 | Leftover no-op code: a dead split_comma line marked 'wrong', a no-op retag_unsigned in both engines, and a dead ternary in apply_confidence | S | src/prism/stages/contracts.cpp:33; src/prism/bmc_encoder.inc:166 / prism/bmc.py:327; … |
| D73 | Wire predict::unwind_for into pir's first-try unwind (off by default: the built-in model has no bound target) | S | docs/AI.md:522-523; src/prism/pir/encode.cpp:1386-1412 (constexpr int kFirstUnwind = 4) |
| D74 | Vacuity audit writes nothing when its result is SAT, unknown, or all clauses are unencodable (Law 7 / header drift) | S | src/prism/ai/assumption_audit.cpp:3-9 ('SAT is recorded, anything the encoder cannot … |
| D75 | prism regress uses sanitizers that cannot see INT-TRUNC or unsigned-wrap findings, and generates non-reproducing tests for CONC-* rows | S | docs/AI.md:539 (atom_bad, wrap_u_local, wrap_u_branch, trunc_bad do not reproduce); … |
| D76 | --pir-drafts gives PROVED-ASSUMING under a harness PRISM drafted itself; the harness stage keeps NEEDS-HARNESS for the same draft | S | docs/AI.md:96-101 (a drafted assumption 'never yields a proof class on its own (Law … |
| D77 | Proof repair has no deterministic half: stored invariants are not re-seeded with templates or loop re-matching when the loop count changes | S | docs/AI.md:355-366; src/prism/ai/proof_repair.cpp:173-183, 464-482 |
| D78 | Concrete interpreter and interval stage treat long/size_t as 32-bit (ILP32), which causes false CRASH/FAILED on LP64 Linux | M | docs/ROADMAP_STATUS.md:145 (6.7) -> docs/EVALUATION.md:236-240 ('concrete interpreter … |
| D79 | pir false alarm on cJSON_CreateNumber: 'call through a null function pointer' through a 3-member function-pointer hooks struct | M | docs/ROADMAP_STATUS.md:145 -> docs/EVALUATION.md:196 ('FP, **open**: global_hooks = { … |
| D80 | pir analyses the functions of one unit sequentially, so single-file hot spots dominate wall time (zlib crc32.c, tinyexpr npr) | M | docs/ROADMAP_STATUS.md:145 (6.7 'pir on zlib is still heavy (698 s wall ...; crc32.c … |
| D81 | std::list is not modelled: the libstdc++.so _List_node_base hooks are UNENCODED calls | M | docs/ROADMAP_STATUS.md:69 (2.6 standard library PARTIAL); docs/PIR.md:1219 … |
| D82 | Clang-AST lints: headers that no parsed unit includes get regex lints only; parse them as standalone header units | M | docs/ROADMAP_STATUS.md:76 ('a header that no parsed unit includes ... are NOTRUN for the … |
| D83 | conc stage refuses relaxed/acquire/release atomics outright; could run SC and report only violations | M | docs/ROADMAP_STATUS.md:71 ('2.6 threads and atomics DONE (SC) — relaxed/acquire/release … |
| D84 | Refinement certificate fragment: add llvm.assume | M | docs/ROADMAP_STATUS.md:126 ('Not in the fragment: ... llvm.assume ...'); … |
| D85 | libc string models: strcat/strncat unbounded contracts stay BOUNDED (Houdini out of time) | M | docs/ROADMAP_STATUS.md:130 ('13/15 _true PROVED-UNBOUNDED ... strcat/strncat BOUNDED, … |
| D86 | Measurement reruns the roadmap marks as not rerun: 4,179-VC portfolio vs Z3 with the Bitwuzla fix and learned scheduler; full certified suite; ESBMC C++ before/after | M | docs/ROADMAP_STATUS.md:83 ('the 4,179-VC comparison has not been rerun with the fix and … |
| D87 | bmc never inlines non-static callees or calls inside if/while/for bodies, so SV-COMP mains that call their own helpers stay NEEDS-HARNESS | M | docs/SVCOMP.md:686-690 (non-static definition not inlined); src/prism/inline.cpp:119-120 … |
| D88 | No time budget for bmc and none in the wrapper, so a slow bmc (nested_6) loses every answer, including pir's | M | docs/SVCOMP.md:457-462 (nested_6 killed at the wall limit), 403; … |
| D89 | bmc rejects a goto into the start of an else branch and a goto to a self-loop label (`STUCK: goto STUCK;`) | M | docs/SVCOMP.md:173-177 ('Anything else ... is NEEDS-HARNESS'); src/prism/bmc_encoder.inc … |
| D90 | The Python engine's bmc reports no nondet trace (extra nondet/nondet_loc/nondet_loc_kind) and does no nondet site tagging, unlike C++ | M | docs/PIR.md:1241-1249 ('the same shape as the bmc stage'); src/prism/bmc.cpp:3240-3248; … |
| D91 | pir's library-model nondeterminism (malloc/calloc/fopen failing) is not in the nondet trace, so such refutations never replay | M | docs/SVCOMP.md:623-625; src/prism/pir/translate.cpp:1296-1300 (`model_stack.empty()` … |
| D92 | PRISM_FUNCTION_BUDGET test coverage is minimal | M | tests/cpp/test_pir_mem.cpp:1016-1038; tests/test_supply_chain.py:304-306 |
| D93 | volatile loads and stores are UNENCODED (this also blocks correct setjmp code that uses volatile locals) | M | docs/PIR.md:173, 913-917; src/prism/pir/ir_parser.cpp:887-903; … |
| D94 | Atomic load/store, atomicrmw, cmpxchg and fence are UNENCODED in pir | M | docs/PIR.md:173; src/prism/pir/ir_parser.cpp:887-903; src/prism/pir/translate.cpp:733 |
| D95 | thread_local / _Thread_local globals and llvm.threadlocal.address are UNENCODED | M | docs/PIR.md:174; src/prism/pir/translate_mem.cpp:439, 320; … |
| D96 | errno is not modelled: __errno_location is an unmodelled call | M | docs/PIR.md:816-817 |
| D97 | Two-field aggregates with floating-point fields ({double,double}, {float,float}, {i32,double}, _Complex) are UNENCODED | M | docs/PIR.md:596-604; src/prism/pir/translate.cpp:270-280 |
| D98 | Pointer arithmetic on a freed object is not checked (GEP has no liveness check) | M | docs/PIR.md:192-193 (known gap: 'pointer arithmetic on an already freed object'); … |
| D99 | Pointer-parameter contract language covers only \valid / \valid_read ranges (no strings, nullable, extern arrays) | M | docs/PIR.md:367-399; src/prism/pir/contracts.cpp:69-71; … |
| D100 | The k-induction step is answered by in-process Z3 alone (no portfolio, no query cache, no certificate) | M | docs/SOLVERS.md:213-214; docs/PIR.md:1971-1973 comment; src/prism/pir/encode.cpp:1077-1111 |
| D101 | Fast-math flags make the instruction UNENCODED | M | docs/PIR.md:815-816; src/prism/pir/translate_fp.cpp:106-111 |
| D102 | Reading an indeterminate local that is only copied (int y = x; y unused) is not checked | M | docs/PIR.md:187-189 |
| D103 | Falling off the end of a non-void C function is never checked (undef return folded away by mem2reg) | M | docs/PIR.md:189-190, 55-57 ('except %retval'); src/prism/pir/stage.cpp:320, 337; … |
| D104 | Non-volatile locals modified between setjmp and longjmp are read as their stored value instead of reported as indeterminate | M | docs/PIR.md:190-192, 913-917 |
| D105 | Every last-field array is exempt from sub-array bounds (flexible-array idiom), including fixed [N>1] arrays | M | docs/PIR.md:190-191; src/prism/pir/translate_mem.cpp:778, 798-815 |
| D106 | MEM-COPY-LEN misses the real CVE-2022-37434 code (zmemcpy with an `extra_max - len` length) | M | docs/EVALUATION.md:211 and :213-216 (all four CVE shapes flagged); … |
| D107 | PTR-CHAIN-NULL raises many false alarms on real code (truthiness tests, Z_NULL, while/?:, assert macros, one row per line) | M | docs/EVALUATION.md:210, 221-243 (FP rate of the new lints not measured); … |
| D108 | Open false alarm: taint TAINT-SINK on memcpy whose length is strlen(src) into a malloc(len + 1) buffer | M | docs/EVALUATION.md:202 (zlib examples/gun.c:689, FP open) |
| D109 | Concrete interpreter (concolic/fuzz/execute) rejects 64-bit locals and ULL literals | M | docs/EVALUATION.md:236-240 (unsigned long long locals, ULL literals are PRISM gaps) |
| D110 | bmc and the concrete interpreter reject unsized array initialisers, 2-D array initialisers and digit separators | M | docs/CONFORMANCE.md:983-989 (G1/G2 coverage gaps); docs/EVALUATION.md:167-169 (Unity … |
| D111 | PARSE-GAP: C++ shapes from cxxopts/Catch2 (26 gaps, identical in both engines) | M | docs/EVALUATION.md:136 (cxxopts PARSE-GAP 76 -> 26, remaining not addressed) |
| D112 | conc: pthread_rwlock_* are not modelled | M | docs/CONCURRENCY.md:303-307 ('Not modelled: pthread_rwlock_*'); … |
| D113 | Solver portfolio / certified checker lookup ignores PRISM_TOOLS_DIR, the pinned manifest commit and --tool | M | docs/SUPPLY_CHAIN.md:82 (installs into ~/.prism/tools/<name>/<commit>/, override with … |
| D114 | Prove the fptrunc FLOAT-OVERFLOW condition in Lean (format conversion) and lock the C++ text | M | docs/PROOFS_REFINEMENT.md:572-579 ("the fptrunc condition is the same shape, but it is … |
| D115 | Model clang UB markers __prism.folded / __prism.poison.iN in the Lean extended fragment | M | docs/PROOFS_REFINEMENT.md:339-342 ("Not covered here: ... clang-folded UB markers … |
| D116 | Model llvm.assume (check then assume) in the Lean fragment | M | docs/PROOFS_REFINEMENT.md:730-745 (finding 8: "The Lean translator still refuses … |
| D117 | Lean: accept poison phi inputs (check on the edge) once the C++ check is edge-specific | M | docs/PROOFS_REFINEMENT.md:632-636 |
| D118 | Model the libc-model intrinsics __prism_obj_size, __prism_memcpy (C memcpy rule), __prism_memset, __prism_check in the Lean fragment (realloc, strcpy-style models) | M | docs/PROOFS_REFINEMENT.md:774-775 ("Not yet: realloc (__prism_obj_size, … |
| D119 | Run pir_lean_check inside the pir stage and attach a per-function refinement verdict (per-run translation validation) | M | docs/TRUSTED_BASE.md:10-12 ("per-run translation validation ... planned"), :228 (T1 … |
| D120 | CI never runs the end-to-end C++-vs-Lean correspondence or the formal-semantics-vs-lli test | M | docs/PROOFS_REFINEMENT.md:379-383, 502-513 (tools/llvm_sem_vs_lli.py results are from a … |
| D121 | PrismSem: prove the VC is unchanged under SMT-LIB division-by-zero semantics | M | docs/PROOFS_SEMANTICS.md:302-307 (gap 4: "That is argued here, not proved") |
| D122 | Build PRISM with GCC as well as Clang (trusted-base mitigation for the C++ compiler) | M | docs/TRUSTED_BASE.md:281 ("Build with Clang and GCC and cross-check results on the … |
| D123 | bmc front end (both engines) gives ERROR on 2-D arrays with initialisers | M | docs/CONFORMANCE.md:983-985 (G1) |
| D124 | Self-fuzzing covers only cparse, the IR parser and the report/journal readers, and is not run in CI | M | docs/FUZZ_SELF.md:145-151 ('Not covered yet') |
| D125 | fetch_deps has no build recipe for most pinned tools (cppcheck, cbmc, esbmc, aflplusplus, klee, ...): the NOTRUN install hint only fetches source | M | scripts/fetch_deps.py:284-357,428-433; third_party/MANIFEST.toml build_hint rows |
| D126 | Compound decreases measures (n - i, abs(n)) are ERROR in both engines; only a bare identifier is encoded. C++ now encodes linear measures (identifiers and constants joined by + / -): countdown_complex is FAILED in C++, still ERROR in Python (intentional; do not freeze the Python verdict). `*`, calls and abs stay ERROR | M | src/prism/stages/contracts.cpp:20-29; prism/contracts.py:339-341; … |
| D127 | Proof store re-checks pir artefacts with the legacy textual engines and hides proof-rank drops as 'reused' | M | docs/AI.md:345-371; src/prism/ai/proof_repair.cpp:170-225 (recheck_artefact), 353-356 … |
| D128 | GUI parity: the C++ GUI has no stage table or NOTRUN list; the Python GUI has no finding ids or double-click explain and writes to prism-out instead of prism-out-gui | M | CLAUDE.md 'Output directory is prism-out/ (GUI: prism-out-gui/)'; docs/AI.md:490-494; … |
| D129 | Template harness draft refuses non-int element buffers even when pir, which has a byte-level memory model, consumes it | M | docs/AI.md:104-107 and 211 (45 of the 67 uncleared NEEDS-HARNESS are char/void/typedef … |
| D130 | conc stage does not model pthread_rwlock_*, semaphores, spin locks, barriers, pthread_once, pthread_detach/self, pthread_cond_timedwait or mtx_timedlock | M | docs/CONCURRENCY.md:307-309 ('pthread_rwlock_*, the scull driver model and qrcu-1 ... … |
| D131 | fuzz, concolic, wp and rapid refuse every POINTER function even when the harness stage can materialise an honest buffer from its // requires: | M | src/prism/stages/harness_bmc.cpp:83-137 (materialize, used only by run_harness_bmc); … |
| D132 | std::unordered_map/unordered_set not modelled (libstdc++.so _Prime_rehash_policy / _Hash_bytes) | L | docs/ROADMAP_STATUS.md:69 ('iostreams, unordered containers, libc++ ... not modelled'); … |
| D133 | PIR floating-point formats: bfloat, x86_fp80 (long double) and fp128 are UNENCODED | L | docs/ROADMAP_STATUS.md:70 ('Still UNENCODED: long double, fp128 and fast-math'); … |
| D134 | Clang-AST flow checks are straight-line only: add path merging across branches and loops | L | docs/ROADMAP_STATUS.md:76 ('flow checks are straight-line only (no path merging across … |
| D135 | Clang-AST lints skip template-dependent code; check implicit instantiations instead | L | docs/ROADMAP_STATUS.md:76 ('code that depends on a template parameter is not checked'); … |
| D136 | Refinement certificate fragment: libc models beyond alloc/free (realloc: __prism_obj_size/__prism_memcpy; FILE: __prism_read_range/__prism_check/__prism_havoc_bytes, … | L | docs/ROADMAP_STATUS.md:126 ('realloc/FILE models'); :159 (M9 'realloc/FILE ... not yet') |
| D137 | Refinement certificate fragment: ptrtoint pointer differences, undef outside freeze/store, symbolic-size contract parameters, FP instructions | L | docs/ROADMAP_STATUS.md:126 ('Not in the fragment: ... symbolic-size contracts, ptrtoint, … |
| D138 | Lean: state and prove the k-induction frame lemma over PrismSem/Memory.lean | L | docs/ROADMAP_STATUS.md:158 (M8: 'the frame lemma for PIR memory (writes only inside the … |
| D139 | Lean: prove the pir frem/fmod-from-IEEE-remainder encoding (and sqrt/fma rounding) | L | docs/ROADMAP_STATUS.md:128 ('Not proved: other rounding modes, frem/sqrt/fma/libm'); … |
| D140 | bmc does not model file-scope variables (reducer tasks' __return_main), so any use is NEEDS-HARNESS | L | docs/SVCOMP.md:397-400 ('the reducers' __return_main global'); … |
| D141 | bmc and pir stop at the first FAILED property of main, so a reachable reach_error()/abort() blocks a no-overflow proof, and vice versa | L | docs/SVCOMP.md:224-228 (nested_1b −2), 404-406 (byte_add-1, modulus-2, id_trans); … |
| D142 | The SV-COMP archive cannot run on a machine without clang-18/opt-18 (and optional solvers): nothing is bundled or declared, and the wrapper cannot use bundled tools | L | docs/SVCOMP.md:649-662 ('Still missing: clang/opt bundling'); tools/svcomp/README.md:19-25 |
| D143 | The no-data-race property is always unknown, although the conc stage finds races | L | docs/SVCOMP.md:638-642; tools/svcomp/prism_svcomp.py:82 (STAGES excludes conc), 109-113 … |
| D144 | Recursion and inline depth limits make the function UNENCODED instead of giving a bounded check with a depth cut | L | docs/PIR.md:160-161; src/prism/pir/translate.cpp:1372-1375 |
| D145 | Common libc functions have no models (ctype, strstr, strspn/strcspn, strtoll/strtod, qsort/bsearch, scanf/v*printf, stream positioning) | L | docs/PIR.md:417-430; docs/ROADMAP_STATUS.md:130 |
| D146 | ptrtoint other than pointer differences, and inttoptr other than 0, are UNENCODED (alignment tests, pointer hashing) | L | docs/PIR.md:172-173; src/prism/pir/translate.cpp:1666-1692 |
| D147 | Memory leaks (valid-memtrack / valid-memcleanup) are not checked by pir | L | docs/PIR.md:192; docs/SVCOMP.md:49-52, 641 |
| D148 | const_cast refusal is a text match over the whole C++ function instead of modelling const objects | L | docs/PIR.md:184-186; src/prism/pir/stage.cpp:1000-1020 |
| D149 | C++20 named modules: .cppm/.ixx units are not pir units and importing units fail to compile | L | docs/PIR.md:1222 (Modules: NOT TESTED) |
| D150 | C++ library models missing for std::list and unordered containers (out-of-line libstdc++.so calls) | L | docs/PIR.md:1219 (Standard library PARTIAL); docs/ROADMAP_STATUS.md:69 |
| D151 | Houdini templates cannot prove strcat/strncat (4-loop string models stay BOUNDED) | L | docs/PIR.md:1146-1151; docs/ROADMAP_STATUS.md:130 |
| D152 | Model noreturn error/exit calls in the Lean fragment: __assert_fail family, reach_error, llvm.trap, glibcxx assert, abort (OOM flag), exit/_Exit/quick_exit | L | docs/PROOFS_REFINEMENT.md:339-341, :758-768, :774-777 ("Not yet: ... abort() (it reads … |
| D153 | Lean fragment: variable-length arrays (variable-size alloca) and llvm.stacksave/stackrestore | L | docs/PROOFS_REFINEMENT.md:781-782 ("variable-size alloca ... … |
| D154 | Lean PIR `revive` statement so llvm.lifetime.start is back in the fragment | L | docs/PROOFS_REFINEMENT.md:716-727 (finding 7: "The Lean PIR has no such statement, so … |
| D155 | Lean model of frem (fmod via IEEE remainder), fma/fmuladd rounding, and PRISM's FP checks for them | L | docs/PROOFS_REFINEMENT.md:566-568 ("Not covered: ... frem, sqrt, fma/fmuladd and libm … |
| D156 | pir: any recursive call makes the whole function NEEDS-HARNESS (no bounded recursion unrolling) | L | src/prism/pir/translate.cpp:1372-1375; docs/PIR.md:161,507-509 |
| D157 | conc stage: std::thread/std::jthread, thread arguments, and dynamic or nested thread creation are UNENCODED | L | src/prism/conc/extract.cpp:873-889 (unsupported_threading), 750-760 (thread argument), … |
| D158 | prism regress cannot build tests for pointer-parameter findings (the pir counterexample has no object contents) | L | docs/AI.md:434-436; src/prism/ai/regress.cpp:572-577; src/prism/pir/encode.cpp:1795-1801 |
| D159 | Textual loop cut (bmc-stage Houdini) refuses nested loops, break/continue/switch, calls and pointer declarations | L | docs/AI.md:44-50; src/prism/ai/invariants.cpp:14-16, 274-276 |
| D160 | k-induction and Houdini are not attempted for loops that allocate, free or restore the stack | XL | docs/ROADMAP_STATUS.md:70 ('Loops that allocate or free stay BOUNDED (not attempted)'); … |
| D161 | Certified mode: wide-multiplication VCs (fn_macro_true, long_mul_true) are not certified within budget; FP VCs are not certifiable | XL | docs/ROADMAP_STATUS.md:86 ('fn_macro_true / long_mul_true (wide multiplications) are not … |
| D162 | Lean: concurrency slot encoding and race/deadlock monitors are unproved | XL | docs/ROADMAP_STATUS.md:129 ('Not proved: the Z3 slot encoding, the race/deadlock … |
| D163 | Lean: no theorem that the translator mirror translateX always yields a valid certificate | XL | docs/ROADMAP_STATUS.md:126 ('No theorem that translateX always yields a valid … |
| D164 | D2: Clang/LLVM runs as a subprocess (clang, opt), not linked as a library | XL | docs/ROADMAP_STATUS.md:30 (D2 PARTIAL: 'Clang runs as a process, not linked as a library') |
| D165 | No ILP32 data model in the engines, so width-dependent ILP32 tasks are answered unknown before analysis | XL | docs/SVCOMP.md:643-648, 384-389 (64 of the 128 unknown answers); … |
| D166 | bmc has no floating-point encoding: __VERIFIER_nondet_float/double and float variables are unencoded | XL | docs/SVCOMP.md:629-630; src/prism/bmc_encoder.inc:2388-2400 (NONDET table has no … |
| D167 | valid-memsafety can never be true: no stage encodes valid-memtrack or valid-memcleanup | XL | docs/SVCOMP.md:49-52, 638-642; tools/svcomp/prism_svcomp.py:105-107 |
| D168 | Correctness witnesses are empty for pir plain k-induction and bounded proofs (no invariant exported) | XL | docs/SVCOMP.md:128-132, 632-637; src/prism/pir/encode.cpp:1996 (k_induction=closed … |
| D169 | Integers wider than 64 bits and 80/128-bit floating point (x86_fp80, fp128) are UNENCODED | XL | docs/PIR.md:166-169, 795; src/prism/pir/translate.cpp:53-55; … |
| D170 | ILP32 targets: pir is hard-coded to LP64 and 64-bit pointers | XL | docs/SVCOMP.md:643-648 |
| D171 | conc: shared non-scalar data (heap, arrays, structs, pointers, non-global mutexes, thread arguments) is not connected to the PIR memory model that has since landed | XL | docs/CONCURRENCY.md:112-118 and 'Memory model hook' (Rewriter::shared_var is the single … |
| D172 | Prove that translateX always yields a certificate validB accepts (remove the per-run certificate premise) | XL | docs/PROOFS_REFINEMENT.md:790-802 ("there is no theorem that translateX always produces … |
| D173 | Extend the PrismSem memory encoder model to what the C++ Bv encoding does (multi-byte little-endian access, per-byte init shadow, alloc kinds/alignment, 16/48 pointer … | XL | docs/PROOFS_SEMANTICS.md:329-343 (modelling simplifications); docs/PIR.md:361-365 ("Not … |
| D174 | No ILP32 data model: pir and bmc are LP64 only, so SV-COMP ILP32 tasks with width-dependent types are unknown | XL | docs/SVCOMP.md:384-389,642-647; tools/svcomp/prism_svcomp.py:117 (WIDTH_TYPES) |
| D175 | conc stage still refuses heap, arrays, structs, pointer-typed shared data and used thread arguments 'until the PIR memory model lands', but that memory model is DONE … | XL | src/prism/conc/extract.cpp:20-23 and :310-311 ('only scalar integer globals until the … |
| D176 | pir not scored on NIST Juliet (only bmc) | M (blocked) | docs/ROADMAP_STATUS.md:154 (M4 'Juliet scored for bmc'); docs/CONFORMANCE.md:266-279 |
| D177 | In-process GGUF backend has no grammar sampler, so AI features never use it | M (blocked) | docs/AI.md:146-148; src/prism/ai/core.cpp:646-648; src/prism/stages/llm.cpp:154-156 |
| D178 | The solver process runner is not implemented on Windows, so every external portfolio member (Bitwuzla, CaDiCaL, Kissat, cake_lpr) fails there | M (blocked) | src/prism/solver/util.cpp:214-218 ('process runner not implemented on Windows') |
