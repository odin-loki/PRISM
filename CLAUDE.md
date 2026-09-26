# PRISM — read this first

## The name is PRISM

This project is **PRISM** (Performance, Regression, Integration and Security
Module). It is a code checker: one command that finds every error it can in a
code base and says honestly what it could not check.

- The project was once called **Helix**. That name is **retired**. Do not use
  it in code, comments, docs, env vars, file names, commit messages or output.
  If you find "Helix"/"helix"/"HELIX" anywhere outside `third_party/`, it is a
  leftover: rename it to PRISM (`tests/test_naming.py` fails on it).
- Env vars are `PRISM_*` only (`PRISM_AFL`, `PRISM_LIBFUZZER`, `PRISM_PBSD`,
  `PRISM_GGUF`, `PRISM_MODEL`, `PRISM_LLAMA_SERVER`, `PRISM_NATIVE_DLL`,
  `PRISM_TOOLS_DIR`, AI: `PRISM_PROVER_SERVER`, `PRISM_PROVER_GGUF`,
  `PRISM_PROOF_CACHE`, `PRISM_EMBED_SERVER`, `PRISM_TRIAGE`,
  `PRISM_SOLVER_MODEL`, `PRISM_SOLVER_PREDICT`, `PRISM_PREDICT_COLLECT`,
  `PRISM_PREDICT_BUDGET`; certified mode: `PRISM_CERT_CACHE_MAX`,
  `PRISM_CHECKER_MEM`, `PRISM_CERTIFY_BUDGET`; pir: `PRISM_FUNCTION_BUDGET`,
  `PRISM_HOUDINI_BUDGET`).
  There are no `HELIX_*` fallbacks.
- Output directory is `prism-out/` (GUI: `prism-out-gui/`).

## One product, two engines

| | C++ engine (primary) | Python engine (reference) |
|---|---|---|
| Source | `src/prism/*.cpp`, `include/prism/*.hpp` | `prism/*.py` (package `prism`) |
| Run | `./build/prism PATH` | `python -m prism PATH` |
| Tests | `tests/cpp/test_main.cpp` → `prism_tests` | `tests/test_*.py` (pytest / unittest) |
| GUI | `src/gui/` (Qt 6, `prism_gui`) | `prism/gui.py` (PySide6) |

Both are PRISM. In comments and tests, "the Python engine" means `prism/*.py`
and "C++" means `src/prism/`. The two must stay in parity: same stage order
(`prism/pipeline.py:STAGE_ORDER` == `include/prism/pipeline.hpp`), same status
vocabulary, same report shape. Many Python tests read the C++ sources to lock
that parity, so when you change one engine, change the other.

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
  `src/prism/sarif.cpp`). `tests/test_sarif.py` runs both engines on one tree
  and compares the SARIF when a C++ binary is available (`PRISM_BIN`).

## Build and test

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build          # first build compiles vendored Z3 (slow)
./build/prism_tests
PRISM_BIN=build/prism python -m pytest tests   # ~10 minutes; PRISM_BIN enables engine parity
```

`third_party/` holds only the six linked libraries (z3, pcre2, xsimd,
nlohmann, doctest, llama.cpp; ~140 MB), pinned in `third_party/MANIFEST.toml`.
Never edit them (`fetch_deps.py --linked` checks their tree digest), never
glob them from CMake. External tools are not vendored:
`python scripts/fetch_deps.py --tool NAME` builds the pinned commit into
`~/.prism/tools/<name>/<commit>/`, which adapters search before PATH. Linked
components must stay permissive (`scripts/licence_check.py`, in CI). New tool:
add a manifest row, and a `VENDOR_DIR` entry in `prism/config.py` plus the same
entry in `src/prism/config.cpp`. See `docs/SUPPLY_CHAIN.md`.

## Layout

- `prism/` Python engine; `prism/cocci/` shipped Coccinelle rules
- `src/prism/` C++ engine; `src/gui/` Qt GUI; `src/cuda/` CUDA havoc
- `src/prism/stages/` one .cpp per analysis stage + shared `common`/`interp`/`llm`/`platform` (internal headers)
- `include/prism/` C++ headers
- `tests/` Python tests (+ `tests/cpp/` doctest suite)
- `testdata/` planted-bug corpus both engines run on
- `docs/PLAN.md` pipeline design; `docs/MINED.md` what was mined from each tool
  (the mined source trees were deleted); `docs/SUPPLY_CHAIN.md` pins, SBOM, releases
- `tools/` one-shot generators; `scripts/` local build/smoke helpers,
  `fetch_deps.py`, `licence_check.py`, `sbom.py`, `rewrite_history.sh`
- `.github/workflows/ci.yml` builds the C++ engine, runs both suites, and
  uploads a SARIF self-check of this repo
