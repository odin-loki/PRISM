# Feedback on PRISM from the OS-Proving project (13 November 2026, revised after review)

Source: the OS-Proving project ([odin-loki/OS-Proving](https://github.com/odin-loki/OS-Proving)) tried PRISM as a second proof engine on mined Linux and FreeBSD functions, then ran 24,806 functions through Frama-C WP and CBMC. This file is what that experience says PRISM needs. A first draft was written from the outside; it was then checked item by item against this repository ([REVIEW-OF-FEEDBACK.md](REVIEW-OF-FEEDBACK.md)) and this version keeps only what survived that check. Evidence figures belong to the OS-Proving data (`data/proving/core/`, `docs/plan/lib/code-mined/pilot/PILOT_REPORT.md`) and were not re-measured against PRISM.

## A. What happened when PRISM was used

- Pilot (200 functions plus 60 loop-bearing): Frama-C WP proved 29. PRISM, run on the same 29 units, answered PROVED-ASSUMING for 4, NEEDS-HARNESS for 20, NOTRUN for 4, no finding for 1; on 22 built-but-not-proved units: NEEDS-HARNESS 12, ERROR 3, NOTRUN 4, none 3. It never disagreed with the other engine.
- Which PRISM stage produced those answers (`pir`, `wp`, `contracts`) was not recorded. The review notes that the `wp` stage can return ERROR for ACSL it cannot encode but not NEEDS-HARNESS, so the NEEDS-HARNESS answers most likely came from `pir`. The run must be repeated with stage names kept (see item 15 and the upgrade plan, P0).
- An early timing of about 118 s per proof (567 s before the 5 to 7 November speed work) was for the Lean-proof-search path, not for the C stages, so it says nothing about C-function throughput. No functions-per-hour figure exists for the C stages; measuring it is the first step of the upgrade plan.
- Result: PRISM was kept as a cross-check and not counted as a proof. PRISM stays valuable for what Frama-C WP does not have: a closed verdict lattice proved in Lean, certified mode (CaDiCaL LRAT checked by cake_lpr, plus Lean's verified checker), a Lean-proved translator, k-induction and Houdini invariants, and 0 wrong proofs on its conformance suite.

## B. What PRISM already does well (so the feedback does not repeat it)

Verdict lattice and nine laws with a verdict audit; `PROVED-CERTIFIED` for QF_BV; k-induction and Houdini; counterexample reproducers (`prism regress`); `compile_commands.json` and preprocessed `.i` ingestion with a compiler-message ERROR row; flat-buffer pointer contracts (`// requires: \valid(p+(0..n-1))`) and template harness drafts (`--pir-drafts`); solver query cache, certificate store, library-model cache and proof store; `--jobs`, function, Houdini and certification budgets, a memory budget with Z3 interruption and re-check after OOM; vacuity audit of assumptions and a `muttest` stage; a learned solver-member scheduler. Details and file references are in the review.

## C. Confirmed gaps (ordered by value to a corpus-scale prover)

1. **Assumption registry (item 9).** Assumptions are free text in `extra.assumptions`. Do: a fixed registry of assumption ids, each with a one-line statement and a "checkable" flag, attached to every verdict together with engine version, source-file hash and contract-text hash.
2. **Per-function ledger (item 15).** No one-row-per-function file exists. Do: `ledger.jsonl` next to `report.json` with function, stage, engine, verdict, reasons, time, level reached and the assumption ids.
3. **Replay command (item 13).** Do: `prism replay RESULT` that re-proves a finished result in a fresh directory with caches off and compares verdict and assumptions. The pieces exist (certificate re-check, proof store, `leanchecker`/`nanoda`).
4. **Function-level resume, STOP, and an explicit OOM reason (item 7).** Resume is per stage; there is no `STOP` file and no per-function done list; running out of memory is a reported UNKNOWN. Do: per-function done list, a `STOP` file, and a stable reason string such as `memory-budget` in `extra.reason` (the verdict lattice does not need to change).
5. **Kernel idioms (items 3 and 1).** Unknown callees give NEEDS-HARNESS; volatile/atomic loads and stores, inline asm without a contract, and integers wider than 64 bits are UNENCODED. Kernel code uses all of these (`READ_ONCE`, `spin_lock`, `kmalloc`, `copy_from_user`, `list_*`, `container_of`, atomics). In the OS-Proving core run, `callees-without-spec` was recorded for 6,820 of 24,806 functions (28%) and `pointer-or-memory-precondition` for 3,501. Do: a kernel model library next to the libc models, sound assigns-only stubs for unknown callees (havoc what they can reach, as the indirect-call fallback already does), `\separated`, `container_of`, and nested/linked pointer-graph harnesses built from declared types.
6. **Whole-function verdict cache and translation-unit slicing (items 5 and 4).** The VC cache needs clang and the encoder to rebuild the VC first; lowering is per whole unit and functions in a unit run one after another. In our run 47% of core functions could not be built at all, mostly a per-unit flags problem, which we solved with per-file kbuild flags, reconstructed BSD flags and cached preprocessed units. Do: cache keyed on (PIR hash, contract hash, options, engine version) and a slicer that keeps only the function, the types it needs and its callees' bodies.
7. **Mutation of proofs, kill rate (item 6).** `muttest` runs on the concrete interpreter; it does not mutate the code and re-run the prover per PROVED function. Do: for each proof, mutants are first filtered by concrete execution and survivors re-proved; report a kill rate per proof. This is how OS-Proving showed that a proof means something.
8. **Run-time cross-engine check (item 14).** The policy exists in the lattice and as `pir-vs-bmc` in QA, but nothing raises an incident at run time when `bmc` says FAILED and `pir` says PROVED for the same function. Do: record it as a finding with a stable class, without a new verdict.
9. **Integer semantics recorded (item 2).** PRISM is bitvector throughout; ACSL is mathematical. The OS-Proving random testing found 436 contracts wrong only at integer limits and 1,875 with operator-precedence bugs, so the reading matters. Do: record the reading in `extra.int_semantics` on every verdict, and add a mathematical-integer mode only after the lattice question below is settled.
10. **Loop summaries (item 12).** Unwinding, deepening, k-induction and Houdini exist. Missing are summaries for counted loops, list walks and bit scans. Houdini is a time sink on some code (`npr`, `strcat`).
11. **Stage-level scheduling (item 11).** The solver-member scheduler (GBDT, on by default) orders Z3, Bitwuzla, CaDiCaL, Kissat and ProbSAT on one formula. Nothing chooses between interval, WP, BMC, pir and k-induction by function features, and the pipeline does not stop early after a conclusive result. `PRISM_SOLVER_PREDICT` switches the member scheduler on or off; it is not a hook for choosing stages.
12. **Counterexample reproducers for pointer parameters (item 8).** `prism regress` handles scalar parameters; pointers and structs are listed with the reason. Add pointer and struct reproducers and a minimisation step.

## D. Corrections to the first draft (kept so nobody repeats them)

- `prism prove` in PRISM is Lean proof search; the C proving stages are `contracts`, `wp`, `bmc`, `pir`, `harness` and `review` under `prism PATH`.
- `wp` is a small in-tree substitution prover, not Frama-C WP.
- `PRISM_SOLVER_PREDICT` is not an engine-choice hook.
- PRISM cannot emit Alethe or any SMT proof; only QF_BV certification (LRAT) exists. k-induction steps and BOUNDED/PROVED-UNBOUNDED results are not certified.
- New proof verdicts (`PROVED-C-SEMANTICS`, `WEAK`, `ENGINE-DISAGREE`, `OUT-OF-MEMORY`) would each need a change to the Lean-proved lattice and regenerated tables; use an `extra` field or a finding class unless a real new level is needed.
- The reasons quoted from OS-Proving (`stub-headers-EMPTY`, `bv-sat-uncertified`, `pointers-modelled-as-disjoint-malloc-objects`) are OS-Proving's own labels, not PRISM's.
- Training a scheduler on OS-Proving labels would teach it Frama-C/CBMC outcomes, not PRISM's; the labels are useful as a benchmark (below), the training data must come from PRISM runs.

## E. Where the OS-Proving data can help PRISM

- **A benchmark of 24,806 real kernel functions** with kind, class, source tree, the Frama-C/CBMC verdict and reasons, timings, and the reasons for not building. Use it as a corpus to measure PRISM's coverage and speed, and to find where PRISM proves what the other tools cannot (and the reverse).
- **2,148 refuted contracts with concrete counterexamples** (CBMC) and 5,855 proved functions, as a known-answer regression set for `pir`.
- **349,708 error events with the original annotation and every repair attempt** (`data/errors/`), as training and evaluation data for contract drafting and review.
