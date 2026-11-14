# Proof library: plan

Written 14 November 2026. Input: about one million PRISM verdicts over the
Linux and FreeBSD kernel sources, being vetted by the owner; a large share
already passes a mechanical check. Nothing here is built yet except where a
section says so. The laws in CLAUDE.md stay: a stored verdict is never a
verdict by itself, `PROVED` and `BOUNDED` are never merged, a model's output
never covers a defect class.

## 1. Does a library help? Yes, in eight ways

| # | Use | What it saves or adds | Law that bounds it |
|---|---|---|---|
| U1 | **Exact reuse with re-check.** A function whose PIR matches a library entry starts from that entry's artefacts: the invariants that closed its loops, the unwind that closed, the contract and harness assumptions, the counterexample. Re-checking an artefact is one or a few small queries (Houdini from survivors, one inductive step, one replay), not the search that found it. | the search time: minutes to seconds for the heavy tail (Houdini, deepening) | a hit is re-checked; only the re-check makes the verdict (as the Houdini cache already does: `hit (proof re-checked)`) |
| U2 | **Near-match transfer.** Kernel code repeats itself: the same list walk, bit scan, bounds check or state-machine step in thousands of drivers. A structural key (PIR with names and constants abstracted) finds functions *like* this one; their invariants and harness shapes become candidates, and Houdini keeps only the ones that hold here. | invariants and harnesses that today need `--pir-drafts`, a template or a model | candidates are filtered by Houdini / the solver, so a wrong one costs time, never soundness |
| U3 | **Scheduler training** (UPGRADE_PLAN P3). Features of a function (size, loops, pointers, callees, kind) and which engine, unwind and budget gave its final verdict, a million times. | the learned stage scheduler and budget predictor, trained on PRISM's own outcomes, not other tools' labels | every learned rule is gated by "no lost verdict on the benchmark" (the unwind-prediction lesson in docs/SOLVERS.md) |
| U4 | **What to model next** (UPGRADE_PLAN P2). The NEEDS-HARNESS reasons (`UNENCODED: call @kmalloc`, volatile, inline asm, `__int128`) counted over the whole kernel give an exact priority list for the kernel model library and the encoder. | the order of P2 work, by functions unblocked per model | none (it only orders work) |
| U5 | **Fixed benchmark and regression corpus** (UPGRADE_PLAN P0). A stratified sample (by verdict, kind, size, subsystem) with its known verdicts. Every PRISM change re-runs it: a verdict that changes is an incident, a verdict that improves is measured progress. | the P0 baseline and every later gate; functions-per-hour on real kernel code | a changed verdict is investigated, never auto-accepted |
| U6 | **Soundness audit at scale.** Cross-engine disagreements (pir PROVED vs bmc FAILED on the same function), vacuous assumptions, PROVED-ASSUMING whose assumptions no caller meets, mutation kill rates on a sample. | finds soundness bugs a conformance suite of hundreds cannot | each finding is an incident with a reproducer |
| U7 | **Retrieval for the model features.** Proved functions and their invariants as few-shot examples for `--pir-drafts`, the harness stage and contract drafting; proved Lean statements as lemmas for `prism prove`. | better drafts per model call | model output stays HYPOTHESIS / READS until checked (Law 4) |
| U8 | **Refutation seeds.** Every FAILED with a replayed counterexample is a concrete input; mutated kernel versions and similar functions are tried on those first (concrete execution, later GPU, UPGRADE_PLAN G2/G3). | early refutation without SMT | a candidate counterexample is replayed before it becomes FAILED |

The largest direct speed gain is U1 (re-proving an unchanged or barely
changed kernel in minutes) and the largest coverage gain is U2 (kernel
idioms proved once, reused everywhere).

## 2. Trust tiers (what "vetted" means here)

Every entry carries a tier. Only T1 and above may seed U1/U2; T0 is data
for U3–U6 only.

| Tier | Meaning | How an entry gets it |
|---|---|---|
| T0 recorded | PRISM produced the verdict; nothing re-checked it | import of a report row |
| T1 replayed | `prism replay` (UPGRADE_PLAN P4) re-ran the function in a fresh directory with caches off and got the same verdict and assumptions; a FAILED's counterexample replayed concretely | the mechanical check the owner is running now, if it is a re-run |
| T2 certified | `PROVED-CERTIFIED`: CaDiCaL LRAT checked by cake_lpr (and Lean's checker when `prism-bitblast` made the CNF) | `--certified` run |
| T3 Lean | the verdict follows from a Lean-checked statement (refinement pilot, UPGRADE_PLAN P6) | Lean kernel + leanchecker + nanoda |
| Q quarantine | the owner's vetting rejected it, or a re-check disagreed | vetting, U5, U6 |

A quarantined entry is kept (it is the most valuable kind of data for U6)
but never seeds anything.

## 3. What an entry is

One JSON object per (function, verdict run), content-addressed:

| field | source today | note |
|---|---|---|
| `keys.exact` | **new**: BLAKE3 of `fncache::fingerprint(fn)` (the complete PIR serialisation) | engine-independent; the verdict cache's key adds the engine digest on top |
| `keys.source` | proof store fingerprint (sha256 of signature + body, comments and whitespace removed) | survives an unrelated edit elsewhere in the unit |
| `keys.shape` | **new**: PIR with variable names, constants, block names and source lines abstracted, then hashed | the near-match key for U2 |
| `keys.callees` | `extra.inlined` + the library models linked | an entry is only reused when its callees match too |
| `verdict` | status, cls, message, line, counterexample, prop | from the pir row |
| `level` | L0–L5c of UPGRADE_PLAN §3 | derived |
| `artefacts` | `pir_invariants`, `unwind`, `unwind_closed`, `unwind_deepened_to`, `k_induction`, `assumptions` / `extra.assumption`, contract clauses, harness draft, `certificate` | what U1 re-checks |
| `features` | sizes, loop count and nesting, pointer params, callee count, kind, unit size, time split, peak memory | U3 |
| `provenance` | PRISM version and engine digest, Z3 version, options that change an answer (`fncache::key` inputs), source path, kernel tree and commit, run id | staleness and replay |
| `tier`, `vetting` | §2 | the owner's vetting result and note |

Source text is **not** stored in the library, only hashes and the path and
commit it came from: Linux is GPL-2.0, FreeBSD mostly BSD, and the library
should be shareable without redistributing either. Artefacts (invariants,
contracts) are short expressions over parameter names and are kept.

Size: about 2 KB per entry, so one million entries is about 2 GB of JSON,
well under 1 GB compressed. Stored as a FORGE content-addressed store
(`prism::cache`, BLAKE3-keyed, digest-checked on read) for the entries, plus
three sorted index files (exact, source, shape → entry ids), so a lookup is
a binary search and no database dependency is added (`third_party/` stays at
four libraries).

## 4. Phases

### L0. Make every run library-ready (small; do first)

1. Every pir row records `extra.pir_fingerprint` (BLAKE3 of the complete
   serialisation) and `extra.pir_shape` (the abstracted hash), next to the
   existing `pir_hash`.
2. Every pir row records its features (U3) under `extra.features`.
3. `prism replay ROW` (UPGRADE_PLAN P4.1) exists, because it is the T1 check.
- Gate: a run's report has all three fields on every encodable function; the
  conformance suite's verdicts are unchanged.
- Status: items 1 and 2 are in (`extra.pir_fingerprint`, `extra.pir_shape`,
  `extra.features`; `src/prism/pir/fncache.cpp` `exact_key` / `shape_key` /
  `features_json`; `tests/cpp/test_pir_parallel.cpp`). Item 3 is open.
  Runs made before this have `pir_hash` only (the display text's sha256),
  which is not a complete key: L1 recomputes the keys by re-translating
  (clang + translate, no solver) when it imports such a run.

### L1. Import and index

1. `prism library import RUN_DIR... --library DIR [--tier T0|T1] [--vetting FILE]`
   reads `report.json` rows (stage `pir`, and the other solver stages),
   runs the verdict audit on each (`laws` / `verdict.cpp`; a row the audit
   demotes is imported demoted, with the reason), and writes entries and
   indexes. Idempotent: re-importing a run changes nothing.
2. `prism library stats` (by tier, verdict, level, subsystem, NEEDS-HARNESS
   reason) and `prism library export --sample N --stratify ...` (U5).
3. The owner's vetting results come in as a file of (entry or row id, tier
   or Q, note).
- Gate: the owner's one million rows imported on this machine class in
  under an hour; `stats` matches a direct count over the reports.

### L2. Exact reuse with re-check (U1)

In `check_rec`, before encoding: look the function up by `keys.exact` (and
`keys.callees`). On a T1+ hit, try the stored artefacts first, each re-checked:

| artefact | re-check | verdict on success |
|---|---|---|
| closing unwind k | unwind k directly instead of the 4 → 8 → deepening ladder | whatever the check at k says |
| loop invariants | Houdini started from the stored set; base and step proved again | PROVED-UNBOUNDED only if the step is proved now |
| counterexample | concrete replay on the PIR interpreter, then the solver's validation | FAILED only after replay |
| contract / harness | the existing proof-store re-check | PROVED-ASSUMING with the assumptions listed |

On a mismatch or a failed re-check, the normal search runs (and the entry
gets a disagreement mark for U6). `extra.library = "hit <id> (re-checked: ...)"`.
- Gate: re-running the imported kernel trees at least 10× faster than the
  original runs, with every verdict equal or stronger, and 0 wrong proofs on
  the conformance suite.

### L3. Near-match transfer (U2)

Lookup by `keys.shape`: the invariants of the nearest T1+ entries (renamed
to this function's variables through the shape alignment) are added to
Houdini's candidate set; harness shapes from matching pointer signatures
are offered as drafts (still `NEEDS-HARNESS` when a failure appears only
under a draft, as `--pir-drafts` today).
- Gate: the share of benchmark functions at L3+ rises measurably over P2
  alone, with no new unsoundness entry.

### L4. Learning and audits (U3, U4, U6)

1. Train the stage scheduler and budget predictor on `features` → outcome;
   ship only rules that lose no verdict on the U5 benchmark.
2. Generate the P2 priority list from NEEDS-HARNESS reasons.
3. Run the cross-engine and vacuity audits over the library; each finding is
   an incident with a reproducer.

### L5. Retrieval for model features (U7, U8)

Few-shot retrieval by shape for the harness and contract drafters; Lean
lemmas for `prism prove`; FAILED counterexamples as refutation seeds for
the concrete-execution engine.

## 5. Risks

- **Staleness.** An entry from an older engine may be wrong for the new one
  (an encoder bug fixed since). That is why reuse is re-check, never
  trust; and why `provenance` carries the engine digest, so an audit can
  find every entry an engine bug touched.
- **Overfitting the scheduler.** Kernel-trained rules may lose verdicts on
  user code: the gate runs on the conformance suite too.
- **Licensing.** No kernel source in the library (§3). Artefacts are short
  expressions; if the library is published, say which trees and commits
  it came from.
- **Vetting debt.** One million T0 rows are data, not proofs. The library
  says so in every lookup result (`tier`).

## 6. Questions for the owner

1. Were the runs made with PRISM (which commit?), and is `report.json` (or
   `stages.jsonl`) kept per run? If another format, a small adapter goes in
   L1.
2. What exactly is the mechanical check: a PRISM re-run, `--certified`, a
   different tool? That decides whether those rows import as T1.
3. Which kernel versions and configs (they become `provenance.tree` and the
   U5 strata)?
