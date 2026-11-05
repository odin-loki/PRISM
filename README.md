# PRISM

**PRISM** = Performance, Regression, Integration and Security Module.

PRISM is a code checker. One command finds every error it can in a code
base, and says just as plainly what it could **not** check. It runs
deterministic instruments first, then proofs, then fuzzing, then a language
model that may only propose.

PRISM is one C++23 program, `prism`.

## What it checks

| Area | How |
|---|---|
| C and C++ | pattern lints on regex and Clang-AST layers, taint, thread and interval checks, compiler warnings, cppcheck, sanitizers, the `pir` model checker (Clang/LLVM IR to Z3, bounded checking, k-induction, loop invariants, libc and C++ library models), threads by lazy sequentialisation, contracts, concolic runs, fuzzing, differential, property and mutation testing, LTL on state machines |
| Every other language | the `polyglot` stage: syntax checks, linters and type checkers for Python, JSON, TOML, JavaScript, TypeScript, shell, Go, Rust, Ruby, PHP, Perl, Lua and YAML |
| Every text file | merge-conflict markers and leaked credentials |
| External analysers | ESBMC, CBMC, Infer, Semgrep, Coccinelle, KLEE, Frama-C, clang-tidy, Dafny, Strix, when installed |

A tool that is not installed is reported as `NOTRUN`, never as a clean
result.

## Build and run

You need CMake, Ninja and the pinned clang (LLVM 23.1.4;
`python3 scripts/fetch_llvm.py` downloads it).

```
L=/path/to/llvm-23.1.4/bin
cmake -B build -G Ninja -DCMAKE_C_COMPILER=$L/clang -DCMAKE_CXX_COMPILER=$L/clang++ \
  -DPRISM_LLVM_PINNED_BIN=$L -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build
./build/prism path/to/code --no-llm
```

The report lands in `prism-out/`:

- `report.md`: the human report
- `report.json`: the full machine report
- `report.sarif`: SARIF 2.1.0 for code scanning and IDEs
- `TRUSTED_BASE.md`, `VERDICTS.md`: what a proof trusts, and what each verdict means

`--fail-on defect` makes the exit code fail CI on a defect.
`--fail-on gap` also fails it when something was not checked.

PRISM never runs the code it checks unless you pass `--allow-exec`.

The [user guide](docs/USER_GUIDE.md) has every option, the report format
and a CI recipe.

## Laws

- A missing tool is `NOTRUN`. It is never a clean result.
- `PROVED` and `BOUNDED` are never merged.
- A fuzzer `CLEAN` is not a proof.
- Model output is `HYPOTHESIS` or `READS`. It is never evidence.
- Running scanned code needs `--allow-exec`.

The full list is in [CLAUDE.md](CLAUDE.md). The verdict lattice is proved in
Lean 4 (`proofs/`), and the C++ verdict code is tested against the proved
model entry by entry ([docs/VERDICTS.md](docs/VERDICTS.md)).

## Documentation

| For | Read |
|---|---|
| Running PRISM | [docs/USER_GUIDE.md](docs/USER_GUIDE.md) |
| What works today | [docs/STATUS.md](docs/STATUS.md) |
| How the pipeline works | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Verdicts and what a proof trusts | [docs/VERDICTS.md](docs/VERDICTS.md), [docs/TRUSTED_BASE.md](docs/TRUSTED_BASE.md) |
| The model checker | [docs/PIR.md](docs/PIR.md), [docs/SOLVERS.md](docs/SOLVERS.md), [docs/CONCURRENCY.md](docs/CONCURRENCY.md) |
| How trustworthy the verdicts are | [docs/CONFORMANCE.md](docs/CONFORMANCE.md), [docs/EVALUATION.md](docs/EVALUATION.md), [docs/SVCOMP.md](docs/SVCOMP.md) |
| Language-model features | [docs/AI.md](docs/AI.md) |
| Proofs | [docs/PROOFS_SEMANTICS.md](docs/PROOFS_SEMANTICS.md), [docs/PROOFS_TECHNIQUES.md](docs/PROOFS_TECHNIQUES.md), [docs/PROOFS_REFINEMENT.md](docs/PROOFS_REFINEMENT.md), [docs/assurance/](docs/assurance/README.md) |
| Windows | [docs/MSVC_PORT.md](docs/MSVC_PORT.md) |
| Dependencies, pins, releases | [docs/SUPPLY_CHAIN.md](docs/SUPPLY_CHAIN.md) |
| Contributing | [CLAUDE.md](CLAUDE.md), [AGENTS.md](AGENTS.md), [docs/CXX_STANDARD.md](docs/CXX_STANDARD.md) |

## Licence

GNU Affero General Public License, version 3 ([LICENSE](LICENSE)); see also
[NOTICE](NOTICE). The four linked third-party libraries are pinned in
[third_party/MANIFEST.toml](third_party/MANIFEST.toml).
