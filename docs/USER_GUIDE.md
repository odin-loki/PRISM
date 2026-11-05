# PRISM user guide

PRISM (Performance, Regression, Integration and Security Module) is a code
checker. One command finds every error it can in a code base. It also says
what it could not check. This guide covers building PRISM, running it,
reading its reports and using it in CI.

- How the pipeline works: [ARCHITECTURE.md](ARCHITECTURE.md)
- Every verdict and the laws behind it: [VERDICTS.md](VERDICTS.md)
- How far the verdicts can be trusted: [CONFORMANCE.md](CONFORMANCE.md)
- What works today: [STATUS.md](STATUS.md)

## 1. Build

PRISM is a C++23 program (`src/prism/`, `include/prism/`). Linux x86-64 is
the main platform. Windows x64 builds natively ([MSVC_PORT.md](MSVC_PORT.md)).

You need:

- CMake 3.20 or later and Ninja.
- The pinned clang, LLVM 23.1.4 (`third_party/llvm/PIN.toml`).
  `python3 scripts/fetch_llvm.py` downloads it and checks its SHA-256.
  Other compilers get a configure warning.
- FORGE (odin-loki/FORGE), the platform layer and model engine. It is not
  vendored. Pass `-DFORGE_SOURCE_DIR=PATH` for a local checkout. Leave it
  empty and CMake fetches it. Branch port/cipin (landing) pins the commit
  in `cmake/FORGE_PIN`; on main (991c4eafa) CMake fetches FORGE `main`.

Z3, PCRE2, nlohmann/json and doctest are vendored in `third_party/` and
built with PRISM.

```
L=/path/to/llvm-23.1.4/bin
cmake -B build -G Ninja -DCMAKE_C_COMPILER=$L/clang -DCMAKE_CXX_COMPILER=$L/clang++ \
  -DPRISM_LLVM_PINNED_BIN=$L -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build            # the first build compiles Z3 and is slow
./build/prism --list-stages
```

| CMake option | default | effect |
|---|---|---|
| `PRISM_Z3` | ON | build the vendored Z3; the `pir` stage needs it |
| `PRISM_QT` | ON | build the Qt 6 GUI `prism_gui` |
| `PRISM_CUDA` | ON | build the CUDA mutation kernels |
| `PRISM_TESTS` | ON | build `prism_tests` |
| `PRISM_LIBCLANG` | ON | load libclang for the AST lints (needs `<clang-c/Index.h>` at build time) |
| `PRISM_TOOLS` | ON | build the `prism_ai` developer tool |
| `PRISM_SANITIZE` | OFF | build PRISM itself with ASan and UBSan |
| `PRISM_WERROR` | OFF | treat warnings as errors on engine targets |
| `PRISM_FUZZ` | OFF | build the libFuzzer self-fuzz target ([FUZZ_SELF.md](FUZZ_SELF.md)) |
| `FORGE_SOURCE_DIR` | empty | local FORGE checkout; empty fetches it |
| `PRISM_LLVM_PINNED_BIN` | empty | the pinned LLVM `bin/` that `pir` and the tests use |

Add `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
to make rebuilds cheap.

### GUI

`-DPRISM_QT=ON` builds `prism_gui` against Qt 6.4 or later. `prism PATH
--gui` opens it. To check it without a display:

```
QT_QPA_PLATFORM=offscreen ./build/prism_gui SRC --no-llm --out OUT \
  --smoke-screenshot gui.png
```

The run loads into the window and the window is saved to `gui.png`. The
exit code is 0 when the findings table is not empty and the screenshot was
written, 3 for an empty table and 2 when `prism` is not found.

### External tools

PRISM calls these as separate programs when they are on `PATH`, named with
`--tool NAME=PATH`, or built with `build/prism-deps tool NAME` (into
`~/.prism/tools/<name>/<commit>/`). A tool that is not installed is
`NOTRUN` with an install hint. It is never a clean result (Law 1).

| kind | tools |
|---|---|
| C/C++ analysers | cppcheck, clang-tidy, ESBMC, CBMC, Infer, Semgrep, Coccinelle (`spatch`), Frama-C, KLEE, Strix, Dafny |
| fuzzers | AFL++ (`PRISM_AFL=1`), libFuzzer (`PRISM_LIBFUZZER=1`) |
| other languages | ruff or pyflakes, mypy, node, tsc, eslint, bash, shellcheck, gofmt, cargo clippy, ruby, php, perl, luac, yamllint |
| sandbox | bubblewrap (`bwrap`) for `--allow-exec` |
| certified mode | CaDiCaL and cake_lpr (`prism-deps tool cadical cake_lpr`) |
| language model | a FORGE `.fpk` model file ([AI.md](AI.md)) |

## 2. Run

```
./build/prism path/to/code --no-llm            # every stage except the model
./build/prism src/foo.c --stage inventory,classify,pir
./build/prism path/to/code --skip fuzz,diff --jobs 8
./build/prism path/to/code --resume            # continue an interrupted run
```

A value follows its option (`--out DIR`) or is joined to it (`--out=DIR`,
`-j4`). An unknown option, a bad value, an unknown stage or a missing PATH
is exit 2.

| option | meaning |
|---|---|
| `PATH` | a file or a directory (default `testdata`) |
| `--out DIR` | report directory (default `prism-out`) |
| `--stage a,b` | run only these stages; repeat to add more; empty runs every stage |
| `--skip a,b` | leave these stages out; repeat to add more. Left-out stages are listed as `skipped` |
| `--list-stages` | print the stage order and exit |
| `--no-llm` | do not run the language model |
| `--jobs N`, `-j N` | worker count (default and 0: half the cores) |
| `--unwind N` | loop unwinding bound for `pir` and the external model checkers (default 8) |
| `--timeout S` | solver seconds per query (default 30) |
| `--fuzz-budget S`, `--fuzz-iters N` | fuzzing time (default 8 s) and iterations (default 2048) |
| `--repair-rounds N` | repair-stage rounds (default 3) |
| `--tool NAME=PATH` | the binary for one adapter. Searched before `~/.prism/tools/<name>/<commit>/bin` and `PATH` |
| `--resume` | reuse stages that finished `ok` or `NOTRUN` in `--out/stages.jsonl` (`report.json` as fallback) |
| `--fail-on never\|defect\|gap` | exit-code policy for CI (section 5) |
| `--allow-exec` | allow steps that run scanned code (section 6) |
| `--certified` | `pir` asks for a checked certificate of every verification condition (section 8) |
| `--solver-cache DIR` | solver query cache (default `$XDG_CACHE_HOME/prism/solver`, else `~/.cache/prism/solver`) |
| `--strict-aliasing` | `pir` also checks effective types (C11 6.5p6-7). Off by default because real code often breaks the rule on purpose |
| `--pir-drafts` | `pir` takes pointer sizes from the drafted harness when no `// requires:` gives them. A result under a drafted size is at best `PROVED-ASSUMING`; a violation found only there stays `NEEDS-HARNESS` (Law 6) |
| `--fp-checks` | `pir` also reports floating-point division by zero, invalid operations and overflow to infinity. Off by default because IEEE 754 defines these results. Out-of-range float-to-integer conversion is always checked |
| `--requirements PATH` | requirement documents (file or directory, repeatable); `review` drafts contracts traced to their sentences ([AI.md](AI.md)) |
| `--contracts-approved PATH` | approvals of drafted contracts (default `<root>/contracts.approved.json`); only approved clauses give `PROVED-ASSUMING` |
| `--pbsd PATH` | ParanoidBSD tree for the `pbsd` stage (else `PRISM_PBSD`; no default). Importing its modules also needs `--allow-exec` |
| `--gui` | open the Qt GUI |
| `--version`, `-V` | print the version and exit |
| `--help`, `-h` | print the option summary and exit |

Options from branches that are landing:

| option | branch | meaning |
|---|---|---|
| `--allow-exec=unsandboxed` | port/harden | also run scanned code where bubblewrap cannot run, with resource limits only (section 6) |
| `--target TRIPLE` | port/winmsvc | the platform the scanned code is built for (else `PRISM_TARGET`, else the host): `x86_64-unknown-linux-gnu` (LP64), `i386-unknown-linux-gnu` (ILP32) or `x86_64-pc-windows-msvc` (LLP64; UCRT and MSVC STL headers). The default on a Windows x64 host is `x86_64-pc-windows-msvc`. An unsupported triple is exit 2 |

### Stages

<!-- generated:stage-order -->
The stages run in a fixed order (`--list-stages`): inventory, classify,
lints, taint, thread, interval, warnings, cppcheck, pbsd, sanitize, optional,
polyglot, esbmc, dafny, contracts, wp, pir, conc, harness, review, concolic,
fuzz, diff, rapid, muttest, ltl, llm, execute, repair, unify. What each stage
does is in [ARCHITECTURE.md](ARCHITECTURE.md#stages).
<!-- /generated:stage-order -->

The `lints` stage has two layers. The regex layer runs on every C/C++
file. The Clang-AST layer parses each translation unit with libclang, loaded
at run time, or with `clang -fsyntax-only -Xclang -ast-dump=json` when
libclang is missing. Flags come from `compile_commands.json` in the root or
its `build/`. Both front ends give the same rows (`tests/cpp/test_astlint.cpp`
runs every case on both), and `extra.ast_backend` names the one that ran.
An AST row that replaces a regex row carries `extra.supersedes = "regex"`.
A unit that does not parse gets a `NOTRUN` row with
`extra.layer = "clang-ast"` and keeps its regex lints. Code that depends on
a template parameter is not checked by the AST layer.

### Report subcommands

These read a finished `report.json`. They never change a verdict
([AI.md](AI.md)).

| command | options |
|---|---|
| `prism regress` | `--report OUT/report.json` (default `prism-out/report.json`); `--write-tests DIR` (default `<out>/regression_tests/`); `--framework auto\|gtest\|catch2\|ctest\|pytest\|plain`; `--run` compiles and runs the tests (needs `--allow-exec`) |
| `prism ask "<question>"` | `--report ...`; `--json` prints the query and answer as JSON; `--no-llm` uses only the deterministic grammar |
| `prism draft` | `--report ...`; `--kind report\|assurance`; `--proofs DIR` where Lean theorems are looked up |
| `prism triage [OUT]` | `--threshold T` clustering threshold (default 0.7); `--no-embed` skips the embedding model |

### `prism prove`

`prism prove FILE.lean THEOREM` searches for a Lean proof of one theorem.
The model proposes tactics. The Lean kernel checks each one. The axioms
must stay within `propext`, `Classical.choice` and `Quot.sound`. Lean
elaboration runs code, so it needs `--allow-exec`; without it the result is
`NOTRUN`. Exit codes: 0 proved, 1 not proved, 3 `NOTRUN`, 2 error. With
`--all`, 0 means every theorem was proved and 3 means every one was
`NOTRUN`.

| option | meaning |
|---|---|
| `FILE.lean THEOREM` | the Lean file and the theorem |
| `--all [ROOTS...]` | try every theorem with a `sorry` under ROOTS (default `proofs`, `proofs/semantics`, `proofs/techniques`) and write `<out>/prove/summary.json` |
| `--list` | list the theorems that still contain a `sorry` |
| `--write` | write the proof into the file and add it to the lemma library |
| `--allow-exec` | allow Lean elaboration (required) |
| `--project DIR` | the Lake project (default: the nearest parent with a lakefile) |
| `--lemmas PATH` | lemma library (default `proofs/lemmas.jsonl`) |
| `--budget N`, `--beam N` | model calls (default 24) and partial proofs kept (default 4) |
| `--timeout S` | Lean seconds per check (default 120, minimum 5) |
| `--out DIR` | output directory (default `prism-out`) |
| `--prover-fpk PATH`, `--prover-model NAME` | the prover model file (else `PRISM_PROVER_FPK`) and the name recorded in the audit (else `PRISM_PROVER_MODEL`) |
| `--json` | print only the JSON result |
| `--help`, `-h` | print the option summary |

### `prism svcomp`

`prism svcomp --prop PROPERTY.prp TASK.c` runs PRISM as an SV-COMP verifier
on one task. It prints `PRISM-SVCOMP-RESULT: true`, `false(<property>)`,
`unknown` or `error`, and writes a witness. `false` needs a counterexample
that replays, which runs the task; without `--allow-exec` every refutation
is `unknown`. `prism svcomp score` scores the pinned subset and
`prism svcomp pack` builds the tool archive. See [SVCOMP.md](SVCOMP.md).

## 3. Read the report

| file in `--out` | contents |
|---|---|
| `report.md` | human report: confidence, stage table, findings |
| `report.json` | full machine report (`stages[].findings[]`) |
| `report.sarif` | SARIF 2.1.0 (section 4) |
| `stages.jsonl` | one line per finished stage; `--resume` reads it |
| `functions.json` | every function and its class |
| `taxonomy.json` | each defect class as COVERED, PARTIAL or GAP |
| `triage.json` | root-cause clusters (ordering only) |
| `TRUSTED_BASE.md` | what a proof depends on ([TRUSTED_BASE.md](TRUSTED_BASE.md)) |
| `VERDICTS.md` | the verdict definitions; `report.md` links each finding's verdict here |

Each finding has a `stage`, a `status` (the verdict), `file`, `function`,
`line`, a defect class `cls` (for example `INT-SIGNED-OVF` or
`MEM-OOB-READ`), a `message`, a `strength` (`PROVES`, `FINDS`, `SOME` or
`READS`: what the stage's silence is worth) and, for a refutation, a
`counterexample`.

| verdict | reads as |
|---|---|
| [`PROVED-CERTIFIED`](VERDICTS.md#verdict-proved-certified) | proved, and the solver's answer was checked by a verified checker |
| [`PROVED-UNBOUNDED`](VERDICTS.md#verdict-proved-unbounded) | the checked properties hold for every input and every number of loop iterations |
| [`PROVED`](VERDICTS.md#verdict-proved) | the checked properties hold; every loop closed within the bound |
| [`PROVED-ASSUMING`](VERDICTS.md#verdict-proved-assuming) | proved under a precondition you wrote |
| [`BOUNDED`](VERDICTS.md#verdict-bounded) | nothing found within the unwind bound. Not a proof (Law 2) |
| [`FAILED`](VERDICTS.md#verdict-failed) | a defect was found |
| [`NEEDS-HARNESS`](VERDICTS.md#verdict-needs-harness) | a precondition is missing (pointer parameters, Law 6) or a construct is not modelled |
| [`UNKNOWN`](VERDICTS.md#verdict-unknown), [`TIMEOUT`](VERDICTS.md#verdict-timeout), [`ERROR`](VERDICTS.md#verdict-error) | the tool ran and did not conclude |
| [`NOTRUN`](VERDICTS.md#verdict-notrun) | the step did not run. Never a clean result (Law 1) |
| [`CRASH`](VERDICTS.md#verdict-crash), [`SANFAIL`](VERDICTS.md#verdict-sanfail) | a fuzzer or sanitizer saw the defect happen |
| [`CLEAN`](VERDICTS.md#verdict-clean) | a fuzzer found nothing. Not a proof (Law 3) |
| [`HYPOTHESIS`](VERDICTS.md#verdict-hypothesis), [`READS`](VERDICTS.md#verdict-reads) | model output or advice: a lead, never evidence (Law 4) |

A proof covers the properties PRISM inserts (signed overflow, division by
zero, shift range, memory safety, the code's `assert`s) for the function as
Clang compiled it. [CONFORMANCE.md](CONFORMANCE.md) lists the known wrong
answers.

Pointer-parameter functions are never model-checked without a precondition
(Law 6). Write one `// requires:` per line at the top of the body. A proof
under them is `PROVED-ASSUMING` (see `testdata/planted/ptr_copy.c`):

```c
void copy(char *dst, char *src, int n)
{
    // requires: n >= 0
    // requires: n < 8
    for (int i = 0; i < n; i++) dst[i] = src[i];
}
```

`report.md` opens with `confidence = visibility × answer × resolution`
(Law 5). No data scores 0; 0 means "no data", not "clean". The exact
definition is in [VERDICTS.md](VERDICTS.md).

## 4. SARIF

`report.sarif` is SARIF 2.1.0. Only `FAILED`, `CRASH` and `SANFAIL` become
results. `HYPOTHESIS` becomes a note. Stages that did not run become tool
execution notifications, so a viewer shows what was not checked.

## 5. Exit codes and `--fail-on`

| `--fail-on` | exit 1 when |
|---|---|
| `never` (default) | never |
| `defect` | any `FAILED`, `CRASH` or `SANFAIL` at error level. A finding with `extra.severity` `warning`, `note` or `style` is reported but does not fail the run |
| `gap` | a defect, or anything `NOTRUN`, `ERROR` or `TIMEOUT` |

A stage that crashed is exit 2 whatever the policy.

## 6. Untrusted code and `--allow-exec`

PRISM does not run the code it checks unless you pass `--allow-exec`
(Law 9). Without it, sanitizer runs, compiled fuzz and diff harnesses,
AFL++, libFuzzer, KLEE, `perl -c`, `cargo clippy`, `eslint`, Coccinelle
script rules and model-written programs are `NOTRUN` with the hint
`re-run with --allow-exec (only on code you trust)`. Parsing, lints, `pir`,
the in-process interpreter and compile-only diagnostics still run.

With `--allow-exec`, scanned code runs in a bubblewrap jail when `bwrap`
works: a read-only system view, a private scratch directory, no network.
Each finding records the sandbox in `extra.sandbox`. The sanitize stage
calls only zero-argument functions you mark with `// prism: run`.

Branch port/harden (landing) makes the jail narrower (no `/`, no home
directory). Where bwrap cannot run, it keeps those steps `NOTRUN` unless
you pass `--allow-exec=unsandboxed`, which accepts resource limits only.
The per-stage table is in
[ARCHITECTURE.md](ARCHITECTURE.md#running-on-untrusted-code-law-9).

## 7. Other languages

The `polyglot` stage checks every non-C file with the usual tool for its
language: Python (`compile()`, ruff or pyflakes, mypy), JSON, TOML,
JavaScript (`node --check`, eslint), TypeScript (tsc), shell (`bash -n`,
shellcheck), Go (gofmt), Rust (cargo clippy), Ruby, PHP, Perl, Lua and YAML
(yamllint). Every text file is also scanned for merge-conflict markers and
leaked credentials. A linter that ran and said nothing is `UNKNOWN`, not
clean. Files with broken syntax are kept from the type checkers.

```
./build/prism . --no-llm --stage inventory,lints,polyglot
```

## 8. Certified proofs

A plain `PROVED` trusts the solver. With `--certified`, `pir` bit-blasts
every verification condition to CNF, CaDiCaL writes an LRAT proof that the
CNF is unsatisfiable, and the verified checker `cake_lpr` must accept it.
When the Lean tools are built (`lake build` in `proofs/techniques`), the CNF
comes from the Lean-proved bit-blaster and Lean's LRAT checker must accept
the proof too. A function is `PROVED-CERTIFIED` only when every one of its
conditions passes. Otherwise it stays `PROVED` and `extra.certify_note`
says why. `BOUNDED` and `PROVED-UNBOUNDED` are never certified.

```
build/prism-deps tool cadical cake_lpr      # once
(cd proofs/techniques && lake build)        # optional: the Lean bit-blaster
./build/prism src/ --stage inventory,classify,pir --certified
```

What a certificate still trusts is in [TRUSTED_BASE.md](TRUSTED_BASE.md).

## 9. CI

A GitHub Actions job that fails on defects and uploads SARIF:

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
          sudo apt-get update && sudo apt-get install -y ninja-build
          git clone --depth 1 <your PRISM mirror> prism-src
          L=$(python3 prism-src/scripts/fetch_llvm.py --dest "$HOME/.cache/prism/tools")
          cmake -S prism-src -B prism-build -G Ninja \
            -DCMAKE_C_COMPILER=$L/clang -DCMAKE_CXX_COMPILER=$L/clang++ \
            -DPRISM_LLVM_PINNED_BIN=$L -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
          cmake --build prism-build --target prism
      - name: Check
        run: prism-build/prism . --no-llm --fail-on defect --out prism-out
      - uses: github/codeql-action/upload-sarif@v3
        if: always()
        with:
          sarif_file: prism-out/report.sarif
```

- Use `--fail-on gap` when "not checked" must fail the build too.
- Leave `--allow-exec` off for pull requests from forks.
- Cache the build directory or use ccache: the first build compiles Z3.

<a id="environment-variables"></a>

## 10. Environment variables

The list is `prism::env_vars()` in `src/prism/config.cpp`. A `PRISM_*`
variable read anywhere else fails `tests/cpp/test_env_registry.cpp`. The
table below is generated by `scripts/gen_doc_tables.sh`; do not edit it by
hand.

<!-- generated:env-vars -->
| variable | meaning |
|---|---|
| `CC_SAN` | qa replay: C compiler for sanitizer builds |
| `CXX_SAN` | qa replay: C++ compiler for sanitizer builds |
| `MALLOC_ARENA_MAX` | glibc; if unset PRISM caps arenas at 2 |
| `PRISM_AFL` | fuse: 1 = use afl-fuzz (when on PATH) for the FuSeBMC fuzz oracle |
| `PRISM_BIN` | path of the prism executable used by tools and tests |
| `PRISM_CERTIFY_BUDGET` | certified mode: per-function certification seconds |
| `PRISM_CERT_CACHE_MAX` | maximum entries kept in the proof-certificate cache |
| `PRISM_CHECKER_MEM` | memory cap (MB) for external certificate checkers |
| `PRISM_CONF_KEEP` | qa conformance/soundness: keep per-task scratch dirs |
| `PRISM_DEVICE` | FORGE device for the models (cpu, cuda, vulkan, ...) |
| `PRISM_EMBED_FPK` | FORGE embedding model (.fpk) used by ai triage |
| `PRISM_FPK` | FORGE assistant model (.fpk), default ~/.prism/models/qwen3.5-9b.fpk |
| `PRISM_FUNCTION_BUDGET` | per-function time budget (seconds) |
| `PRISM_HOST_PYTHON3` | z3gen: host Python that non-Z3 build scripts are forwarded to (build time) |
| `PRISM_HOUDINI_BUDGET` | pir: run-wide Houdini loop-invariant seconds (0 = none) |
| `PRISM_HOUDINI_FINAL_RESERVE` | pir: fraction of the function budget Houdini keeps for the final query (0..0.9, default 0.3) |
| `PRISM_HOUDINI_TRACE` | diagnostics: one stderr line per Houdini goal query |
| `PRISM_LAKE` | path of Lean's lake for proof search |
| `PRISM_LEAN_PAD_DEBUG` | diagnostics: trace Lean export padding to stderr |
| `PRISM_LIBFUZZER` | fuse: 1 = use libFuzzer (pinned LLVM) for the FuSeBMC fuzz oracle |
| `PRISM_LINUX_SYSROOT` | pir off Linux: pinned glibc/libstdc++ sysroot for the Linux target (else scripts/fetch_toolchain.py linux-sysroot) |
| `PRISM_LLVM_BIN` | directory of clang/opt/lli for pir (before the pinned LLVM build) |
| `PRISM_MEM_BUDGET_MB` | process-tree memory budget (MB); 0 = unlimited |
| `PRISM_MEM_RESERVE_MB` | memory held back from the budget for the main process |
| `PRISM_MEM_STUCK_MS` | time over the hard limit before the stuck hook fires |
| `PRISM_MEM_TRACE` | diagnostics: memory-budget, Z3 and pir memory trace lines on stderr |
| `PRISM_MODEL` | default LLM model name |
| `PRISM_PBSD` | path of the pbsd tool |
| `PRISM_PIR_BATCH` | pir: functions per fresh-process batch |
| `PRISM_PIR_DEEPEN_MAX` | pir: largest unwind a BOUNDED single loop is re-checked at (default 64; 0 = off) |
| `PRISM_PIR_DIVERT_S` | pir: seconds a function gets in the main process before an UNKNOWN is re-checked in a child (default off) |
| `PRISM_PIR_DUMP_INVARIANTS` | diagnostics: 1 = report Houdini survivors of a search that did not prove (pir_invariants_surviving) |
| `PRISM_PIR_EXPORT_ONLY` | pir: with PRISM_PIR_LEAN_EXPORT, only export Lean, skip the checks |
| `PRISM_PIR_LEAN_EXPORT` | pir: directory to export Lean obligations to |
| `PRISM_PIR_MEM_MB` | pir: memory budget (MB) when PRISM_MEM_BUDGET_MB is unset |
| `PRISM_PIR_NO_HEADERS` | pir: skip the system-header IR pass (diagnostics) |
| `PRISM_PIR_ONLY` | internal: set in pir rerun child processes (the units and functions to re-check) |
| `PRISM_PIR_PROGRESS` | pir: file to append lower-start/done progress lines to |
| `PRISM_PROOF_CACHE` | directory of the proof cache |
| `PRISM_PROVER_FPK` | path of the FORGE prover model (.fpk) |
| `PRISM_PROVER_MODEL` | prover LLM model name |
| `PRISM_SOLVER_MODEL` | path of the solver-prediction model |
| `PRISM_SOLVER_PREDICT` | enable/disable solver prediction |
| `PRISM_TOOLS_DIR` | directory of vendored external tools |
| `PRISM_TRIAGE` | enable LLM triage |
| `PRISM_VERSION` | override the reported version string |
| `PRISM_Z3_GENERATED_ROOT` | z3gen: output root for generated Z3 sources (build time) |
| `PRISM_Z3_WORKER` | path of the executable used for killable Z3 child queries |
| `SOURCE_DATE_EPOCH` | reproducible timestamps in reports |
| `XDG_CACHE_HOME` | cache root |
<!-- /generated:env-vars -->
