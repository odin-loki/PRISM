# PRISM AI layer

What the language model does in PRISM, what checks its output, and what runs
without it. Code: `src/prism/ai/`, `include/prism/ai.hpp`,
`include/prism/ai_proof.hpp`, `include/prism/ai_assist.hpp`,
`include/prism/forge_llm.hpp`, `src/prism/stages/llm.*`; grammars:
`grammars/*.gbnf`; developer tool: `src/tools/prism_ai/`.

## The rule

**The model proposes; a checker decides.** Every model output is
`HYPOTHESIS` or `READS` (Law 4). It cannot cover a defect class. It can
change a verdict only through a checker (Z3, PIR, the contracts engine, the
Lean kernel), and the finding then records the model, the prompt and the
checker (`extra.ai_audit_id` points at a line of `<out>/ai_audit.jsonl`).

The verdict audit enforces this by origin: the `llm` and `repair` stages
have origin `model`, and a model origin may never hold a proof status
(`src/prism/verdict/verdict.cpp`, `may_prove`; [VERDICTS.md](VERDICTS.md)).

| Feature | What the model may propose | Who decides | Best result |
|---|---|---|---|
| Loop invariants | C boolean expressions | Z3: Houdini filter, then the loop-cut induction (base and step) | `PROVED-UNBOUNDED` |
| Harness drafting | non-NULL, element counts, integer ranges | PIR on the drafted harness | `NEEDS-HARNESS` with `draft_verdict=PROVED-ASSUMING` and the `// requires:` lines to confirm |
| Contract drafting | `requires` / `ensures` clauses | contracts engine (PIR), Z3 vacuity, caller check | `HYPOTHESIS` until a human approves; approved clauses give `PROVED-ASSUMING` |
| Counterexample explanation | text and a replacement body | PIR on the patched function | `HYPOTHESIS` / `READS`, labelled "verified fix" or "unverified suggestion" |
| Lean proof search | tactic lines | Lean kernel, `#print axioms`, `lake build` | `PROVED` (for the Lean theorem) |
| Hypotheses (`llm` stage) | suspected defects | nobody: text | `HYPOTHESIS` / `READS` |

Without a model nothing is skipped quietly (Law 7). The deterministic halves
run (template invariants, template harnesses, Z3 vacuity, approved contracts,
proof-regression detection, triage, `ask`, `draft`, `regress`). Every model
half that could not run is a `NOTRUN` row or note that names `PRISM_FPK`.

## The model and the engine

PRISM has one model: **Goedel-Prover-V2-32B**, Q4_K_M quantisation,
converted to a FORGE `.fpk` with `forge convert`. It serves the assistant
features and Lean proof search.

Inference is FORGE, in process (`src/prism/ai/forge_llm.cpp`,
`include/prism/forge_llm.hpp`). There is no server and no other backend.

| Setting | Source | Default |
|---|---|---|
| Model file | `PRISM_FPK` | `~/.prism/models/goedel-prover-v2-32b.fpk` (`%USERPROFILE%\.prism\models\...` on Windows) |
| Model name in the audit | `PRISM_MODEL` | `goedel-prover-v2-32b` |
| Device | `PRISM_DEVICE`, else `FORGE_DEVICE` | `cpu` |
| Lean prover model | `--prover-fpk`, `PRISM_PROVER_FPK` | the `PRISM_FPK` model |
| Triage embedding model | `PRISM_EMBED_FPK` | none (TF-IDF) |
| Switch the model off | `--no-llm` | model on when the `.fpk` exists |

The full list is the generated table in
[USER_GUIDE.md](USER_GUIDE.md#environment-variables).

How `forge_llm::complete` runs a request:

* **Model cache.** A model is opened once per path and kept until the process
  ends. It is never destroyed, because tearing down a CUDA model during static
  destruction aborted with a double free. Each call gets a fresh session.
* **Context.** `min(model context, 8192)` tokens. A prompt that does not fit
  is an error. The reply may use what the prompt leaves, capped by
  `Request::max_new`.
* **Chat template.** ChatML (`<|im_start|>role ... <|im_end|>`) plus the
  assistant opener. `<think>` and `</think>` are single vocabulary tokens in
  the model, but the converted tokenizer does not mark them special. So the
  opener is built from their vocabulary ids (`encode_opener`), not encoded as
  text.
* **Thinking.** `Request::think = false` (the default) closes an empty
  `<think></think>` block, so the model answers at once. `think = true` lets
  the model reason freely, without the grammar, until it emits `</think>` or
  reaches `think_max` tokens (default 4096). At the cap PRISM closes the block
  itself (`Reply::thinking_capped`). The answer is then decoded under the
  grammar. The reasoning goes to `Reply::thinking` and is never parsed as the
  answer. No pipeline feature turns thinking on today: the assistant backend
  and the Lean prover backend both run with `think = false`.
* **Grammar.** The request's GBNF goes through `lower_grammar` and is then
  decoded under `forge::inference::grammar_constraint`, which masks every
  token the grammar cannot accept (see [Grammars](#grammars-41)).
* **Timeout.** Decoding stops with error `timeout` after `Request::timeout_s`
  (default 180 s).
* **Reply.** `text`, `error`, `thinking`, token counts (`prompt_tokens`,
  `new_tokens`, `thinking_tokens`), and wall times `load_s` (model open, first
  use only), `prefill_s` (prompt) and `decode_s` (generation).

Sampling settings per caller:

| Caller | Temperature | Max new tokens | Thinking |
|---|---|---|---|
| Assistant backend (`ai::connect_backend`: invariants, harness, contracts, assumption audit, explain, proof repair, `ask`, `draft`) | 0.2 | 768 | off |
| Lean prover backend (`resolve_prover`) | 0.6 | 1024 | off |
| Stage chat engine (`LlmEngine`: `llm`, `contracts` specs, fuzz seeds, `execute`, `repair`) | 0.2 | 768 | off |

A missing or non-`.fpk` path gives a `NOTRUN` row with an install hint
(`stages_detail::LLM_INSTALL`). `ai::connect_backend` returns no backend with
`--no-llm`.

## The `llm` stage and the other chat stages

The stage chat engine is `LlmEngine` (`src/prism/stages/llm.cpp`). It uses the
same FORGE model without a grammar. Every chat is an audit record with
`feature: "chat"`, `grammar: "none"`, `checker: "none"`.

| Stage | What the model does | Bound |
|---|---|---|
| `contracts` | Dafny-style `requires` / `ensures` / `invariant` / `decreases` for SCALAR and VOID functions (`dafny_specs`) | first 3 such functions; rows are `HYPOTHESIS` / `READS`, class `FUNC-CONTRACT` |
| `fuzz` | Fuzz4All-style seeds and mutants (`fuzz4all_seeds`, `chatfuzz_mutants`) | only when the fuzz stage runs; seeds are inputs, the fuzzer decides |
| `llm` | defect hypotheses per function (`hypothesize`) | first 4 functions, 6000 characters each |
| `execute` | a C `main()` that demonstrates the first FAILED/CRASH finding (`interpreter_loop`) | 2 rounds; needs `--allow-exec` |
| `repair` | RLEF: a corrected C file per round, scored by compile, run and PIR (`rlef_repair`) | `--repair-rounds` (default 3); needs `--allow-exec` |

`llm_forced_reads` (`src/prism/pipeline.cpp`) rewrites every `llm` row to
strength `READS` and to `HYPOTHESIS` unless it is already `NOTRUN`, `ERROR`,
`TIMEOUT`, `HYPOTHESIS` or `READS`. A reply with no hypotheses is still a
`HYPOTHESIS` row: silence is not a proof and does not cover a class.

**Law 9 for model-written programs.** `execute` and `repair` compile and run
C that the model wrote. `sandbox_run` refuses without `--allow-exec`
(`exec-disabled`), and the stage writes the `sandbox::exec_notrun` row.
With it, the program runs through `sandbox::wrap_argv` with the sandbox's
resource limits; the result records `sandbox::kind()`. `exec_stages()` lists
`execute` as `part` (the concrete counterexample replay runs; LLM-written C
does not) and `repair` as `whole`. See
[ARCHITECTURE.md](ARCHITECTURE.md#running-on-untrusted-code-law-9).

An RLEF candidate that PIR proves gets a second, checked audit record
(`checker: "pir(rlef candidate)"`). The repair row stays `HYPOTHESIS` with
`extra.patch_verdict` and `extra.fix_label = "verified fix"`; the `FAILED`
verdict does not change.

## Loop invariant synthesis and Houdini (4.2, 8.2 "Houdini")

Code: `src/prism/ai/invariants.cpp`, `src/prism/ai/program_check.cpp`.

`ai::strengthen_bounded` takes a `BOUNDED` function and tries to close it by
induction. The review stage's proof repair uses the same machinery
(`loop_cuts`, `houdini`) to re-check stored invariants and to accept model
invariants. The `pir` stage has its own Houdini and k-induction
([PIR.md](PIR.md)); this section is the AI layer's.

1. **Loop cut.** Every loop is replaced by

   ```
   init; assert(I) ; havoc(W) ; assume(I) ;
   step (cond) { body ; incr ; assert(I) }   // then continue under !cond
   ```

   `W` over-approximates everything the loop writes (scalars and whole
   arrays). Every reachable loop-head state satisfies `I`, so every body
   execution and every post-loop state of the real function is covered.
   Loops the cut cannot model soundly are refused and stay `BOUNDED`
   (`extra.invariants_attempt` says why): nested loops, `break` / `continue`
   / `goto` / `switch` in the body, calls, pointer declarations, side effects
   in the condition. Goto-built loops are first rewritten to `while` loops
   (`extra.goto_loops_rewritten`).
2. **Candidates.** `template_candidates` generates bounds from the loop
   condition, sign facts, array bounds, bounds against the function's integer
   literals, order relations and small linear relations. The cheap families
   run first; the extended set (non-linear and three-variable shapes) runs
   only when the cheap set leaves the step open.
3. **Houdini** (Z3): every candidate is asserted at loop entry and after the
   body, the conjunction is assumed at the head, and every candidate whose
   assertion can fail is dropped, to a fixpoint.
4. **Decision.** The cut program with the survivors is checked by
   `ai::check_program`, which lowers the hook statements to C and runs PIR
   (`stages_detail::pir_one`). Only if every UB property and every invariant
   assertion is unsatisfiable is the function `PROVED-UNBOUNDED`.
5. **Model rounds.** When a model is bound and the step is still open, the
   counterexample to induction goes to the model (grammar `invariants`,
   identifiers narrowed to the loop's variables) for up to 3 rounds. Its
   candidates go through the same Houdini and check.

Findings record `extra.invariants` (surviving set per loop),
`extra.invariant_source` (`template` or `llm:<backend>`),
`extra.k_induction = "closed-invariants"`, `extra.bounded_status` and
`extra.unwind_closed = "false"`. Without a model, `extra.llm_invariants` is
`NOTRUN: <why>`.

The hook statements (`__prism_assume`, `__prism_assert`, `__prism_havoc`,
`__prism_step`) exist only in programs built by `check_program`; user code
cannot use them.

## Harness drafting (4.2, 9.2)

Code: `src/prism/ai/harness.cpp`, called from the `harness` stage
(`src/prism/stages/harness.cpp`, `ai::drafted_harness_check`).

For a `POINTER` function without `// requires:` (which would otherwise be
`NEEDS-HARNESS`, Law 6), the harness stage drafts:

* `p != NULL`;
* the element count from usage: an index bounded by an early return, else a
  loop bound `i < n` with `n` a scalar parameter, else a conventional length
  name (`n`, `len`, `size`, `*_len`, ...), else the largest literal index + 1;
* a checked size range `1 <= n <= 4`, one PIR run per size with a buffer of
  exactly that many elements, so an off-by-one at `p[n]` is caught;
* element values unconstrained.

All sizes proved: the row stays `NEEDS-HARNESS` with
`extra.draft_verdict = PROVED-ASSUMING`, every assumption in
`extra.assumptions` and in the message, and `extra.confirm_with` holding the
`// requires:` lines. A drafted assumption was invented by PRISM, not stated
by a human, and can hide the bug (`p != NULL` when NULL is a legal input).
So it never yields a proof class on its own (Law 6; conformance tasks
`ptrparam*_false`). Paste the lines into the source and the ordinary harness
path reports `PROVED-ASSUMING`.

A counterexample under a drafted assumption is not a defect (the draft may
be too narrow). The row stays `NEEDS-HARNESS` with `extra.draft_cls` and
`extra.draft_cex`.

Only `int` elements are drafted (`int_element`). Pointers passed on to calls
are refused, and the refusal is written to `extra.harness_draft`. When the
template cannot draft, or its draft does not prove, and a model is bound,
the model proposes a draft (grammar `harness`). It goes through the same
builder and PIR (`extra.harness_source = "llm:..."`, audit
`checker: "pir(drafted harness)"`). Without a model the template row carries
`extra.llm_harness = "NOTRUN: ..."`.

`--pir-drafts` lets the `pir` stage take pointer sizes from the template
draft (never a model draft) when no `requires` gives them.

## Counterexample explanation and verified repair (4.2, 9.3)

Code: `src/prism/ai/explain.cpp`, called from the `repair` stage.

Up to 4 `FAILED` findings get `ai::explain_failed`. The model (grammar
`explain`) returns an explanation and optionally a replacement body. The
explanation is a `HYPOTHESIS` row, strength `READS`. The fix is checked by
running PIR on the patched function (nothing is compiled or executed). Only
a proof is labelled `"verified fix"`, and that covers the encoded UB
properties, not functional equivalence; anything else is
`"unverified suggestion"`. The status is in `extra.fix_pir_status`, the audit
checker is `pir(patched function)`. The `FAILED` verdict never changes.
Without a model there is one `NOTRUN` row.

## The review stage (9.2 proof repair, 9.3, 4.2 contract drafting)

`review` runs after `harness` (`src/prism/ai/review.cpp`). Its origin is
`solver` in the verdict table, because it emits re-checked proofs and proofs
of approved contracts. Everything the model writes there is `HYPOTHESIS` /
`READS`. Order:

### Assumption auditing (9.3)

* **Vacuity (Z3, no model).** For every `contracts`, `wp`, `harness` and
  approved-contract finding, the `requires` conjuncts over integer parameters
  and the drafted ranges are checked for satisfiability
  (`assumptions_satisfiable`, checker `pir(z3 probe)`). Unsatisfiable: a
  `FAILED` finding of class `VACUOUS-ASSUMPTION` naming the culprit clause.
  A `PROVED-ASSUMING` under it proves nothing. Clauses the encoder cannot
  express (`p != NULL`) are listed as not audited, never assumed
  satisfiable.
* **Model pass.** Each finding's assumption list, with the source fenced,
  goes to the model (grammar `assumption_audit`). Each flag names an
  assumption by index, an input it would exclude, and why. Flags are `READS`
  rows with `extra.verdict_effect = "none"`.

### Contract drafting and contracts from requirements (4.2, 9.3)

* `--requirements PATH` (file or directory of `.md` / `.markdown` / `.txt` /
  `.rst`, repeatable) is split into sentences `R1, R2, ...` with file and
  line.
* For up to 8 `SCALAR` functions without a spec, those named by requirements
  first, the model drafts clauses (grammar `contract`, identifiers narrowed to
  the parameters). Each clause may carry a trace ` // from R<n>`, `comment`
  or `code`. A trace to a sentence that was not offered rejects the draft.
* The draft is checked three ways: Z3 vacuity of the `requires`; the
  contracts engine (`prove_with_contract`, on PIR) proves the function
  against it; and every caller is checked to satisfy the `requires`
  (`check_callers_requires`, at most 8 callers).
* The row is `HYPOTHESIS` (strength `READS`) with `extra.contract`,
  `extra.trace`, `extra.proof_status`, `extra.callers` and
  `extra.approve_with` (the approval entries to paste).
* **Approval.** `contracts.approved.json` in the scanned root (or
  `--contracts-approved PATH`): `{"approved": [{"function", "file",
  "clause", "hash"}]}` with `hash = sha256(function "\n" clause)[:16]`
  (`clause_hash`). An entry whose hash does not match is ignored. Approved
  clauses are proved on every run, with or without a model: a proof is
  `PROVED-ASSUMING` (never more), a counterexample is `FAILED`
  (`FUNC-CONTRACT`), and a caller that violates an approved `requires` is
  `FAILED` on the caller.

### Proof store and proof repair (9.2)

Code: `src/prism/ai/proof_repair.cpp`.

* Every function with a proof-class result is recorded in
  `<out>/proof_store.json`, and in the persistent store
  (`PRISM_PROOF_CACHE`, else `$XDG_CACHE_HOME/prism/proofs`, else
  `~/.cache/prism/proofs`; file `<sha(root)[:16]>.json`). An entry holds the
  fingerprint (sha256 of signature and body without comments and whitespace)
  and each artefact that proved it: invariants, contract clauses, harness
  assumptions, stage, status, source, unwind.
* Next run, a stored function that the current run proves at the same or a
  stronger rank is kept. Otherwise each artefact is re-checked
  (`recheck_artefact`): invariants by Houdini and the loop-cut induction (a
  changed loop count is refused), contracts by the contracts engine, harness
  assumptions by re-drafting (different assumptions set
  `extra.assumptions_changed`), anything else by PIR.
* Nothing re-checks: with a model, a repair is tried (new invariants seeded
  with the stored ones, accepted only if Houdini and the induction close; or
  an updated contract, which is a new `HYPOTHESIS` with
  `extra.approve_with`). Otherwise the row is `UNKNOWN`, class
  `PROOF-REGRESSION`, `extra.regression = "true"`, with both fingerprints and
  every re-check result. The old entry stays, so the regression is reported
  until it is fixed. Detection needs no model.

Without a model the review stage writes one `NOTRUN` row naming the model
features that did not run; the vacuity audit, approved contracts and the
regression check still run.

## Lean proof search (9.2)

Code: `src/prism/ai/proof_search.cpp`. Command:
`prism prove FILE.lean THEOREM [--write] [--allow-exec]`;
`prism prove --all [ROOTS...]` runs every `sorry` theorem under the roots
(default `proofs`, `proofs/semantics`, `proofs/techniques`). The target's
whole proof must be one `sorry` / `by sorry`.

1. **Prover model.** `--prover-fpk`, else `PRISM_PROVER_FPK`, else the
   assistant model (`PRISM_FPK` or its default), else the first `.fpk` in
   `~/.prism/models` whose name contains `prover` or `goedel`. The audit name
   is `--prover-model`, else `PRISM_PROVER_MODEL`, else `PRISM_MODEL`. No
   model: `NOTRUN` (exit 3).
2. **Scratch copy.** The lake package is copied (nested packages and `.git`
   excluded) and built once. The source tree is not touched unless
   `--write`.
3. **Best-first search.** A node is a tactic prefix that Lean accepts with
   only `unsolved goals` left. The frontier keeps `--beam` nodes (default 4);
   the budget is `--budget` model calls (default 24). The prompt holds the
   statement, the file's context, the accepted prefix, the goal state, the
   node's last Lean error and up to 8 lemmas from the lemma library, all
   fenced as untrusted. The model returns `{"tactics": [...]}` (grammar
   `lean_proof`).
4. **Validation** (`validate_lean_tactics`): 1 to 40 lines of at most 400
   characters; no `sorry`, `admit`, `native_decide`, `#` commands,
   `set_option`, `axiom`, `run_tac`, `IO`, `unsafe`, elaborator or macro
   tokens.
5. **Kernel check.** The candidate is spliced in with `#print axioms` after
   the declaration and elaborated with `lake env lean` in the sandbox. An
   error other than `unsolved goals` rejects it and is fed back; `unsolved
   goals` alone makes a new node.
6. **Acceptance.** No error, axioms within `{propext, Classical.choice,
   Quot.sound}` (a proof through a user `axiom` or `sorryAx` is rejected),
   and `lake build` of the module succeeds. Only then `PROVED`. With
   `--write` the proof is written into the file, rebuilt in place (restored on
   failure), and appended to the lemma library (`proofs/lemmas.jsonl`, or
   `--lemmas PATH`).

Law 9: elaborating the project and the model's tactics runs code, so
`prism prove` without `--allow-exec` is `NOTRUN`. Every model call is an
audit record (`feature: "lean-proof"`). The result JSON (`--json`, and
`<out>/prove/<theorem>.json`; `--all` adds `<out>/prove/summary.json`) lists
every attempt. Exit 0 proved, 1 not proved, 3 `NOTRUN`, 2 error.

The repository's own proofs have no `sorry` (`proofs/check.sh` forbids it;
doctest "ai9 prove --all: the repository's proofs have no sorry"). The search
loop is tested with the real Lean kernel and a fake prover ("ai9 lean:
best-first search rejects a wrong proof with the real kernel, accepts the
right one").

## Grammars (4.1)

| File | Output | Validator |
|---|---|---|
| `grammars/invariants.gbnf` | JSON list of C boolean expressions | `validate_invariants` |
| `grammars/harness.gbnf` | `{"assumptions": [nonnull / size / range]}` | `validate_harness` |
| `grammars/contract.gbnf` | `requires ...;` / `ensures ...;`, optional trace ` // from R<n>` / `comment` / `code` | `validate_contract` |
| `grammars/assumption_audit.gbnf` | `{"flags": [{"index", "input", "reason"}]}` | `validate_assumption_audit` |
| `grammars/explain.gbnf` | `{"explanation": str, "fix_body": str}` | `validate_explain` |
| `grammars/lean_proof.gbnf` | `{"tactics": [str, ...]}` | `validate_lean_tactics` |
| `grammars/ask.gbnf` | structured query for `prism ask` | `query_from_json` |
| `grammars/draft.gbnf` | list of `{"text", "links"}` claims | `validate_draft` |

* **Narrowing.** At run time the `ident` rule is replaced with the names in
  scope (and in `ask`, the stage and status rules with the pipeline's
  vocabularies), so the sampler cannot spell an unknown name.
* **Lowering.** The grammars use bounded repetition (`X{m}`, `X{m,}`,
  `X{m,n}`), which FORGE's GBNF parser does not read. `lower_grammar`
  rewrites it to `m` copies of `X` plus nested optionals when `n - m <= 16`,
  else `X*`. The validators still enforce the bounds.
* **Validation.** The decoded text is validated again. Output that fails is
  rejected, logged (`output_valid: false`, `checker: grammar-validator`) and
  never used.
* **Embedding.** CMake writes `<build>/generated/prism/ai/grammars.inc` from
  `grammars/*.gbnf` at configure time, so the binary's copy cannot drift
  (doctest "ai-assist grammars: every grammars/*.gbnf is embedded verbatim").
  `tests/cpp/test_forge_ai.cpp` checks the lowering and that every grammar
  compiles in FORGE and constrains decoding on a tiny test model.

## Prompt-injection safety (9.6)

Source text in a prompt is fenced (`ai::fence_untrusted`):

```
<<<UNTRUSTED <label> id=<hash>
...
UNTRUSTED <label> id=<hash>>>>
```

Any `UNTRUSTED` inside the source is defanged, so the code cannot close the
fence. The system prompt says fenced text is data, never instructions. That
is defence in depth only. The guarantee is structural: the grammar limits
what the model can emit, the validators re-check it, and model output never
changes a verdict without a checker. Tests: "ai prompt fences untrusted
source" and "ai invariant output is validated after decoding"
(`tests/cpp/test_main.cpp`). `testdata/planted/ai_injection.c` is a corpus
file whose comment tells the model to output `PROVED`.

## Audit log (9.6)

Every model call appends one JSON line to `<out>/ai_audit.jsonl`. The file is
removed at the start of a run unless `--resume`. Fields:

`id, feature, function, file, model, model_sha256, prompt_sha256, grammar,
raw_output_sha256, output_valid, rejected_reason, checker, checker_result,
verdict_effect` (`"none"` or the checked verdict).

Stage chats are logged with `feature: "chat"`, `grammar: "none"`,
`checker: "none"`. A finding whose verdict was raised with model help
carries `extra.ai_audit_id`, `extra.ai_checker` and
`extra.ai_checker_result`.

## Assistant features (9.1, 9.3, 9.4)

Code: `include/prism/ai_assist.hpp`,
`src/prism/ai/{regress,triage,ask,draft}.cpp`; tests:
`tests/cpp/test_ai_assist.cpp`, `tests/cpp/test_ai_assist_cli.cpp`. Each
feature has a deterministic core that works without a model. None changes a
status: they read a finished `report.json` and write their own files.
Finding ids are `<stage>#<index in that stage>` in report.json order.

| Subcommand | What it does |
|---|---|
| `prism regress [--report OUT/report.json] [--write-tests DIR] [--framework F] [--run --allow-exec]` | regression tests from counterexamples |
| `prism ask "<question>" [--report ...] [--json] [--no-llm]` | questions over findings; the query is printed with the answer |
| `prism draft [--report ...] [--kind report\|assurance] [--proofs DIR] [--out FILE]` | report or assurance-case prose where every claim is linked |
| `prism triage [OUT] [--threshold T] [--no-embed]` | re-run triage over a saved report |
| `prism prove FILE.lean THEOREM` / `prism prove --all` | Lean proof search ([above](#lean-proof-search-92)) |

### Regression tests (9.3)

Every `FAILED` / `CRASH` finding whose counterexample assigns the function's
scalar parameters becomes a test. The framework is detected from the
project: GoogleTest, then Catch2, then CMake/ctest, then pytest, else plain;
the deciding file is in `manifest.json`. Each case includes the source
(`main` renamed), calls the function with the counterexample arguments, and
is built with UBSan and ASan, `-fno-sanitize-recover=all` (MemorySanitizer
for uninitialised-read classes). So the test fails while the defect is
present and passes once it is fixed. A framework-free `run_tests.sh` is
always written.

Tests go to `<out>/regression_tests/`, never into the user's tree unless
`--write-tests DIR`. Generating executes nothing. `--run` compiles and runs
every case and records `reproduces` / `does-not-reproduce`; without
`--allow-exec` it is `NOTRUN` (Law 9). A finding that cannot become a test
(no counterexample, pointer or struct parameter) is listed with the reason.

### Triage

`src/prism/ai/triage.cpp`. Findings with status `FAILED`, `CRASH`,
`SANFAIL`, `ERROR`, `UNKNOWN`, `TIMEOUT`, `NEEDS-HARNESS`, `BOUNDED` or
`HYPOTHESIS` are embedded deterministically: TF-IDF over class, function,
file, status, normalised message tokens and bigrams, identifiers from the
source lines around the finding, and character 3-grams.

Edges: identical `(file, line, class)` always joins; cosine >= 0.7
(`--threshold`) joins two findings of the same file. Similarity edges are
blocked by file because comparing across files let union-find chain
unrelated functions through a shared class. Clusters are ranked by most
severe status, then the number of stages reporting a defect, then size.

Output: `triage.json` and a `## Clusters` section appended to `report.md`
after the unchanged findings list. Triage orders only; no status changes.
The pipeline runs it after every scan; `PRISM_TRIAGE=0` switches it off.
With `PRISM_EMBED_FPK` set to a FORGE one-shot embedding `.fpk`, the
similarity is the mean of the TF-IDF and the model cosine. Without one,
`triage.json` records the `NOTRUN` reason.

### Questions over findings (9.4)

A keyword grammar (`parse_question`) maps a question to a structured query:
stages, statuses (with groups such as "unproved", "proofs", "bugs"), class
substrings (for example "memory safety"), file and function globs, message
text, `group_by`, count. The answer starts with `query (grammar): {...}` and
lists the words the grammar did not use.

When a model is reachable and `--no-llm` is not given, the model translates
instead, under `grammars/ask.gbnf`. Its JSON is re-validated
(`query_from_json` rejects unknown keys, stages or statuses), logged
(`checker: query-validator`), and a rejection falls back to the grammar with
the reason shown. `explain <id>` explains a verdict from the verdict
vocabulary and the finding's evidence; `trusted base` summarises
[TRUSTED_BASE.md](TRUSTED_BASE.md).

### Report and assurance-case drafting (9.4)

`prism draft` writes `draft_<kind>.md` and the claims as JSON. Every claim
carries links: `finding:<id>`, `verdict:<id>` (a proof, `BOUNDED`, `FAILED`,
`CRASH` or `SANFAIL`), `certificate:<id>` (`PROVED-CERTIFIED` with
`extra.certificate = checked`), `theorem:<name>` (declared under `proofs/`)
or `stage:<name>`. The validator rejects a claim with no link, with a link
that does not resolve, or whose words assert a proof or certificate that no
link supports. Rejected claims are listed under "Rejected claims" and never
rendered as prose. A model may reword the claims (`grammars/draft.gbnf`);
its draft goes through the same validator (`checker: draft-link-validator`).

### GUI assistant (9.4)

`prism_gui` (`src/gui/MainWindow.cpp`, built with Qt 6 under `PRISM_QT`)
has an Assistant row that calls `ai::assistant_reply` on the loaded report.
Double-clicking a finding explains it.

## Solver and bound prediction (9.1, 9.3)

Code: `src/prism/solver/gbdt.cpp`, `src/prism/solver/predict.cpp`,
`include/prism/solver_predict.hpp`.

`gbdt.cpp` is a dependency-free gradient-boosted tree model (squared loss, or
a censored loss where a solver timeout only says "at least the timeout").
`predict.cpp` loads `PRISM_SOLVER_MODEL`, else
`<solver cache>/predict_model.json`, else the built-in model
(`src/prism/solver/predict_default.json`, embedded by CMake). A model whose
feature names differ is refused. `PRISM_SOLVER_PREDICT=0` switches
prediction off.

The model sets only the portfolio members' expected times and the head start,
never for certified requests. A wrong prediction costs time, never an
answer. Every solve appends a training line to
`<solver cache>/solve_log.jsonl` (capped at 32 MB).

The built-in model enables the solver-time targets and disables the bound
target. `predict::unwind_for` exists but is not wired into `pir`. The
measured comparison is in [SOLVERS.md](SOLVERS.md) "Learned scheduler".

Training data: `PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict
collect*"` over the directories in `DIR/roots.txt` (default the conformance
suite); a collection resumes from `DIR/files_done.txt`.

## The `prism_ai` developer tool

`src/tools/prism_ai/` builds `prism_ai` (CMake option `PRISM_TOOLS`, default
on). It runs offline, never inside the pipeline, and changes no verdict.
Tests: `tests/cpp/test_prism_ai.cpp`.

| Command | What it does |
|---|---|
| `prism_ai predict` | train and measure the solver / bound model on a held-out split by source file; `--out MODEL.json` writes it, with `"enabled": true` only if it beats the rules and the history baselines |
| `prism_ai replay` | replay the portfolio scheduler on per-member alone-times (`--solve-log FILE`) |
| `prism_ai e2e` | end-to-end A/B of the learned scheduler (`--prism BIN --solve-log FILE --out DIR`) |
| `prism_ai measure {triage,ask,regress,draft} OUT` | per-feature metrics of the assistant features |
| `prism_ai sample-pairs OUT` | seeded sample of the finding pairs triage merges, for manual review (fixed seed and shuffle in `pycompat.hpp`, so a sample reproduces) |

`measure ask` scores the keyword grammar on the hand-labelled questions in
`testdata/fixtures/ask_questions*.jsonl`. `measure triage` scores against a
proxy root cause (same function, else same line), which counts merges across
lines without a function as wrong.

## Measured results

No measurement of the Goedel-Prover-V2-32B model is recorded in the tree
yet: no dated run with a commit gives its proof rate, invariant or harness
yield, or speed. The real-model doctest ("forge ai: the real model answers
plainly, under a grammar, and thinks on request") is skipped by default; run
it with

```
PRISM_FPK=~/.prism/models/goedel-prover-v2-32b.fpk PRISM_DEVICE=cuda \
  prism_tests --no-skip -tc='forge ai: the real model*'
```

CI has no GPU and no model `.fpk`. Every model half is tested there only
against deterministic fake backends (`ai::set_session_backend_for_testing`,
`stages_detail::set_chat_backend_for_testing`) and the tiny FORGE test model.
