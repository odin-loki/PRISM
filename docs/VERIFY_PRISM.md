# Verifying PRISM: an exhaustive, easy-to-run checklist

Written 14 November 2026. The question: is PRISM any good, and can we show
its proofs are right? PRISM cannot vouch for itself (a bug in PRISM can hide
a bug in PRISM), so almost every check below compares PRISM against
something independent of its engine: a known answer, another tool, the
compiled program, a checked certificate, a Lean proof, or itself under a
transformation that must not change the answer.

Existing evidence is catalogued in [assurance/EVIDENCE.md](assurance/EVIDENCE.md)
(E01–E21) and the last completed round in [VERIFICATION_PLAN.md](VERIFICATION_PLAN.md).
This list adds what is missing and says how to run each item. Status:
**have** (exists and is run), **part** (exists, not run routinely or not at
scale), **new** (to build). Effort: **S** (an hour to a day), **M** (days),
**L** (weeks).

The one number that matters most: **wrong proofs** (PROVED where the
property fails). Every section feeds it. The second: **wrong refutations**
(FAILED where the property holds), which cost trust but not soundness.

## A. Are its FAILED verdicts right? (refutations)

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| A1 | Counterexample replay on the PIR interpreter | already done before FAILED is written (E13) | every FAILED has `extra.cex` and replayed | have | — |
| A2 | Counterexample replay on the **compiled** program | build the function with clang -fsanitize=undefined,address, call it with the cex arguments in a generated `main`, sandboxed (`--allow-exec`, `prism regress`) | the sanitizer or assertion fires | part (E13, scalar only) | S per type class; M for pointer/struct cex (UPGRADE_PLAN P4.4) |
| A3 | Independent witness validation | export SV-COMP violation witnesses, validate with CPAchecker and UAutomizer (`docs/SVCOMP.md`) | validator confirms | part (SV-COMP subset) | S to run on all tasks |
| A4 | Refutation sample review | 50 random FAILED from the kernel runs, read by a person with the cex | each is a real path | new | S |

## B. Are its PROVED verdicts right? (the core question)

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| B1 | Known-answer suite | `prism-qa conformance` (testdata/conformance, Juliet, ESBMC C++, libc models) | 0 wrong proofs | have (E09) | — |
| B2 | Random programs vs compiled execution | `prism-qa soundness` (E10): generate, prove, then run the compiled program on many inputs | no PROVED program fails at run time | have | S to run 10× larger |
| B3 | **Cross-tool agreement on the kernel** | the OS-Proving core set has a Frama-C / CBMC verdict per function (UPGRADE_PLAN P0): join with PRISM's verdict per function; triage every (PRISM PROVED, other tool FAILED) pair | every such pair explained (other tool wrong, different property, different assumptions) or an incident | new | S to join; M to triage |
| B4 | **Mutation testing of proofs** | for a sample of PROVED functions, inject single bugs (off-by-one in a bound, `<` → `<=`, drop a check, widen a type, remove a guard); re-prove each mutant | a mutant that breaks the property is not PROVED; report kill rate per proof (UPGRADE_PLAN P4.2) | part (`muttest` stage) | M |
| B5 | Vacuity | every PROVED-ASSUMING: are its assumptions satisfiable? (`vacuity_audit`, E-series) and does every caller meet them? | no vacuous proof | part | S to run over the library |
| B6 | Certified re-check | `--certified` on a sample: LRAT checked by cake_lpr and Lean's checker | certificate accepted | have (E08) | S to run on a kernel sample |
| B7 | **Second-solver re-check of plain PROVED** | the solver cache keeps every VC (`--solver-cache`): re-solve every UNSAT VC with a different solver (Bitwuzla / CVC5 / CaDiCaL on the CNF) | every UNSAT confirmed | new | S |
| B8 | Translation validation of the front end | `prism-qa pir-lean-check` (IR → PIR against the Lean-proved translator), `lli` input vectors (E12) | mismatch 0 | have | S to run on the kernel |
| B9 | Planted bugs | testdata/planted, testdata/tp: every planted bug FAILED, never PROVED | 100% | have | — |
| B10 | Verdict audit | `prism::verdict::audit` on every report (E04): a stage cannot write a verdict its origin may not | no violation | have | — |

## C. Does it give the same answer when it must? (metamorphic and differential)

These are the cheapest strong checks: no oracle is needed, only the rule
that the answer must not change.

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| C1 | `--jobs 1` vs `--jobs N` | full conformance and testdata/pir at both | identical verdicts and row order | part (tests/cpp/test_pir_parallel.cpp; testdata/pir measured) | S |
| C2 | Cache on vs off, cold vs warm | `PRISM_PIR_FN_CACHE=0`, fresh vs reused `--solver-cache` | identical verdicts | part (doctest; 100-function unit) | S |
| C3 | Same run twice | report.json minus timing fields | identical | new as a CI diff | S |
| C4 | Unwind monotonicity | `--unwind 8` vs `16`: PROVED stays PROVED, FAILED stays FAILED, BOUNDED may become PROVED/FAILED, never the reverse | no reversal | new | S |
| C5 | Semantics-preserving rewrites | rename variables and functions, reorder independent statements, add dead code, split / inline a helper, `-O0` vs `-O2` lowering, add unrelated functions to the unit | same verdict per function | new | M (a small rewriter on the planted corpus) |
| C6 | Semantics-changing rewrites | the B4 mutants seen from the other side: a change that breaks the property must change PROVED | verdict changes | new | with B4 |
| C7 | Engine cross-check | `pir` vs `bmc` vs `contracts` on the same function (E11); `conc` on single-threaded programs vs `pir` | no PROVED/FAILED contradiction | part | S to make an incident finding at run time (UPGRADE_PLAN P3.5) |
| C8 | Data model | LP64 vs ILP32 on width-independent code | same verdict | new | S |
| C9 | Kernel version pairs | the same function in two kernel versions with an identical body (by `pir_fingerprint`) | same verdict | new (needs the library) | S after L1 |

## D. Is "I don't know" honest? (Laws 1, 7)

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| D1 | Every function gets a row | inventory's function list vs pir rows per unit | no function without a verdict or a stated reason | new as an automatic check | S |
| D2 | Missing tool is NOTRUN | remove clang / z3 / a linter from PATH | NOTRUN with an install hint, never clean | have (tests) | — |
| D3 | Budget cuts say so | tiny `PRISM_FUNCTION_BUDGET`, `PRISM_HOUDINI_BUDGET` | TIMEOUT / budget note, never PROVED | have (test_pir_budget) | — |
| D4 | Memory cuts say so | small `PRISM_MEM_BUDGET_MB` on a big corpus | UNKNOWN with the memory message, or re-run in a child | part | S (this session found 476 UNKNOWNs at the budget: now a regression test) |
| D5 | Crash-free on everything | the kernel trees: CRASH and internal ERROR counts | 0 | part | S from the library stats |

## E. Is the engine code itself sound? (PRISM as a program)

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| E1 | Sanitizers | prism_tests and conformance under ASan + UBSan (+ TSan now that pir is multi-threaded per function) | clean | have (ASan/UBSan); **TSan new** | S |
| E2 | Fuzz the parsers | IR parser, report reader, contract parser, Lean output parser, `prism library import` (E15) | no crash, no hang | part | S per parser |
| E3 | **Mutation testing of PRISM itself** | apply small faults to the encoder/translator (drop an overflow check, flip a comparison, skip a loop cut) and run the conformance subset | every fault turns the gate red | new | M; the best measure of how much the test suite would catch |
| E4 | Coverage of the tests | llvm-cov over prism_tests + conformance | report which encoder paths no test reaches | new | S |
| E5 | Static analysis | clang-tidy (root .clang-tidy), `-DPRISM_WERROR=ON`, PRISM on PRISM | clean or triaged | have (VERIFICATION_PLAN step 1) | — |
| E6 | Fault injection in the solver layer | make a solver member return a wrong SAT/UNSAT (test hook) | validation rejects it; no wrong verdict | part (validated answers) | S |
| E7 | Thread-safety of the new parallel code | TSan run of `--jobs 4` on conformance; a stress test with many tiny functions | no race | new | S |

## F. Is the trusted base what we say? ("prove the proving")

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| F1 | Verdict lattice and laws | Lean, re-checked by leanchecker and nanoda (E01–E03) | checked | have | — |
| F2 | PIR semantics and encoder soundness (models) | `proofs/semantics` (E05) | checked | have | — |
| F3 | IR → PIR translator | Lean-proved translator + per-run correspondence (E12) | mismatch 0 | have for the fragment | L to widen the fragment |
| F4 | **The C++ encoder** (PIR → SMT) | not proved; mitigated by B1, B2, B7, C-series, E3 | — | gap | L: a Lean-checked VC generator for a fragment, compared per run like F3 |
| F5 | Solver UNSAT | QF_BV: LRAT + verified checkers (B6); other theories: second solver (B7) or Alethe proofs from CVC5 | checked | part | M for Alethe |
| F6 | Library models (libc, C++, kernel) | verified like user code; listed as assumptions | each model proved against its contract | have for libc | M for kernel models |
| F7 | Clang/LLVM | trusted (pinned 23.1.4); binary-level translation validation for a pilot (UPGRADE_PLAN P6.6) | — | gap | L |
| F8 | Lean search acceptance | axiom audit + `lake build` gate (docs/AI.md) | no sorryAx / user axiom in an accepted proof | have (tests with a real kernel) | — |
| F9 | The trusted base document | docs/TRUSTED_BASE.md lists every trusted component; check it against the code once per release | matches | have | S per release |

## G. Are the outputs right? (reports)

| # | Check | How | Pass | Status | Effort |
|---|---|---|---|---|---|
| G1 | SARIF valid | validate report.sarif against the SARIF 2.1.0 schema | valid | part (test_sarif) | S |
| G2 | report.json schema | a JSON schema for report.json, validated in CI | valid | new | S |
| G3 | Counts add up | per-stage counts in report.md = rows in report.json; confidence follows Law 5 | equal | part | S |
| G4 | `--fail-on` | exit codes (test_cli) | as documented | have | — |
| G5 | The proof library round trip | import a run, look every row up again | every row found, status equal or audit-demoted with the reason | part (test_library) | S |

## H. Process: keep it true

| # | Check | How | Status | Effort |
|---|---|---|---|---|
| H1 | Gate every merge on B1 (0 wrong proofs), C1, C3, D1 | CI | part | S |
| H2 | Publish the numbers per release (wrong proofs, proved / refuted share, functions per hour) | CONFORMANCE.md, UPGRADE_PLAN gates | part | S |
| H3 | Incident log | every wrong verdict found by any check above: a reproducer in testdata/conformance and a line in CONFORMANCE.md "Known issues" | have | — |
| H4 | Independent review | someone outside the project runs B1, B2, B3 from the docs alone | new | S for them |

## The ten to do first (most confidence per hour)

1. **B3** join the kernel runs with the Frama-C / CBMC verdicts and triage the PROVED-vs-FAILED disagreements.
2. **B7** re-solve every cached UNSAT VC with a second solver.
3. **E3** mutate the encoder and see how many faults the conformance gate catches.
4. **C1 / C2 / C3** jobs, cache and repeat-run equality on the full conformance suite.
5. **E1 / E7** TSan on the new parallel pir.
6. **C4** unwind monotonicity over the conformance suite.
7. **B4** mutation testing on 200 kernel PROVED functions.
8. **D1** every function has a row (automatic).
9. **A2** compiled-program replay of every scalar counterexample.
10. **B8** translation validation over the kernel sample.

None of these needs new theory; most are a script over outputs PRISM
already writes. Results go in a table like VERIFICATION_PLAN.md "Results":
check, commit, date, result.
