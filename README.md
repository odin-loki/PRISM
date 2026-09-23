# PRISM

**PRISM** = Performance, Regression, Integration and Security Module.

One command that finds every error it can in a code base — and says, just as
loudly, what it could **not** check. Deterministic instruments first, then
bounded proofs, then fuzzing, then an LLM (Qwen 3.5 9B) that may only
*hypothesize*.

> The name is **PRISM** (an earlier name is retired) — see [CLAUDE.md](CLAUDE.md).
> PRISM ships two engines with the same stages and laws: the C++23 engine
> (`src/prism`, binary `prism`) and the Python engine (package `prism/`,
> `python -m prism PATH`).

## What it checks

| Area | How |
|---|---|
| C / C++ | ~600-class defect taxonomy: pattern lints, taint, threads, interval ranges, compiler warnings, cppcheck, sanitizers (ASan/UBSan/TSan), Z3 bounded model checking + k-induction, Dafny/ACSL contracts, Frama-C-style WP, harness BMC, concolic, greybox fuzzing (FuSeBMC loop, AFL++/libFuzzer when present), differential + property + mutation testing, LTL on state machines |
| Every other language | `polyglot` stage: Python/JSON/TOML syntax, ruff or pyflakes, mypy, `node --check`, tsc, eslint, `bash -n`, shellcheck, gofmt, cargo clippy, `ruby -wc`, `php -l`, `perl -c`, `luac -p`, yamllint |
| Any text file | merge-conflict markers; leaked credentials (private keys, AWS, GitHub, Slack, Google, Stripe) |
| External analyzers | ESBMC, CBMC, Infer, CodeQL, Semgrep, Coccinelle, KLEE, Frama-C, clang-tidy, Strix — run when installed |

## Run

```
# C++ engine (primary)
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build
./build/prism path/to/code --no-llm --jobs 8
./build/prism path/to/code --resume
./build/prism_tests

# Python engine (same stages, same report)
pip install z3-solver
python -m prism path/to/code --no-llm

# Native MSVC (cl), from a VS/x64 Native Tools prompt
cmake -B build -G Ninja -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build
```

Output under `--out` (default `prism-out/`):

- `report.md` — human report; `report.json` — full machine report
- `report.sarif` — SARIF 2.1.0 for GitHub code scanning, VS Code, CI dashboards
- `stages.jsonl` — live stage log (`--resume` picks up from it)
- `taxonomy.json` — which defect classes were COVERED / PARTIAL / GAP

## Running on untrusted code

PRISM never runs the code it checks unless you say so. Stages that would
execute code from the scanned tree — sanitizer runs, compiled fuzz/diff
harnesses, AFL++/libFuzzer, KLEE, `perl -c`, `cargo clippy`, `eslint`,
Coccinelle script rules, LLM-written programs — are `NOTRUN` with the hint
`re-run with --allow-exec (only on code you trust)`. Parsing, lints, BMC
(Z3), the concrete interpreter, and compile-only diagnostics still run.

With `--allow-exec`, built binaries run in a bubblewrap jail when `bwrap` is
installed (read-only filesystem, private /tmp, no network) and always under
rlimits; findings record `extra.sandbox`. The sanitize stage only calls
functions you mark with `// prism: run`. The per-stage table is in
[docs/PLAN.md](docs/PLAN.md#running-on-untrusted-code-law-9).

CI gating: `--fail-on defect` exits 1 on any FAILED/CRASH/SANFAIL finding;
`--fail-on gap` also exits 1 when anything was NOTRUN/ERROR/TIMEOUT. A stage
that crashed is exit 2.

## Laws

A missing tool is `NOTRUN`. It is never a clean result. `PROVED` and
`BOUNDED` are never merged. A fuzzer `CLEAN` is not a proof. LLM output
is `HYPOTHESIS`. A Dafny-style proof under `requires` is `PROVED-ASSUMING`.
A linter that ran and said nothing is `UNKNOWN`, not clean.
Executing scanned code requires `--allow-exec`; without it that step is
`NOTRUN`. `PROVED-CERTIFIED` (an UNSAT result checked by a verified
checker) is never merged with a weaker verdict either.

These laws are proved in Lean 4 (`proofs/`) and the C++ and Python verdict
code is tested entry by entry against the proved model; see
[docs/VERDICTS.md](docs/VERDICTS.md) for the full lattice.

See [docs/PLAN.md](docs/PLAN.md) and [docs/MINED.md](docs/MINED.md).

## Stack

GUI is Qt. Inference is llama.cpp (Ollama fallback). SIMD is vendored xsimd.
GPU mutation is CUDA. SMT is vendored Z3. Regex is vendored PCRE2.

Third-party sources live in `third_party/` (xsimd, Z3, PCRE2, nlohmann/json,
doctest, llama.cpp, plus the mined adapter projects listed in
[`third_party/SOURCES.md`](third_party/SOURCES.md)). They are trees copied
into this repo, not package downloads at build time and not git submodules.
