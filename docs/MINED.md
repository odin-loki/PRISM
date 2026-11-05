# What PRISM took from each tool

PRISM re-implements techniques from other analysers, and runs some of
those tools as adapters when they are installed. This page lists, per
tool, what PRISM uses and where that code lives.

The mined source trees were deleted from `third_party/` (2026-09-23, commit
a0bcb82fd). External tools are pinned by commit in
`third_party/MANIFEST.toml` and built on demand with `prism-deps tool NAME`
([SUPPLY_CHAIN.md](SUPPLY_CHAIN.md)). An adapter finds its tool through
`--tool NAME=PATH`, then the pinned build under
`~/.prism/tools/<name>/<commit>/`, then `PATH` (`Config::which_adapter`,
`src/prism/config.cpp`). A missing tool is `NOTRUN` (Law 1). A tool that
answers but reports nothing is `UNKNOWN`, never `CLEAN` or `PROVED`.

Status: **implemented** (PRISM's own code), **adapter** (PRISM runs the
tool and maps its output), **not implemented**, **removed**.

## Model checking and proof

| Tool | Technique PRISM uses | Where | Status |
|---|---|---|---|
| ESBMC | Bounded model checking and k-induction over the program. PRISM's own engine is `pir` (Clang/LLVM IR → PIR → Z3); see [PIR.md](PIR.md). | `src/prism/pir/` | implemented (`pir` stage) |
| ESBMC | Adapter: `esbmc FILE --unwind N --overflow-check --memory-leak-check`. `VERIFICATION SUCCESSFUL` with an unwinding assertion is `BOUNDED`; with induction it is `PROVED-UNBOUNDED`; otherwise `PROVED`. Never merged with PRISM's own verdict (Law 2). | `src/prism/adapters.cpp` `run_esbmc` | adapter (`esbmc` stage) |
| ESBMC | Flag polarity: ESBMC turns checks *off* with `--no-…-check`. PRISM refuses any such argument before it starts a process (Law 8). | `src/prism/forge_services.cpp` `exec::refuse_disabled_checks` | implemented |
| ESBMC | The old hand-written C → Z3 encoder (`bmc` stage) | — | removed 2026-10-31 (commit c66284cde); `pir` does all bounded model checking |
| CBMC | Adapter: `VERIFICATION SUCCESSFUL` is `BOUNDED` (unwind-limited), never `PROVED`. | `src/prism/adapters.cpp` `run_cbmc` | adapter (`optional` stage) |
| Dafny | Contract shape `requires` / `ensures` / `invariant` / `decreases` in comments. `requires` is assumed, `ensures` asserted at every return, `invariant` and `decreases` instrumented around loops; checked by `pir`. A proof is `PROVED-ASSUMING`. | `src/prism/stages/contracts.cpp` | implemented (`contracts` stage) |
| Dafny | LLM-proposed Dafny-style specs are `HYPOTHESIS` / `READS` (Law 4). | `src/prism/stages/hypothesize.cpp` `dafny_specs` | implemented |
| Dafny | Adapter: `dafny verify` on `.dfy` files in the tree. | `src/prism/adapters.cpp` `run_dafny` | adapter (`dafny` stage) |
| Dafny | Translating C into Dafny | — | not implemented |
| Frama-C WP | Weakest precondition by return substitution: VC `(requires) ==> (ensures[result := expr])`. A tautology closes as `PROVED-ASSUMING` without the solver (QED); otherwise `pir` checks it under `requires`, and any proof is rewritten to `PROVED-ASSUMING`. Unencodable ACSL (`\valid`, `\old`, quantifiers, `->`, calls, …) is `ERROR`. Pointer or non-scalar functions are `NEEDS-HARNESS`. | `src/prism/stages/wp.cpp`; ACSL parsing in `src/prism/stages/contracts.cpp` | implemented (`wp` stage) |
| Frama-C EVA | Adapter: `0 alarm` is `UNKNOWN`; alarms are `FAILED`. | `src/prism/adapters.cpp` `run_frama_c_impl` | adapter (`optional` stage) |
| Strix | LTL on a finite machine extracted from `switch (state)`. Decides `G p`, `G (p → X q)`, `G (req → F_k ack)`, `G (F_k p)`. Liveness (`GF`, `FG`, `U`, unbounded `F`) is cut to a bound of 8 (`F_BOUND`) and success is `BOUNDED`. No spec, a formula outside the fragment or no FSM is `NOTRUN`. Missing `G (p → X q)` edges are proposed as `HYPOTHESIS`. | `src/prism/stages/ltl.cpp` | implemented (`ltl` stage) |
| Strix | Adapter: a found `strix` is run and recorded; its realizability answer is never `PROVED`. | `src/prism/adapters.cpp` `run_strix` | adapter |
| Strix | Parity-game synthesis of a Mealy machine | — | not implemented |

## Testing and fuzzing

| Tool | Technique PRISM uses | Where | Status |
|---|---|---|---|
| FuSeBMC | Branch goals numbered `GOAL_1`, `GOAL_2`, … per function. Checker counterexamples become fuzz seeds. An uncovered goal is checked as `if (!(cond)) return 0;` + body by `pir`; its counterexample becomes a new seed. Goals are seed sources, never checks; `CLEAN` is not a proof (Law 3). | `src/prism/stages/fuse.cpp` `numbered_goals`, `seeds_from_findings`, `run_fuse` | implemented (`fuzz` stage) |
| AFL++ | Havoc mutation of the input bytes (CUDA kernel when built, CPU otherwise). | `src/prism/havoc.cpp`, `src/cuda/mutate.cu` | implemented |
| AFL++ | Adapter: a bounded `afl-fuzz` campaign on a scalar stdin harness. A crash is `CRASH`; no crash is `CLEAN`, not a proof. Needs `--allow-exec`. | `src/prism/stages/fuse.cpp` `run_afl_fuzz` | adapter |
| libFuzzer | A `clang -fsanitize=fuzzer` harness per scalar function; probe in the `optional` stage. | `src/prism/stages/fuse.cpp`, `src/prism/adapters.cpp` `libfuzzer_probe` | adapter |
| Fuzz4All | The LLM writes a fuzzing prompt from the function and its documentation comments, generates seeds, and mutates interesting ones (generate / mutate / semantic / combine). Prompts are scored by distinct seeds. The output is seeds, never a verdict. | `src/prism/stages/fuse.cpp`; `prism/stages.hpp` `create_prompt_from_source`, `score_prompt_seeds`, `fuzz4all_update_strategy` | implemented |
| ChatFuzz | When a round finds no new coverage, the LLM is asked once for type-aware mutants of the current seed. With no model, `extra.chatfuzz` is `NOTRUN` and the fuzzer continues. | `src/prism/stages/fuse.cpp` `chatfuzz_mutants` | implemented |
| KLEE | Concolic execution: concrete runs, then each branch is flipped. The flipped condition becomes a reachability question for `pir` (KLEE `Executor::fork`); a model gives new arguments, a proof of unreachability drops the side. | `src/prism/stages/concolic.cpp`, `src/prism/stages/fork_flip.cpp` | implemented (`concolic` stage) |
| KLEE | Adapter: no error path is `UNKNOWN`. | `src/prism/adapters.cpp` `run_klee` | adapter (`optional` stage) |
| RapidCheck | Property trials from `requires` / `ensures`; a counterexample is shrunk toward 0, then half, then ±1 (`extra.shrinks`). Still a counterexample, not a proof. | `src/prism/stages/rapid.cpp` | implemented (`rapid` stage) |
| (mutation testing) | One operator flipped per mutant (`+`→`-`, `<`→`>`, `==`→`!=`), the same trials re-run. Killed is `CLEAN`, not a proof of the original; survived is `FAILED`. | `src/prism/stages/rapid.cpp` `run_muttest` | implemented (`muttest` stage) |
| (differential testing) | Two functions (`*_a` / `*_b`, or `// diff: other`) built and run on the same bytes; disagreement is `FAILED`. Needs `--allow-exec`. | `src/prism/stages/diff.cpp` | implemented (`diff` stage) |
| ASan / UBSan / TSan | Sanitize stage: the compiler is probed by making UBSan fire on planted signed overflow (accepting the flag is not enough). Only `// prism: run` functions are run (Law 9). `unexpected memory mapping` is the runtime failing, `NOTRUN`, not `FAILED`. Exit 0 is `CLEAN`, not a proof. | `src/prism/adapters.cpp` `run_sanitize` | implemented (`sanitize` stage) |

## LLM loops

LLM output is `HYPOTHESIS` / `READS` and cannot cover a defect class
(Law 4). Inference is FORGE, in process ([AI.md](AI.md)).

| Tool | Technique PRISM uses | Where | Status |
|---|---|---|---|
| RLEF | Repair loop with execution feedback: candidate patch → compile, run, sanitizer, `pir`. `pir` `FAILED` is negative reward; a `pir` proof of the patch ends the loop and stays a claim about the patch. No training. | `src/prism/stages/rlef.cpp` | implemented (`repair` stage) |
| OpenCodeInterpreter | The model writes a C `main()`, the sandbox compiles and runs it, the output goes back to the model. Runs only with `--allow-exec`. | `src/prism/stages/execute_cex.cpp` `interpreter_loop`; sandbox run in `src/prism/stages/llm.cpp` | implemented (`execute` stage) |

## Lints and static analysers

| Tool | Technique PRISM uses | Where | Status |
|---|---|---|---|
| ParanoidBSD | The bug shapes its `tools/verify` scanners found (lock released on some returns only, `p = realloc(p, n)`, one-sided index, NULL-branch use, masked switch, sibling guard, …) as PRISM lints with taxonomy classes. | `src/prism/checkers_core.cpp`, `src/prism/astlint_checks.cpp`, `src/prism/taxonomy.cpp` | implemented (`lints` stage) |
| ParanoidBSD | `pbsd` stage: with a tree (`--pbsd PATH` / `PRISM_PBSD`) it restages the matching checker classes; the `tools/verify` Python modules are not imported (`extra.ported` lists them). No tree: only the portable classes. | `src/prism/adapters.cpp` `run_pbsd_lints` | implemented (`pbsd` stage) |
| Clang | Clang-AST lints: libclang loaded at run time, else `clang -fsyntax-only -Xclang -ast-dump=json` per unit. The regex "return is discarded" family is one generated table. No clang, or a parse error, is a `NOTRUN` layer row and the regex lints still run. | `src/prism/astlint*.cpp`; `src/tools/gen_astlint_discard.cpp` | implemented (`lints` stage) |
| gcc / clang | `-Wall` warnings from both compilers, unioned. | `src/prism/adapters.cpp` `run_compiler` | adapter (`warnings` stage) |
| cppcheck | Adapter: `--enable=warning,style,performance,portability --xml`. | `src/prism/adapters.cpp` `run_cppcheck` | adapter (`cppcheck` stage) |
| clang-tidy | Adapter: no diagnostics is `UNKNOWN`; hits are `FAILED`. | `src/prism/adapters.cpp` `run_clang_tidy` | adapter (`optional` stage) |
| Semgrep | Adapter: a real scan; no results is `UNKNOWN`. | `src/prism/adapters.cpp` `run_semgrep` | adapter (`optional` stage) |
| Infer | Adapter: `infer run` into a scratch results dir. | `src/prism/adapters.cpp` `run_infer` | adapter (`optional` stage) |
| Coccinelle | Adapter: `spatch` with the `*.cocci` rules found in the scanned tree; no match is `UNKNOWN`. Script rules execute code and need `--allow-exec`. | `src/prism/adapters.cpp` `run_spatch` | adapter (`optional` stage) |
| CodeQL | Adapter | — | removed (the CodeQL engine's terms restrict commercial use) |

Stage order is `include/prism/pipeline.hpp` `STAGE_ORDER`; which stages
execute scanned code is `exec_stages()` in `src/prism/pipeline.cpp`.
