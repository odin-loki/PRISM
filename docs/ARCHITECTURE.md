# PRISM architecture

PRISM is one C++23 program, `prism`, that runs a fixed sequence of stages
over a source tree and writes one report. This page says what each stage
does, how stages hand results on, and which stages may run scanned code.

The laws the pipeline must keep are in [CLAUDE.md](../CLAUDE.md) and are
defined with every verdict in [VERDICTS.md](VERDICTS.md).

## Method

The order is the method. Cheap deterministic instruments run first. Then
come proofs, then dynamic testing, then the language model, which may only
propose. Each later stage uses what the earlier ones found: classify tells
pir which functions take pointers, pir's counterexamples seed the fuzzer,
and repair is scored by pir.

## Code layout

| Layer | Where |
|---|---|
| Entry point and subcommands | `src/prism/main.cpp`, `src/prism/cli.cpp`, `src/prism/cli_svcomp.cpp` |
| Stage order | `include/prism/pipeline.hpp` (`STAGE_ORDER`) |
| Orchestration, confidence, report.md | `src/prism/pipeline.cpp` |
| Status vocabulary and laws | `include/prism/laws.hpp`, `src/prism/laws.cpp` |
| Verdict lattice (pure, no I/O) | `src/prism/verdict/`, `include/prism/verdict.hpp` |
| Stages with their own file | `src/prism/stages/` (shared helpers in `common`, `interp`, `llm`, `platform`) |
| Lints | `src/prism/checkers_*.cpp` (regex layer), `src/prism/astlint*.cpp` (Clang AST layer) |
| C/C++ source parser for the lints | `src/prism/cparse.cpp` |
| Defect taxonomy | `src/prism/taxonomy.cpp` |
| External tool adapters | `src/prism/adapters.cpp`, `src/prism/polyglot.cpp` |
| Model checker | `src/prism/pir/` (Clang/LLVM IR to PIR to Z3; [PIR.md](PIR.md)) |
| Threads | `src/prism/conc/` ([CONCURRENCY.md](CONCURRENCY.md)) |
| Solver portfolio and certificates | `src/prism/solver/` ([SOLVERS.md](SOLVERS.md)) |
| Language model features | `src/prism/ai/`, `include/prism/forge_llm.hpp` ([AI.md](AI.md)) |
| Sandbox and the Law 9 gate | `src/prism/sandbox.cpp`, `include/prism/sandbox.hpp` |
| Memory budget | `src/prism/membudget.cpp` ([MEMORY_RULES.md](MEMORY_RULES.md)) |
| Reports | `src/prism/pipeline.cpp` (report.md), `src/prism/sarif.cpp`, `src/prism/shipdocs.cpp`, `src/prism/journal.cpp` |
| SV-COMP front end | `src/prism/svcomp/` ([SVCOMP.md](SVCOMP.md)) |
| Qt GUI | `src/gui/` (`prism_gui`, CMake `-DPRISM_QT=ON`) |
| CUDA mutator | `src/cuda/` (CMake `-DPRISM_CUDA=ON`) |
| Developer tools | `src/tools/qa/` (`prism-qa`), `src/tools/prism_deps/` (`prism-deps`), `src/tools/prism_ai/` (`prism_ai`) |
| Lean proofs | `proofs/` ([PROOFS_SEMANTICS.md](PROOFS_SEMANTICS.md), [PROOFS_TECHNIQUES.md](PROOFS_TECHNIQUES.md), [PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md)) |

FORGE (odin-loki/FORGE) supplies the platform layer (logging, processes,
thread pool, settings) and the in-process language-model engine. It is
pinned by commit in `cmake/FORGE_PIN` ([SUPPLY_CHAIN.md](SUPPLY_CHAIN.md)).

## Stages

`prism --list-stages` prints the order. `--stage a,b` runs only the named
stages and `--skip a,b` leaves stages out. A stage left out is listed as
`skipped` in the report.

| stage | what it does |
|---|---|
| inventory | Lists files, translation units and functions in scope. Lists each skipped vendor/build directory that holds source as `UNKNOWN`. |
| classify | Sorts functions into SCALAR, POINTER, VOID and OTHER. POINTER functions are not model-checked without a precondition (Law 6). |
| lints | Pattern checks on C/C++: a regex layer (`checkers_*.cpp`) and a Clang-AST layer through libclang or `clang -ast-dump=json` (`astlint*.cpp`). |
| taint | Source-to-sink taint through assignments and copies. |
| thread | File globals written without a mutex in files that start threads. |
| interval | Path-sensitive integer ranges. A finding is `FAILED`; silence is not a proof. |
| warnings | Compiler diagnostics (`-fsyntax-only`). |
| cppcheck | The cppcheck adapter. |
| pbsd | ParanoidBSD checks: PRISM's portable copies, plus the ParanoidBSD tree when `--pbsd PATH` or `PRISM_PBSD` names one. |
| sanitize | Builds with ASan/UBSan/TSan and calls only functions marked `// prism: run`. |
| optional | External analysers when installed: clang-tidy, CBMC, Infer, Frama-C, Semgrep, Strix, KLEE, Coccinelle (`spatch`, tree-local `*.cocci` only). |
| polyglot | Every non-C language: syntax checks, linters and type checkers, plus a conflict-marker and leaked-credential scan of every text file. |
| esbmc | The ESBMC adapter. |
| dafny | The Dafny adapter. |
| contracts | Proves `requires`/`ensures`/`invariant`/`decreases` comments with the PIR checker. A proof under `requires` is `PROVED-ASSUMING`. |
| wp | ACSL `ensures` by weakest precondition. ACSL it cannot encode is `ERROR`, never `PROVED-ASSUMING`. |
| pir | The model checker: Clang to LLVM IR to PIR to Z3, with bounded checking, k-induction and Houdini invariants ([PIR.md](PIR.md)). |
| conc | Threads by lazy sequentialisation over the PIR memory model ([CONCURRENCY.md](CONCURRENCY.md)). It never proves; its best answer is `BOUNDED`. |
| harness | Checks POINTER functions with PIR under the preconditions their `// requires:` lines give. |
| review | Audits every `requires` for vacuity with Z3, applies approved contracts, drafts new ones as `HYPOTHESIS`, and keeps the proof store ([AI.md](AI.md)). |
| concolic | Concrete runs in the in-process interpreter, with branch flips. |
| fuzz | In-process fuzzing with the concrete oracle; with `--allow-exec`, compiled harnesses, AFL++ (`PRISM_AFL=1`) and libFuzzer (`PRISM_LIBFUZZER=1`). `CLEAN` is not a proof (Law 3). |
| diff | Differential testing of same-named function pairs. |
| rapid | Property trials of comment contracts. |
| muttest | Mutation testing of those contracts. |
| ltl | LTL safety properties on state machines extracted from `switch` statements. |
| llm | Model hypotheses. Always `HYPOTHESIS` or `READS` (Law 4). |
| execute | Concrete replay of counterexamples; with `--allow-exec`, model-written programs. |
| repair | Model-written patches, scored by running them and by pir. The verdict is about the patch. |
| unify | Merges results, computes confidence (Law 5) and taxonomy coverage, and writes the reports. |

A stage that cannot run says so with a `NOTRUN` row and an install hint
(Laws 1 and 7). An external tool's output that PRISM cannot parse is
`ERROR "output not understood"`.

## Results and reports

Every stage returns findings with a status from the closed vocabulary in
`include/prism/laws.hpp`. Each stage appends one line to
`--out/stages.jsonl` when it finishes.

`--resume` reuses stages that finished `ok` or `NOTRUN` in `stages.jsonl`
(`report.json` as fallback). Branch port/harden adds a run key:
`--out/run_key.json` holds a SHA-256 digest of the scanned tree and the
options that change results. When the key no longer matches, the stages the
change affects run again and the report notes say so.

At the end the run writes, under `--out` (default `prism-out/`):

| file | contents |
|---|---|
| `report.md` | human report: confidence, stage table, findings |
| `report.json` | full machine report |
| `report.sarif` | SARIF 2.1.0 |
| `stages.jsonl` | one line per finished stage |
| `functions.json` | the inventory |
| `taxonomy.json` | each defect class as COVERED, PARTIAL or GAP |
| `triage.json` | root-cause clusters; ordering only, never a status change |
| `TRUSTED_BASE.md`, `VERDICTS.md` | copies of [TRUSTED_BASE.md](TRUSTED_BASE.md) and [VERDICTS.md](VERDICTS.md), embedded in the binary at build time |
| `ai_audit.jsonl` | every model call, when the model ran |
| `proof_store.json` | proofs kept for the review stage's regression check |

Confidence is `visibility × answer × resolution` (Law 5). Its exact
definition is in [VERDICTS.md](VERDICTS.md).

## Running on untrusted code (Law 9)

Reading, parsing, compiling and model checking scanned code is safe.
Running it is not: a Perl `BEGIN` block, a Cargo `build.rs` or an
`eslint.config.js` runs with the user's rights. So every step that runs
code from the scanned tree, or code a model wrote, needs `--allow-exec`
(`Config.allow_exec`, default off).

Without the flag the step does not run. It writes `NOTRUN` with the message
`<step>: executes code from the scanned tree; re-run with --allow-exec (only
on code you trust)` and `extra.reason = "executes-scanned-code"`. A stage
marked "part" below keeps its analysis half and adds one such row for the
held-back half (`exec_gate_note`).

The list of stages that may run code is `exec_stages()` in
`src/prism/pipeline.cpp`. The gate is `src/prism/sandbox.cpp`.

With the flag, scanned code runs in a sandbox. `sandbox::kind()` names it,
and findings record it in `extra.sandbox`:

| kind | where | what it does |
|---|---|---|
| `bwrap` | Linux with a working bubblewrap | Isolates. The program sees a read-only view of the system and a private scratch directory. It has no network. |
| `job-object` | Windows | Resource limits only: a kill-on-close Job Object with memory and CPU caps. |
| `rlimits-only` | other POSIX | Resource limits only: CPU time (timeout + 1 s) and address space (not for ASan/TSan builds). |

On main (991c4eafa) the Windows kind is `none`, the bwrap jail binds `/`
read-only, and the step runs under any kind. Branch port/harden, which is
landing, gives the table above and changes this:

- The jail binds only `/usr`, the `/bin`, `/sbin` and `/lib*` entries, a
  minimal `/etc`, the LLVM toolchain, the `prism-deps` tool home and the
  program's own install prefix. It never binds `/` or the home directory.
- Only `bwrap` counts as isolating (`sandbox::isolates`). Where bwrap cannot
  run, `--allow-exec` is held back and the steps stay `NOTRUN` with an
  install hint, unless the user passes `--allow-exec=unsandboxed`
  (`Config.allow_unsandboxed`) to accept resource limits only.
- A jailed child killed by a signal is a crash, not a clean run.

The sanitize stage never calls an arbitrary function, even with the flag. It
calls only a zero-argument, non-static function marked `// prism: run`.

| stage | runs scanned code? | without `--allow-exec` | with `--allow-exec` |
|---|---|---|---|
| inventory, classify, lints, taint, thread, interval, warnings | no | runs | runs |
| cppcheck, esbmc, dafny | no (static analysers) | runs | runs |
| pbsd (part) | PRISM's portable checks: no. The named ParanoidBSD tree: yes, its `tools/verify` modules are imported | portable checks run; the tree half is `NOTRUN` | tree modules imported |
| sanitize (whole) | yes: compiles and runs the unit | `NOTRUN` | calls `// prism: run` functions in the sandbox |
| optional (part) | KLEE (external calls run natively) and `spatch` rules with `@script:`/`@initialize:`/`@finalize:` blocks: yes. The others: no | KLEE and script rules `NOTRUN`; the rest run | KLEE in the sandbox; script rules run |
| polyglot (part) | `perl -c`, `cargo clippy`, `eslint`: yes. Every other checker: no | those three `NOTRUN`; the rest run | those three run |
| contracts, wp, harness, review | no (in-process Z3) | runs | runs |
| pir (part) | Clang/opt compile and in-process Z3: no. Translation validation with `lli`: yes | verdicts run; validation `NOTRUN` | `lli` in the sandbox |
| conc | no | runs | runs |
| concolic | no (in-process interpreter) | runs | runs |
| fuzz (part) | concrete oracle: no. Compiled harness, AFL++, libFuzzer: yes | oracle runs; the binary half `NOTRUN` | binary half in the sandbox |
| diff (whole) | yes: compiles and runs both functions | `NOTRUN` | sandbox |
| rapid, muttest (part) | interpreter: no. Compiled fallback harness: yes | interpreter runs; fallback `NOTRUN` | fallback in the sandbox |
| ltl | no | runs | runs |
| llm | no (the model reads source) | runs | runs |
| execute (part) | counterexample replay: no. Model-written C: yes | replay runs; the model half `NOTRUN` | model half in the sandbox |
| repair (whole) | yes: compiles and runs model-written candidates | `NOTRUN` | sandbox |
| unify | no | runs | runs |

`prism prove` elaborates Lean, which runs code, so it needs `--allow-exec`
too. `prism svcomp` replays a counterexample by running the task, so
without the flag every refutation is `unknown`.

On Windows, adapters start processes with `CreateProcessW` and a quoter
tested for `CommandLineToArgvW`, never through `cmd.exe`. A `.bat` or `.cmd`
target is refused when an argument holds `%`, `!`, `"` or a newline.
