# PRISM user guide

PRISM (Performance, Regression, Integration and Security Module) is a code
checker: one command that finds every error it can in a code base and says,
just as plainly, what it could not check. This guide covers installing it,
running it, reading its reports and wiring it into CI.

- Design and stage order: [PLAN.md](PLAN.md)
- Every verdict and the laws that govern it: [VERDICTS.md](VERDICTS.md)
- How far PRISM's verdicts can be trusted, measured: [CONFORMANCE.md](CONFORMANCE.md)

## 1. Install

PRISM has two engines with the same stages, laws and report format. The C++
engine is the product; the Python engine is a frozen reference used to
cross-check it (roadmap decision D8). Linux x86-64 is the primary platform
(Windows users run PRISM under WSL).

### C++ engine (primary)

Requirements: CMake 3.20+, Ninja, clang 18 (or GCC 13), Python 3.11 for the
tooling. Z3, PCRE2, xsimd and nlohmann/json are vendored under `third_party/`
and built with PRISM; nothing is downloaded at build time.

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build            # the first build compiles Z3 and is slow
./build/prism_tests            # unit tests
./build/prism --list-stages
```

Options: `-DPRISM_QT=ON` builds the Qt 6 GUI (`prism_gui`), `-DPRISM_LLAMA=ON`
links llama.cpp for the LLM stage, `-DPRISM_CUDA=ON` builds the GPU mutator.
Add `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
to make rebuilds cheap.

### Python engine (reference)

```
pip install z3-solver
python -m prism path/to/code --no-llm
```

### Optional external tools

PRISM calls these as external programs when they are on `PATH` (or named
with `--tool NAME=PATH`). A tool that is not installed is reported as
`NOTRUN` with an install hint; it is never counted as a clean result
(Law 1).

| kind | tools |
|---|---|
| C/C++ analyzers | cppcheck, clang-tidy, ESBMC, CBMC, Infer, CodeQL, Semgrep, Coccinelle (`spatch`), Frama-C, KLEE, Strix |
| fuzzers | AFL++ (`PRISM_AFL=1`), libFuzzer (`PRISM_LIBFUZZER=1`) |
| other languages | ruff or pyflakes, mypy, node, tsc, eslint, bash, shellcheck, gofmt, cargo clippy, ruby, php, perl, luac, yamllint |
| sandbox | bubblewrap (`bwrap`) for `--allow-exec` |
| LLM | a GGUF model (`PRISM_GGUF`, `PRISM_MODEL`) or a llama.cpp server (`PRISM_LLAMA_SERVER`) |

## 2. Run

```
./build/prism path/to/code --no-llm            # whole pipeline, no LLM
./build/prism src/foo.c --stage inventory,classify,bmc,harness
./build/prism path/to/code --skip fuzz,diff --jobs 8
./build/prism path/to/code --resume            # continue an interrupted run
./build/prism --list-stages
```

| option | meaning |
|---|---|
| `PATH` | a file or directory (default `testdata`) |
| `--out DIR` | report directory (default `prism-out/`) |
| `--stage a,b` / `--skip a,b` | run only / leave out these stages. Left-out stages are listed as `skipped` in the report, never silently dropped |
| `--no-llm` | do not run the LLM stage |
| `--unwind K` | loop unwinding bound for BMC (incremental: 1, 2, 4, … K) |
| `--jobs N`, `-j N` | worker count for lints (0 = half the CPUs) |
| `--fuzz-budget S`, `--fuzz-iters N` | fuzzing time / iteration budget |
| `--repair-rounds N` | budget for the repair stage |
| `--tool NAME=PATH` | use this binary for an adapter (searched before `PATH`) |
| `--allow-exec` | allow steps that run code from the scanned tree (section 6) |
| `--fail-on never\|defect\|gap` | exit-code policy for CI (section 5) |
| `--resume` | reuse stages already `ok`/`NOTRUN` in `--out/stages.jsonl` |
| `--gui` | open the Qt GUI (C++ build with `-DPRISM_QT=ON`) |

The stages run in a fixed order (`--list-stages`): inventory, classify,
lints, taint, thread, interval, warnings, cppcheck, pbsd, sanitize, optional,
polyglot, esbmc, dafny, contracts, wp, bmc, harness, concolic, fuzz, diff,
rapid, muttest, ltl, llm, execute, repair, unify. What each one does is in
[PLAN.md](PLAN.md#pipeline-order-is-the-method).

## 3. Read the report

Everything lands in `--out` (default `prism-out/`):

| file | contents |
|---|---|
| `report.md` | the human report: confidence, stage table, findings |
| `report.json` | the full machine report (`stages[].findings[]`) |
| `report.sarif` | SARIF 2.1.0 (section 4) |
| `stages.jsonl` | one line per stage as it finishes; `--resume` reads it |
| `functions.json` | the inventory: every function and its class |
| `taxonomy.json` | which defect classes were COVERED / PARTIAL / GAP |

### Findings

Each finding carries a `stage`, a `status` (the verdict), the `file`,
`function` and `line`, a defect class `cls` (for example `INT-SIGNED-OVF`,
`INT-DIV-ZERO`, `INT-SHIFT-UB`, `MEM-OOB-READ`), a `message`, a `strength`
(how much the silence of that stage is worth: `PROVES`, `FINDS`, `SOME`,
`READS`) and, for refutations, a `counterexample`.

### Verdicts

The status vocabulary is closed (`prism/laws.py`, `include/prism/laws.hpp`);
[VERDICTS.md](VERDICTS.md) defines each one and the laws that relate them.
In short:

| verdict | reads as |
|---|---|
| [`PROVED-CERTIFIED`](VERDICTS.md#proved-certified) | proved, and the solver's answer was checked by a verified proof checker |
| [`PROVED-UNBOUNDED`](VERDICTS.md#proved-unbounded) | the encoded properties hold for every input and every number of loop iterations |
| [`PROVED`](VERDICTS.md#proved) | the encoded properties hold; loops closed within the bound |
| [`PROVED-ASSUMING`](VERDICTS.md#proved-assuming) | proved under an explicit precondition (`requires`) you wrote |
| [`BOUNDED`](VERDICTS.md#bounded) | nothing found within the unwind bound. **Not a proof** (Law 2) |
| [`FAILED`](VERDICTS.md#failed) | a counterexample exists; see `counterexample` |
| [`NEEDS-HARNESS`](VERDICTS.md#needs-harness) | PRISM will not guess preconditions (pointer parameters, Law 6) or cannot model a construct yet |
| [`UNKNOWN`](VERDICTS.md#unknown), [`TIMEOUT`](VERDICTS.md#timeout), [`ERROR`](VERDICTS.md#error) | the tool ran and did not conclude |
| [`NOTRUN`](VERDICTS.md#notrun) | the tool or step did not run (missing tool, or `--allow-exec` not given). Never a clean result (Law 1) |
| [`CRASH`](VERDICTS.md#crash), [`SANFAIL`](VERDICTS.md#sanfail) | a fuzzer or sanitizer observed the defect |
| [`CLEAN`](VERDICTS.md#clean) | a fuzzer found nothing. **Not a proof** (Law 3) |
| [`HYPOTHESIS`](VERDICTS.md#hypothesis), [`READS`](VERDICTS.md#reads) | LLM output: a lead to check, never evidence (Law 4) |

Two things to keep in mind:

- A proof covers the *encoded* properties (signed overflow, division by zero,
  shift range, array bounds, the `assert`s in the code) of the function as
  PRISM's front end understood it. [CONFORMANCE.md](CONFORMANCE.md) lists
  the constructs where the current engine is known to prove something false;
  until that list is empty, treat a proof of code that uses them with care.
- Pointer-parameter functions are never model-checked unguarded (Law 6).
  Write preconditions (one `// requires:` per line, at the top of the body)
  to get a verdict; a proof under them is `PROVED-ASSUMING`, never `PROVED`
  (see `testdata/ptr_copy.c`):

  ```c
  void copy(char *dst, char *src, int n)
  {
      // requires: n >= 0
      // requires: n < 8
      for (int i = 0; i < n; i++) dst[i] = src[i];
  }
  ```

### Confidence

`report.md` opens with `confidence = visibility × answer × resolution`
(Law 5): how much of the code PRISM could see, how much of it got an answer,
and how much of the answer is resolved. No data scores 0; 0 means "no data",
not "clean".

## 4. SARIF

`report.sarif` is SARIF 2.1.0. Only defects (`FAILED`, `CRASH`, `SANFAIL`)
become `results`; `HYPOTHESIS` becomes a `note`; stages that did not run or
failed become tool execution notifications, so a viewer shows what was not
checked. Load it into GitHub code scanning, VS Code (SARIF Viewer) or any
SARIF dashboard.

## 5. Exit codes and `--fail-on`

| `--fail-on` | exit 1 when |
|---|---|
| `never` (default) | never; the report is the output |
| `defect` | any `FAILED`, `CRASH` or `SANFAIL` finding |
| `gap` | a defect, or anything `NOTRUN`, `ERROR` or `TIMEOUT` (nothing was left unchecked) |

A stage that crashed is exit 2 regardless of policy.

## 6. Untrusted code and `--allow-exec`

PRISM never runs the code it checks unless you pass `--allow-exec` (Law 9).
Without it, sanitizer runs, compiled fuzz/diff harnesses, AFL++/libFuzzer,
KLEE, `perl -c`, `cargo clippy`, `eslint`, Coccinelle script rules and
LLM-written programs are `NOTRUN` with the hint `re-run with --allow-exec
(only on code you trust)`. Parsing, lints, BMC, the concrete interpreter and
compile-only diagnostics still run.

With `--allow-exec`, built binaries run in a bubblewrap jail when `bwrap` is
installed (read-only root, private `/tmp`, no network) and always under
rlimits; each finding records `extra.sandbox`. The sanitize stage only calls
zero-argument functions you mark with `// prism: run`. The full per-stage
table is in [PLAN.md](PLAN.md#running-on-untrusted-code-law-9).

## 7. Other languages (polyglot)

The `polyglot` stage checks every non-C file in the tree with the standard
tool for its language: Python (`compile()`, ruff or pyflakes, mypy), JSON,
TOML, JavaScript (`node --check`, eslint), TypeScript (tsc), shell (`bash
-n`, shellcheck), Go (gofmt), Rust (cargo clippy), Ruby, PHP, Perl, Lua and
YAML (yamllint). Every text file is also scanned for merge-conflict markers
and leaked credentials. A linter that is not installed is `NOTRUN`; a linter
that ran and said nothing is `UNKNOWN`, not clean; syntax-broken files are
kept away from type checkers.

```
./build/prism . --no-llm --stage inventory,lints,polyglot
```

## 8. CI integration

A GitHub Actions job that gates on defects and uploads SARIF to code scanning:

```yaml
jobs:
  prism:
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      security-events: write
    steps:
      - uses: actions/checkout@v4
      - name: Build PRISM
        run: |
          sudo apt-get update && sudo apt-get install -y ninja-build clang
          git clone --depth 1 <your PRISM mirror> prism-src
          cmake -S prism-src -B prism-build -G Ninja -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF \
            -DPRISM_QT=OFF -DPRISM_Z3=ON
          cmake --build prism-build --target prism
      - name: Check
        run: prism-build/prism . --no-llm --fail-on defect --out prism-out
      - uses: github/codeql-action/upload-sarif@v3
        if: always()
        with:
          sarif_file: prism-out/report.sarif
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: prism-report
          path: prism-out/
```

Notes:

- Use `--fail-on gap` when "not checked" must fail the build too.
- Leave `--allow-exec` off in CI that runs on pull requests from forks.
- Cache `prism-build/` (or use ccache): the first build compiles Z3.
- This repository's own CI is `.github/workflows/ci.yml` (build, both test
  suites, SARIF self-check) and `.github/workflows/conformance.yml` (the
  release gate below).

## 9. How much to trust a proof

PRISM's release gate is the conformance suite (roadmap 2.7, 6.1): a set of
C/C++ tasks with known answers, a pinned subset of SV-COMP and, nightly, the
NIST Juliet CWE190/191/369/476/680 tests. Any wrong proof blocks a release.

```
PRISM_BIN=build/prism python tools/conformance.py          # metrics + gate
python tools/conformance.py --self-check                   # validate the task labels
PRISM_BIN=build/prism python tools/csmith_soundness.py -n 300   # random programs
```

The metric definitions, the current numbers and every known wrong proof with
a reproducer are in [CONFORMANCE.md](CONFORMANCE.md).
