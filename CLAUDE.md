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
  `PRISM_GGUF`, `PRISM_MODEL`, `PRISM_LLAMA_SERVER`, `PRISM_NATIVE_DLL`).
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

`third_party/` is vendored source (copied trees, not submodules). Never
rewrite it, never glob it from CMake.

## Layout

- `prism/` Python engine; `prism/cocci/` shipped Coccinelle rules
- `src/prism/` C++ engine; `src/gui/` Qt GUI; `src/cuda/` CUDA havoc
- `include/prism/` C++ headers
- `tests/` Python tests (+ `tests/cpp/` doctest suite)
- `testdata/` planted-bug corpus both engines run on
- `docs/PLAN.md` pipeline design; `docs/MINED.md` what was mined from each tool
- `tools/` one-shot generators; `scripts/` local build/smoke helpers
- `.github/workflows/ci.yml` builds the C++ engine, runs both suites, and
  uploads a SARIF self-check of this repo
