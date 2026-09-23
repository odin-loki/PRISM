# PRISM verdicts

Every finding PRISM writes carries one status from a fixed vocabulary. This
document is the full lattice, what each verdict means, which stages may emit
which, and the laws that govern them, each with the name of the Lean theorem
that proves it.

| Where | What |
|---|---|
| `include/prism/verdict.hpp`, `src/prism/verdict/verdict.cpp` | The lattice as a pure C++ module (no I/O) |
| `include/prism/laws.hpp`, `src/prism/laws.cpp` | String constants and API, forwarding to the module; `audit_report` |
| `prism/laws.py` | The Python engine's copy (frozen engine, same vocabulary) |
| `proofs/Prism/Verdict.lean` | The Lean 4 model and the proved laws |
| `proofs/Prism/Export.lean`, `proofs/Main.lean` | `lake exe verdict_tables`: every function over its whole domain as JSON |
| `tests/data/verdict_tables.json` | That JSON, committed |
| `tests/cpp/test_main.cpp` ("verdict module equals the Lean model"), `tests/test_verdict.py` | The code checked against the table entry by entry |
| `proofs/check.sh`, `.github/workflows/proofs.yml` | Build, no-`sorry`, axiom and table checks in CI |

## The lattice

The *formal* verdicts are ordered by strength (`rank`). Everything else has
rank 0 and is not a formal claim.

Each verdict has an anchor `#verdict-<name in lower case>` (for example
`VERDICTS.md#verdict-proved-certified`). Every run copies this file next to
its reports, and every finding in `report.md` links its verdict here
(roadmap 6.4).

| Verdict | Rank | Class | Meaning |
|---|---|---|---|
| <a id="verdict-proved-certified"></a>`PROVED-CERTIFIED` | 5 | proof | The solver's UNSAT result was checked by a verified checker (cake_lpr or Lean's LRAT checker) against the exact CNF PRISM produced (roadmap 3.2). Needs `extra.certificate = "checked"`. |
| <a id="verdict-proved-unbounded"></a>`PROVED-UNBOUNDED` | 4 | proof | k-induction closed: the property holds for every unwinding. |
| <a id="verdict-proved"></a>`PROVED` | 3 | proof | All properties hold, loops closed within k. |
| <a id="verdict-proved-assuming"></a>`PROVED-ASSUMING` | 2 | proof | Proved under an explicit `requires` or harness assumptions, which the report lists. |
| <a id="verdict-bounded"></a>`BOUNDED` | 1 | formal, not a proof | Nothing found within k. That is all it says. |
| <a id="verdict-failed"></a>`FAILED` | 0 | answer, defect | A counterexample. |
| <a id="verdict-unknown"></a>`UNKNOWN` | 0 | no answer | The solver ran and did not conclude. Also what the audit demotes an inadmissible claim to. |
| <a id="verdict-timeout"></a>`TIMEOUT` | 0 | no answer | Ran out of time. |
| <a id="verdict-error"></a>`ERROR` | 0 | no answer | The instrument failed, or an internal check (such as the verdict audit) failed. |
| <a id="verdict-nofunc"></a>`NOFUNC` | 0 | no answer | The function was not found. |
| <a id="verdict-notrun"></a>`NOTRUN` | 0 | no answer | The tool is missing or the step was not allowed to run (Law 1, Law 9). |
| <a id="verdict-needs-harness"></a>`NEEDS-HARNESS` | 0 | no answer | Pointer parameters, no precondition: not model-checked unguarded (Law 6). |
| <a id="verdict-crash"></a>`CRASH` | 0 | defect | A fuzzer or execution crashed the code. |
| <a id="verdict-clean"></a>`CLEAN` | 0 | fuzz silence | The fuzzer found nothing. Not a proof (Law 3). |
| <a id="verdict-noseed"></a>`NOSEED` | 0 | fuzz | The fuzzer had no seed to start from. |
| <a id="verdict-sanfail"></a>`SANFAIL` | 0 | defect | A sanitizer reported an error. |
| <a id="verdict-hypothesis"></a>`HYPOTHESIS` | 0 | model | LLM output. It cannot cover a defect class (Law 4). |
| <a id="verdict-reads"></a>`READS` | 0 | model | LLM reading of the code, weaker than a hypothesis. |

Predicates (same in all three implementations and the model):

- `is_proof`: the four `PROVED*` verdicts.
- `is_formal`: a proof or `BOUNDED`. No two distinct formal verdicts merge.
- `answered`: formal or `FAILED`.
- `no_answer`: `UNKNOWN`, `TIMEOUT`, `ERROR`, `NOFUNC`, `NOTRUN`, `NEEDS-HARNESS`.
- `defect`: `FAILED`, `CRASH`, `SANFAIL` (the only SARIF results).
- `model`: `HYPOTHESIS`, `READS`.

`PROVED-CERTIFIED` is not a SARIF result (there is nothing to fix); SARIF
`runs[0].properties.certified` counts the certified findings. It resolves a
function for confidence like any proof, and taxonomy coverage treats it as
`PROVES`.

## The merge law

`merge_refusal(a, b)` says why two claims about one function must stay two
claims; `refuse_merge` raises when it is not `none`.

| Refusal | When |
|---|---|
| `formal` | `a != b` and both are formal (Law 2, extended to every pair including `PROVED-CERTIFIED`) |
| `promote-fuzz` | one is `CLEAN`, the other formal (Law 3) |
| `promote-model` | one is `HYPOTHESIS`/`READS`, the other formal (Law 4) |
| `notrun-clean` | one is `NOTRUN`, the other `CLEAN` or an answer (Law 1) |
| `none` | otherwise, including `a == b` |

## Rewrites

`may_rewrite(from, to)`: may a recorded status later be rewritten? A formal
verdict may only be weakened to a lower-rank formal verdict (for example
`PROVED-UNBOUNDED` to `PROVED-ASSUMING` when a contract was needed, `PROVED`
to `BOUNDED`) or withdrawn to `UNKNOWN`/`ERROR`. Nothing becomes a formal
verdict by rewriting. A status without an answer only becomes another status
without an answer. Everything else may change freely.

## Admission and origins

Every result has an *origin*. Only two origins may prove.

| Origin | May prove | Stages |
|---|---|---|
| `solver` | yes | `contracts`, `wp`, `bmc`, `pir`, `harness`, `review` (re-checked proofs, approved contracts), `ltl` |
| `external-prover` | yes | `optional` (CBMC), `esbmc`, `dafny` |
| `fuzzer` | no | `fuzz`, `rapid` |
| `model` | no | `llm`, `repair` |
| `lint` | no | `lints`, `taint`, `thread`, `interval`, `warnings`, `cppcheck`, `pbsd`, `polyglot` |
| `execution` | no | `sanitize`, `concolic`, `diff`, `muttest`, `execute` |
| `pipeline` | no | `inventory`, `classify`, `unify`, and any stage not in the table |

`admit(origin, status, certificate_checked)`:

1. A status that is not formal passes unchanged (so `NOTRUN` stays `NOTRUN`).
2. A formal status from an origin that may not prove becomes `UNKNOWN`.
3. `PROVED-CERTIFIED` stays `PROVED-CERTIFIED` only from the `solver` origin
   with a checked certificate; otherwise it falls back to `PROVED`, never upward.
4. Any other formal status from a proving origin passes.

## The verdict audit

After every stage has run, both pipelines (`run_pipeline` in
`src/prism/pipeline.cpp`, `Pipeline.run` in `prism/pipeline.py`) call
`audit_report(report)` once before `unify` (so the taxonomy and confidence
only see admitted verdicts) and once after it. For every finding of every
stage it computes `audit(stage, status, certificate)`, which is
`admit(stage_origin(stage), status, certificate)` plus a *violation* flag
when the result differs from what the stage wrote. On a violation:

- the finding is demoted to the admitted status (`UNKNOWN`, or `PROVED` for an
  uncertified `PROVED-CERTIFIED`), its message is prefixed with the reason and
  `extra.audit_original` keeps the status the stage wrote;
- an `ERROR` finding `verdict audit: <stage> may not emit <status>` (plus
  ` without a checked certificate` for the certificate case) with
  `extra.audit = "verdict"` is appended to that stage.

The audit is idempotent. A violation is a bug in a stage; the report shows it
instead of printing a false proof.

Known consequence: `repair` (origin `model`) returns `PROVED` when the BMC
oracle proves an LLM-written patch. That proof is about the patch, not about
the scanned code, so in a pipeline run the audit demotes it to `UNKNOWN` and
records the `ERROR`. The repair stage should report the patch verdict in
`extra` instead; until then the audit makes the problem visible.

## Confidence (Law 5)

`confidence = visibility × answer × resolution`, each factor a ratio of
counts that is 0 when its denominator is 0 (no data scores 0, never n/a).
`score_counts(n_fun, classified, attempted, answered, resolved)` computes
`classified/n_fun`, `answered/attempted`, `resolved/answered` and their
product; `confidence(v, a, r)` is exactly 0 whenever `v` is 0.

## Proved theorems

All in `proofs/Prism/Verdict.lean`, namespace `Prism`, core Lean 4 only
(toolchain pinned in `proofs/lean-toolchain`), no `sorry`, no
`native_decide`. `proofs/Prism/Axioms.lean` prints the axioms of each; CI
fails on any axiom outside `propext`, `Classical.choice`, `Quot.sound`
(today all use only `propext`, and `confidence_le_one` uses none).
Statements that range over the finite domain are proved by `decide` over
the whole domain; the reachability theorems are proved by induction over
arbitrary chains of rewrites.

| Law | Theorem | Statement |
|---|---|---|
| 2 | `proved_bounded_never_merge` | `mergeRefusal proved bounded = .formal` and the reverse |
| 2 | `formal_never_merge` | two distinct formal verdicts always refuse with `.formal` |
| 2 | `certified_never_merges_weaker` | `PROVED-CERTIFIED` never merges with any other formal verdict |
| - | `mergeRefusal_symm`, `mergeRefusal_refl` | the merge law is symmetric; a verdict merges with itself |
| 3 | `clean_never_promoted` | `CLEAN` never merges with a formal verdict |
| 3 | `clean_never_rewritten_to_proof` | `CLEAN` is never rewritten to a formal verdict |
| 4 | `model_never_promoted` | `HYPOTHESIS`/`READS` never merge with a formal verdict |
| 1 | `notrun_never_merges_clean` | `NOTRUN` never merges with `CLEAN` or an answer |
| 1 | `notrun_never_becomes_clean` | one rewrite from `NOTRUN` is never `CLEAN` or an answer |
| 1 | `notrun_never_clean` | from `admit o NOTRUN c`, no chain of rewrites reaches `CLEAN` or an answer |
| - | `rewrite_never_creates_formal`, `rewrite_to_certified` | rewrites never create or strengthen a formal verdict |
| 3, 4 | `admit_nonproving` | an origin that may not prove never gets a formal verdict out of `admit` |
| 3 | `admit_fuzzer_never_proof` | `admit .fuzzer v c` is never a proof |
| 4 | `admit_model_never_proof` | `admit .model v c` is never a proof |
| 3, 4 | `no_path_fuzzer_or_model_to_proof` | from a fuzzer or model result, no chain of rewrites reaches any formal verdict |
| cert | `admit_certified_iff` | `admit o v c = PROVED-CERTIFIED` iff `o = solver`, `v = PROVED-CERTIFIED`, `c = true` |
| cert | `certified_only_with_certificate` | any chain of rewrites ending in `PROVED-CERTIFIED` started from a checked certificate on the solver origin |
| - | `admit_rank_le`, `admit_proof_requires_proof`, `admit_idem` | `admit` never strengthens, never makes a proof out of a non-proof, and is idempotent |
| audit | `proving_stages` | exactly `optional`, `esbmc`, `dafny`, `contracts`, `wp`, `bmc`, `pir`, `harness`, `review`, `ltl` may prove |
| audit | `audit_nonproving` | the audit never lets a non-proving stage output a formal verdict |
| audit | `audit_flags_nonproving` | a non-proving stage's formal verdict becomes `UNKNOWN` with a violation |
| audit | `audit_certified` | the audit never outputs `PROVED-CERTIFIED` without a checked certificate |
| audit | `audit_notrun`, `audit_nonformal_untouched`, `audit_idem`, `audit_violation_iff` | `NOTRUN` and every non-formal status pass untouched; the audit is idempotent; a violation means the status changed |
| 5 | `confidence_zero_of_visibility_zero` | visibility 0 gives confidence 0 (likewise `_answer_zero`, `_resolution_zero`) |
| 5 | `score_no_functions`, `score_no_data` | no functions (or none classified) scores 0 |
| 5 | `confidence_le_one`, `score_le_one` | each factor at most 1 gives confidence at most 1 |
| 5 | `confidence_le_visibility` | confidence never exceeds visibility |
| 5 | `confidence_mono_visibility`, `confidence_mono_resolution` | more visibility or more resolution never lowers confidence |

## Connecting the proof to the code

The roadmap (Part 5.1) proposes compiling the Lean model to C and linking it
into PRISM. PRISM does not do that. Instead:

1. `lake exe verdict_tables` evaluates every function of the model over its
   **whole** finite domain: 18 verdicts, 324 merge pairs, 324 rewrite pairs,
   252 `(origin, status, certificate)` admissions, 1,044
   `(stage, status, certificate)` audits. The output is committed as
   `tests/data/verdict_tables.json`.
2. The C++ doctest and `tests/test_verdict.py` compare `src/prism/verdict/`,
   the `laws.hpp` string API and `prism/laws.py` with that file entry by
   entry. They also check that the table's stage list is `STAGE_ORDER` plus
   `other`, and that the Python and C++ vocabularies are identical.
3. CI (`proofs.yml`) rebuilds the model and fails if the regenerated table
   differs from the committed one.

Two total functions on a finite domain that agree on every input are equal,
so this is as strong as linking the compiled model for these functions, and
it keeps the Lean runtime and compiler out of the PRISM binary and its
trusted base. What stays trusted: the Lean kernel, the export code in
`Prism/Export.lean` (it prints what the model computes), the JSON reader in
the tests, and the fact that the tests run on the shipped build.

Confidence ranges over the naturals, so its table is a sample grid (every
count up to 3, 119 rows) and the laws above are proved for all inputs; the
C++ and Python products are compared with the model's exact fractions on the
grid to 1e-12.

## Changing the lattice

Change `proofs/Prism/Verdict.lean`, `src/prism/verdict/verdict.cpp` and
`prism/laws.py` together, then run `proofs/check.sh --write` to regenerate
the table. A new pipeline stage needs a row in `Stage` / `Stage.origin`
(Lean), `kStages` / `kStageOrigin` (C++) and `STAGE_ORIGIN` (Python); the
tests fail until all three and `STAGE_ORDER` agree.
