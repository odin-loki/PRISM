# PRISM — read this first

## The name is PRISM

This project is **PRISM** (Performance, Regression, Integration and Security
Module). It is a code checker: one command that finds every error it can in a
code base and says honestly what it could not check.

- The project was once called **Helix**. That name is **retired**. Do not use
  it in code, comments, docs, env vars, file names, commit messages or output.
  If you find "Helix"/"helix"/"HELIX" anywhere outside `third_party/`, it is a
  leftover: rename it to PRISM (`tests/cpp/test_naming.cpp` fails on it).
- Env vars are `PRISM_*` only (`PRISM_AFL`, `PRISM_LIBFUZZER`, `PRISM_PBSD`,
  `PRISM_GGUF`, `PRISM_MODEL`, `PRISM_LLAMA_SERVER`, `PRISM_NATIVE_DLL` (only
  when `libprism_native` is built with `-DPRISM_NATIVE=ON`),
  `PRISM_TOOLS_DIR`, AI: `PRISM_PROVER_SERVER`, `PRISM_PROVER_GGUF`,
  `PRISM_PROOF_CACHE`, `PRISM_EMBED_SERVER`, `PRISM_TRIAGE`,
  `PRISM_SOLVER_MODEL`, `PRISM_SOLVER_PREDICT`, `PRISM_PREDICT_COLLECT`,
  `PRISM_PREDICT_BUDGET`; certified mode: `PRISM_CERT_CACHE_MAX`,
  `PRISM_CHECKER_MEM`, `PRISM_CERTIFY_BUDGET`; pir: `PRISM_FUNCTION_BUDGET`,
  `PRISM_HOUDINI_BUDGET`).
  There are no `HELIX_*` fallbacks.
- Output directory is `prism-out/` (GUI: `prism-out-gui/`).

## Engine

| | C++ |
|---|---|
| Source | `src/prism/*.cpp`, `include/prism/*.hpp` |
| Run | `./build/prism PATH` |
| Tests | `tests/cpp/` → `prism_tests` |
| GUI | `src/gui/` (Qt 6, `prism_gui`) |

PRISM runs on the **C++ engine** (`src/prism/`). Stage order is
`include/prism/pipeline.hpp` (`STAGE_ORDER`); status vocabulary and report
shape are locked by doctest (`tests/cpp/`) and `prism/laws.hpp`. The only
tracked Python outside `third_party/` is `tools/svcomp/prism.py` (BenchExec
tool-info plugin).

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
   off). Without it any step that runs code from the scanned tree or the LLM
   is `NOTRUN` with the hint (`prism/sandbox.py` / `src/prism/sandbox.cpp`
   `exec_notrun`); with it binaries run in the sandbox (bwrap + rlimits).
   sanitize calls only `// prism: run` functions. New exec-type tools get
   `executes=True` in the polyglot table; new exec stages go in `EXEC_STAGES`
   (both engines). Table: `docs/PLAN.md` "Running on untrusted code".

Status vocabulary lives in `prism/laws.py` and `include/prism/laws.hpp`.

## Adding a check

- New C/C++ lint: `prism/checkers.py` + `src/prism/checkers_*.cpp`, and a
  taxonomy class in both `prism/taxonomy.py` and `src/prism/taxonomy.cpp`.
- New language or linter: add a `Check`/`Tool` row to `prism/polyglot.py`
  **and** the same row to `src/prism/polyglot.cpp`; `tests/test_polyglot.py`
  fails if the tables drift. Output parsing is one regex with named groups
  `file line col sev rule msg`.
- New stage: `prism/pipeline.py:STAGE_ORDER` and `include/prism/pipeline.hpp`
  in the same position, called from both `Pipeline.run` and `run_pipeline`.
- Reports: `report.json`, `report.md`, `report.sarif` (`prism/sarif.py`,
 `src/prism/sarif.cpp`). `tests/cpp/test_sarif.cpp` locks SARIF export and
 `--fail-on`; `tests/cpp/test_cli.cpp` runs the binary with `--fail-on defect`.

## Build and test

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build          # first build compiles vendored Z3 (slow)
./build/prism_tests
```

**OneDrive / Desktop clone:** fine for editing; **do not** run `cmake`/`ninja` or
keep `build-wsl/` there — it is slow and flaky. **WSL:** sync then build on ext4
(`scripts/wsl_sync_from_windows.sh`, then `cd ~/PRISM && scripts/wsl_build.sh`).
**Windows MSVC:** `C:\Temp\prism-build-*`, not OneDrive — see `AGENTS.md` and
`docs/MSVC_PORT.md`.

**C++ standard:** [docs/CXX_STANDARD.md](docs/CXX_STANDARD.md) (ISO C++23, memory
safety target). Stricter warnings: `-DPRISM_WERROR=ON` (Clang/GCC). Root `.clang-tidy`
for engine sources.

`third_party/` holds only the six linked libraries (z3, pcre2, xsimd,
nlohmann, doctest, llama.cpp; ~140 MB), pinned in `third_party/MANIFEST.toml`.
Never edit them (`prism-deps linked` checks their tree digest), never
glob them from CMake. External tools are not vendored:
`prism-deps tool NAME` builds the pinned commit into
`~/.prism/tools/<name>/<commit>/`, which adapters search before PATH. Linked
components must stay permissive (`prism-deps licence-check`, in CI). New tool:
add a manifest row, and a `VENDOR_DIR` entry in `prism/config.py` plus the same
entry in `src/prism/config.cpp`. See `docs/SUPPLY_CHAIN.md`.

## Layout

- `src/prism/` C++ engine; `src/gui/` Qt GUI; `src/cuda/` CUDA havoc
- `src/prism/stages/` one .cpp per analysis stage + shared `common`/`interp`/`llm`/`platform` (internal headers)
- `include/prism/` C++ headers
- `tests/` Python tests (+ `tests/cpp/` doctest suite)
- `testdata/` planted-bug corpus both engines run on
- `docs/PLAN.md` pipeline design; `docs/MINED.md` what was mined from each tool
  (the mined source trees were deleted); `docs/SUPPLY_CHAIN.md` pins, SBOM, releases
- `src/tools/prism_deps/` (`prism-deps`); `src/tools/qa/` (`prism-qa conformance`, …);
  `tools/` one-shot generators; `scripts/` local build/smoke helpers, `rewrite_history.sh`
- `.github/workflows/ci.yml` builds the C++ engine, runs both suites, and
  uploads a SARIF self-check of this repo
