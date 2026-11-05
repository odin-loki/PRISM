# PRISM — read this first

## The name is PRISM

This project is **PRISM** (Performance, Regression, Integration and Security
Module). It is a code checker: one command that finds every error it can in a
code base and says honestly what it could not check.

- The project was once called **Helix**. That name is **retired**. Do not use
  it in code, comments, docs, env vars, file names, commit messages or output.
  If you find "Helix"/"helix"/"HELIX" anywhere outside `third_party/`, it is a
  leftover: rename it to PRISM (`tests/cpp/test_naming.cpp` fails on it).
- Env vars are `PRISM_*` only. There are no `HELIX_*` fallbacks. The full
  list, one line each, is `prism::env_vars()` in `src/prism/config.cpp`.
  `tests/cpp/test_env_registry.cpp` fails on a `PRISM_*` getenv that is not
  in it. The user guide's table is generated from it
  (`scripts/gen_doc_tables.sh`).
- Output directory is `prism-out/` (GUI: `prism-out-gui/`).

## Engine

PRISM is C++23 only. There is no Python engine.

| | |
|---|---|
| Source | `src/prism/*.cpp`, `src/prism/<area>/`, `include/prism/*.hpp` |
| Run | `./build/prism PATH` |
| Tests | `tests/cpp/` → `prism_tests` (doctest) |
| GUI | `src/gui/` (Qt 6, `prism_gui`) |
| Design | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); status in [docs/STATUS.md](docs/STATUS.md) |

Stage order is `include/prism/pipeline.hpp` (`STAGE_ORDER`). The status
vocabulary is `include/prism/laws.hpp`. The report shape is locked by the
doctests in `tests/cpp/`.

The only tracked Python outside `third_party/` is `tools/svcomp/prism.py`
(the BenchExec tool-info plugin) and the CI bootstrap scripts
`scripts/fetch_llvm.py` and `scripts/fetch_toolchain.py`.
`tests/cpp/test_deps.cpp` fails on any other tracked `.py`.

## Laws (never break these)

1. A missing tool is `NOTRUN`, never a clean result.
2. `PROVED` and `BOUNDED` are never merged.
3. Fuzzer `CLEAN` is not a proof.
4. LLM output is `HYPOTHESIS` / `READS`; it cannot cover a defect class.
5. Confidence = visibility × answer × resolution; no data scores 0.
6. Pointer-parameter functions are not model-checked unguarded (`NEEDS-HARNESS`).
7. A stage that cannot run writes that down. Nothing is skipped quietly.
8. Never pass a flag that silently disables a check.
9. Executing scanned code requires `--allow-exec` (`Config.allow_exec`, default
   off) and runs only in the bwrap jail (read-only /usr, toolchains, minimal
   /etc; no home, no network). Where bwrap cannot run, the step is NOTRUN
   unless `--allow-exec=unsandboxed` (`Config.allow_unsandboxed`) accepts
   resource limits only (rlimits / Windows Job Object).

For Law 9: without the flag, a step that runs scanned code (or code the model
wrote) writes the `sandbox::exec_notrun` row (`src/prism/sandbox.cpp`). The
sanitize stage calls only `// prism: run` functions. A new stage that runs
code goes in `exec_stages()` (`src/prism/pipeline.cpp`); a new polyglot tool
that runs code gets `executes = true` in its `src/prism/polyglot.cpp` row. The
per-stage table is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#running-on-untrusted-code-law-9).

The laws are proved for the verdict lattice in `proofs/Prism/Verdict.lean`.
[docs/VERDICTS.md](docs/VERDICTS.md) defines every verdict.

## Adding a check

- **New C/C++ lint:** a regex rule in `src/prism/checkers_*.cpp` and, where
  the AST can decide it, a check in `src/prism/astlint_checks.cpp` (an AST
  row supersedes the regex row on the same line and class). Add the class
  to the taxonomy in `src/prism/taxonomy.cpp`. Test it with a planted file in
  `testdata/` and a case in `tests/cpp/test_checkers_c.cpp`,
  `test_checkers_cxx.cpp` or `test_astlint.cpp` (which runs every case on
  both AST front ends).
- **New language or linter:** add a `Check`/`Tool` row to
  `src/prism/polyglot.cpp`; `tests/cpp/test_polyglot.cpp` tests it. Output
  parsing is one regex with named groups `file line col sev rule msg`.
- **New stage:** add it to `STAGE_ORDER` in `include/prism/pipeline.hpp` and
  call it from `run_pipeline` (`src/prism/pipeline.cpp`) in the same
  position. Describe it in the stage table of `docs/ARCHITECTURE.md` and run
  `scripts/gen_doc_tables.sh` (the user guide's stage-order sentence is
  checked against `STAGE_ORDER` by `tests/cpp/test_qa.cpp`).
- **New external tool:** add a `[[component]]` row to
  `third_party/MANIFEST.toml` and an entry in `vendor_dir_for` in
  `src/prism/config.cpp`. A missing tool is `NOTRUN` with the
  `prism-deps tool NAME` hint. See [docs/SUPPLY_CHAIN.md](docs/SUPPLY_CHAIN.md).
- **New env var:** add it to `prism::env_vars()` and run
  `scripts/gen_doc_tables.sh`.
- **New CLI flag:** document it in `docs/USER_GUIDE.md` inside backticks;
  `tests/cpp/test_qa.cpp` fails on a `--help` flag the guide does not name.
- **Reports:** `report.json`, `report.md`, `report.sarif`
  (`src/prism/sarif.cpp`). `tests/cpp/test_sarif.cpp` locks the SARIF export
  and `--fail-on`; `tests/cpp/test_cli.cpp` runs the binary with
  `--fail-on defect`. `docs/VERDICTS.md` and `docs/TRUSTED_BASE.md` are
  embedded in the binary at configure time and shipped with every report;
  tests compare them byte for byte, so reconfigure after editing them.

## Build and test

```
L=/path/to/llvm-23.1.4/bin     # python3 scripts/fetch_llvm.py prints it
cmake -B build -G Ninja -DCMAKE_C_COMPILER=$L/clang -DCMAKE_CXX_COMPILER=$L/clang++ \
  -DPRISM_LLVM_PINNED_BIN=$L -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build          # first build compiles vendored Z3 (slow)
./build/prism_tests
```

- The compiler is the pinned LLVM 23.1.4 (`third_party/llvm/PIN.toml`).
  System clang 18 cannot build FORGE.
- FORGE (odin-loki/FORGE) is linked and not vendored. Pass
  `-DFORGE_SOURCE_DIR=PATH` or let CMake fetch it. Branch port/cipin
  (landing) pins it to the commit in `cmake/FORGE_PIN`; bump it with
  `scripts/bump_forge_pin.sh`.
- A test that cannot run its check here must say `NOTRUN`, not pass
  silently. Branch port/testharness (landing) adds
  `prism_test::notrun(reason)` in `tests/cpp/test_support.hpp`; the run
  prints the count, and `PRISM_TESTS_REQUIRE_TOOLS=1` turns each one into a
  failure.
- Docs checks: `prism_docs_check anchors` and `prism_docs_check
  assurance-check` (`.github/workflows/docs.yml`), and the `docs:` doctests
  in `tests/cpp/test_qa.cpp`.
- Stricter warnings: `-DPRISM_WERROR=ON`. The C++ standard is
  [docs/CXX_STANDARD.md](docs/CXX_STANDARD.md); the root `.clang-tidy`
  covers the engine sources.

**OneDrive / Desktop clone:** fine for editing; **do not** run `cmake`/`ninja`
there. It is slow and flaky. **WSL:** build in a clone on ext4 (`~/PRISM`),
not under `/mnt/c`. **Windows MSVC:** build under `C:\Temp\prism-build-*`;
see [AGENTS.md](AGENTS.md) and [docs/MSVC_PORT.md](docs/MSVC_PORT.md).

`third_party/` holds only the four linked libraries (Z3, PCRE2, nlohmann/json,
doctest), pinned in `third_party/MANIFEST.toml`, plus the LLVM pin
`third_party/llvm/PIN.toml` and the toolchain pins
`third_party/TOOLCHAIN_PINS.toml`. Never edit them (`prism-deps linked`
checks their tree digest) and never glob them from CMake. External tools are
not vendored: `prism-deps tool NAME` builds the pinned commit into
`~/.prism/tools/<name>/<commit>/`, which adapters search before `PATH`.
Linked components must stay permissive (`prism-deps licence-check`, in CI).

## Layout

- `src/prism/` engine; `src/prism/stages/` one .cpp per stage plus shared
  `common`/`interp`/`llm`/`platform`; `src/prism/pir/` model checker;
  `src/prism/conc/` threads; `src/prism/solver/` solver portfolio;
  `src/prism/ai/` model features; `src/prism/verdict/` verdict lattice;
  `src/prism/svcomp/` SV-COMP front end
- `include/prism/` headers
- `src/gui/` Qt GUI; `src/cuda/` CUDA kernels
- `src/tools/qa/` (`prism-qa`: conformance, soundness, docs checks, ...);
  `src/tools/prism_deps/` (`prism-deps`); `src/tools/prism_ai/` (`prism_ai`)
- `tests/cpp/` doctest suite; `testdata/` planted-bug corpus and conformance
  tasks
- `proofs/` Lean 4 proofs
- `docs/` design docs: start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
  and [docs/STATUS.md](docs/STATUS.md); [docs/MINED.md](docs/MINED.md) says
  what PRISM took from each tool it learned from
- `scripts/` build, fetch and maintenance helpers; `tools/` one-shot
  generators and the SV-COMP plugin
- `.github/workflows/` CI: `ci.yml` (build, `prism_tests`, Windows jobs,
  PRISM on PRISM), `conformance.yml` (release gate), `proofs*.yml`,
  `docs.yml`, `self-check.yml` (nightly sanitizers), `release.yml`
