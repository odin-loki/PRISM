# PRISM status

What works, what is partial and what is missing, each with the evidence in
the tree. Written against main at 991c4eafa (2026-11-05). Every number
names its date and commit. Branches that are landing are named where they
change a row.

## Headline

On 991c4eafa (2026-11-05) all four push workflows are green: PRISM CI
(run 41968877603), the conformance release gate (41968877547), proofs and
docs.

Conformance on 991c4eafa, CI run 41968877547 (1430 scored functions; the
full table is in [CONFORMANCE.md](CONFORMANCE.md)):

| stage | wrong proofs | proved (true tasks) | refuted (false tasks) | false alarms |
|---|---|---|---|---|
| pir | **0** | 573 / 714 | 607 / 682 (318 with a replayed counterexample) | 0 |
| conc | **0** (never proves) | `BOUNDED` only | 8 / 9 | 0 |

The nightly jobs (sanitizer self-check, random programs, Juliet) last ran
on 2026-11-04 at 0a9571725 and failed at configure, before the FORGE module
rename was fixed in 8de9b8203. None has run since. Branch port/sanitize
(landing) works on the sanitizer job.

## Works

| Area | Evidence |
|---|---|
| The pipeline: 30 stages in a fixed order, `--stage`/`--skip`, `--resume`, nothing skipped quietly | `include/prism/pipeline.hpp`, `src/prism/pipeline.cpp`; `tests/cpp/test_pipeline.cpp`, `tests/cpp/test_cli_stages.cpp` |
| Reports: `report.json`, `report.md`, SARIF 2.1.0, `--fail-on`, shipped `VERDICTS.md`/`TRUSTED_BASE.md` | `src/prism/sarif.cpp`, `src/prism/shipdocs.cpp`; `tests/cpp/test_sarif.cpp`, `tests/cpp/test_cli.cpp`, `tests/cpp/test_shipdocs.cpp` |
| Verdict lattice equal to the Lean model on its whole finite domain | `src/prism/verdict/`, `proofs/Prism/Verdict.lean`, `testdata/fixtures/verdict_tables.json`; `tests/cpp/test_main.cpp` "Verdict module equals the Lean model" |
| Law 9 gate: scanned code runs only with `--allow-exec`, in a sandbox | `src/prism/sandbox.cpp`, `exec_stages()` in `src/prism/pipeline.cpp`; `tests/cpp/test_sandbox.cpp` ([ARCHITECTURE.md](ARCHITECTURE.md#running-on-untrusted-code-law-9)) |
| `pir`: Clang/LLVM IR to PIR to Z3, memory model, floating point, exceptions, coroutines, k-induction, Houdini invariants | `src/prism/pir/`; `tests/cpp/test_pir*.cpp`; [PIR.md](PIR.md) |
| libc and C++ standard-library models used by `pir` | `src/prism/pir/models/libc/`, 73 entries in `src/prism/pir/models/cxx/`; harnesses in `testdata/conformance/libc-models/`; `tests/cpp/test_cxx_*.cpp` |
| Certified mode: CaDiCaL LRAT checked by cake_lpr and by Lean's checker, CNF from the Lean-proved bit-blaster when built | `src/prism/solver/`, `proofs/techniques`; `tests/cpp/test_certified.cpp`, `tests/cpp/test_leanbb.cpp` |
| Solver portfolio (Z3, Bitwuzla, CaDiCaL, Kissat, SLS) with a query cache and a learned scheduler | `src/prism/solver/`; [SOLVERS.md](SOLVERS.md) |
| Threads: lazy sequentialisation over the PIR memory model, SC, x86-TSO and C11 orders | `src/prism/conc/`; `tests/cpp/test_conc.cpp`; [CONCURRENCY.md](CONCURRENCY.md) |
| Lints: regex layer and Clang-AST layer, both front ends tested on every case | `src/prism/checkers_*.cpp`, `src/prism/astlint*.cpp`; `tests/cpp/test_astlint.cpp` |
| Other languages through `polyglot` | `src/prism/polyglot.cpp`; `tests/cpp/test_polyglot.cpp` |
| Conformance suite and release gate (0 wrong proofs) | `src/tools/qa/conformance.cpp`, `testdata/conformance/`, `.github/workflows/conformance.yml` |
| Random-program soundness campaigns | `src/tools/qa/soundness.cpp`; nightly in `conformance.yml` |
| Lean proofs: verdict laws, PIR semantics, techniques, LLVM IR to PIR refinement for a fragment, rechecked by `leanchecker` and `nanoda` | `proofs/`; `.github/workflows/proofs*.yml`; [PROOFS_SEMANTICS.md](PROOFS_SEMANTICS.md), [PROOFS_TECHNIQUES.md](PROOFS_TECHNIQUES.md), [PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md) |
| Supply chain: pins, hashes, licence firewall, SBOM | `third_party/MANIFEST.toml`, `src/tools/prism_deps/`; `tests/cpp/test_deps.cpp`; [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md) |
| Self-fuzzing of PRISM's own parsers | `prism_fuzz_self` (`-DPRISM_FUZZ=ON`); [FUZZ_SELF.md](FUZZ_SELF.md) |
| Windows x64 host build (clang-cl) with the Windows CI jobs | `.github/workflows/ci.yml` (windows-compile-smoke, windows-gui-qt); [MSVC_PORT.md](MSVC_PORT.md) |
| Qt GUI with a headless smoke run in CI | `src/gui/`; `ci.yml` |
| Language-model features, tested with fakes and FORGE's tiny test model | `src/prism/ai/`; `tests/cpp/test_ai*.cpp`, `tests/cpp/test_forge_ai.cpp`; [AI.md](AI.md) |

## Partial

| Area | What exists | What is missing | Evidence |
|---|---|---|---|
| `pir` coverage | 573 of 714 true tasks proved (991c4eafa) | 157 functions get no answer and 40 stay `BOUNDED`. Constructs that are not encoded are listed in [PIR.md](PIR.md) | CI run 41968877547 |
| C++ library models | 73 model entries; C++ for the MSVC target uses the MSVC STL headers instead (port/winmsvc) | libc++ is not modelled; see PIR.md "C++ library models" for the rest | `src/prism/pir/models/cxx/` |
| A known wrong proof outside the gate | — | Under `-std=c++03`, `pir` proves `1 << 31`, which C++03 leaves undefined. The doctest records it as a `WARN` | [CONFORMANCE.md](CONFORMANCE.md) F7 |
| `conc` | Finds races and deadlocks within a bound | Never proves; unbounded rounds and the slot encoding are not proved in Lean | [CONCURRENCY.md](CONCURRENCY.md) |
| AST lints | AST checks for most regex classes | Code that depends on a template parameter is not checked; flow checks join `if` arms but havoc `switch` bodies | `src/prism/astlint_checks.cpp` |
| Refinement proof | Integer fragment proved; an extended fragment per function under a checked certificate; 0 mismatches on 2026-10-30 at 30eca81b3 | Library calls outside the models, `realloc`/`FILE`, symbolic-size contracts | [PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md) |
| Certified mode | Works per function | No full-suite `--certified` rerun is recorded on the current engine | `prism-qa conformance --certified` |
| SV-COMP | `prism svcomp`, witnesses 2.0, BenchExec plugin, `svcomp pack` | The subset has not been rescored since `bmc` was deleted; no bundled toolchain; BenchExec registration not merged | [SVCOMP.md](SVCOMP.md) |
| Language model | Every feature built; Goedel-Prover-V2-32B is the model (port/goedel) | No measurement on the real model is recorded | [AI.md](AI.md) |
| Windows | Host build with clang-cl in CI | MSVC `cl` builds (port/winmsvc) have no CI job; the MSVC analysis target is landing; SV-COMP stays Linux | [MSVC_PORT.md](MSVC_PORT.md) |
| Sandbox | bwrap jail, rlimits | Narrower jail, `--allow-exec=unsandboxed` and the Windows Job Object kind are on port/harden | [ARCHITECTURE.md](ARCHITECTURE.md#running-on-untrusted-code-law-9) |
| `--resume` | Reuses finished stages | The run-key check that notices a changed tree or option is on port/harden | `src/prism/journal.cpp` |
| Real-world evaluation | Reduced reproducers from five projects run as doctests | `prism-qa bench` (port/bench) has no recorded run | [EVALUATION.md](EVALUATION.md) |
| GPU | CUDA mutation and ProbSAT kernels build with `-DPRISM_CUDA=ON` | Never run on a GPU; no GPU CI runner | `src/cuda/` |

## Missing

| Item | Why |
|---|---|
| A signed release | No `v*` tag has been pushed, so `release.yml` has never run |
| History rewrite | `scripts/rewrite_history.sh` is ready; the force-push needs the owner |
| Legal review of the third-party licence table | Owner action ([SUPPLY_CHAIN.md](SUPPLY_CHAIN.md)) |
| LoRA fine-tuning of the model | Needs the GPU and the model |
| Unwind prediction | Measured and left off: it lost verdicts ([AI.md](AI.md)) |

## Landing branches

These are not on main yet. Each row says what it changes.

| Branch | Change |
|---|---|
| port/goedel | Goedel-Prover-V2-32B Q4_K_M is the only model, for the assistant and Lean proof search |
| port/cipin | FORGE pinned by commit in `cmake/FORGE_PIN`; the PRISM-on-PRISM CI job fails on a crash or a missing report |
| port/sanitize | the nightly sanitizer job and self-check fixes |
| port/testharness | shared test helpers; NOTRUN test cases are counted and printed; `PRISM_TESTS_REQUIRE_TOOLS=1` fails them; C++ model tests use `$PRISM_LLVM_BIN/clang++` |
| port/precision | confidence over the stages that ran and every parsed function; cppcheck advice becomes `READS`; classify rows are one line in report.md; pir fixes |
| port/bench | `prism-qa bench` on twelve pinned projects |
| port/winmsvc | MSVC `cl` host builds; `--target`/`PRISM_TARGET` with the Windows x64 MSVC (LLP64) target |
| port/harden | `--resume` run key; `--allow-exec` needs bwrap or `--allow-exec=unsandboxed`; narrower jail; Windows Job Object kind; libclang choice |
