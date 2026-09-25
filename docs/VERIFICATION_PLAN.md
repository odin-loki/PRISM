# Verification plan after round 6

Round 6 (`realw`, `falar`, `perf6`, `lnprf`, `sv3cm`; see `wip/README.md`) is
merged on `main`. Every GitHub workflow is green on `015b39c8c`: PRISM CI,
the conformance release gate, proofs, the independent Lean recheck and docs.
This plan lists what is still unconfirmed, how to confirm it, and what counts
as a pass. Nothing here adds features.

## What PRISM can and cannot verify about itself

PRISM run on PRISM finds bugs in PRISM's own code: memory errors and
undefined behaviour (ASan + UBSan build), lint and polyglot findings in its
source. It cannot vouch for its own verdicts. A bug in PRISM could hide a bug
in PRISM, so trust in the verdicts comes from sources independent of the
engine:

- the conformance suite's known answers (0 wrong proofs is the gate);
- the Lean kernel rechecking certificates and the LLVM IR → PIR refinement
  (`proofs/`, `tools/pir_lean_check.py`);
- SV-COMP witnesses validated by other tools (CPAchecker, UAutomizer);
- random-program campaigns compared against compiled execution
  (`tools/csmith_soundness.py`).

## Step 1: PRISM on PRISM (sanitizers)

`.github/workflows/self-check.yml` has never run: scheduled workflows run
only from the default branch, and it only reached `main` with round 6.

- Run: trigger it by hand (Actions → "PRISM on PRISM (nightly)" → Run
  workflow), or wait for the 03:17 UTC schedule. Up to about 5 hours.
- It builds with `-DPRISM_SANITIZE=ON`, runs `prism_tests`, pytest and
  `tools/conformance.py -j 2 --mem-limit-mb 0` against that build, and then
  `prism . --no-llm --out prism-self`.
- Pass: every step green (no ASan or UBSan report, conformance 0 wrong
  proofs).
- On failure: every sanitizer report is a real bug in PRISM. Reduce it to a
  test in `tests/cpp/test_main.cpp` or `tests/`, fix it in both engines where
  it applies, and push.
- Self-scan triage: download the `prism-self-sanitized` artifact
  (`report.md`, `report.sarif`). Sort each finding into one of three groups:
  a real bug (fix it and add a test), a false alarm (reduce it to a case in
  `testdata/` and fix the check), or out of scope (for example code under
  `third_party/`). Record the counts at the end of this file.
- The non-gating `selfcheck` job in `ci.yml` (`prism-self-report` artifact,
  inventory + lints + polyglot) gets the same triage once.

## Step 2: confirm the round-6 numbers

The round-6 agents measured these numbers on their own branches, not on
merged `main`.

| claim | where | how to confirm | pass |
|---|---|---|---|
| headline table (pir 273/328 proved, 165/341 refuted; bmc 102/328, 93/341) | `docs/ROADMAP_STATUS.md` | read the `conformance-metrics` artifact of the conformance run on `015b39c8c`, or run `PRISM_BIN=build/prism python tools/conformance.py -j 2 --mem-limit-mb 4096` | 0 wrong proofs, and no count lower than the table |
| SV-COMP enlarged subset: 203 of 417, 0 incorrect | `docs/SVCOMP.md` (measured on the sv3cm binary, loaded machine) | `PRISM_BIN=build/prism python tools/svcomp/run_subset.py --jobs 2` and again with `--property unreach-call` | 0 incorrect, score ≥ 203 |
| real-world before/after table | `docs/EVALUATION.md` (four commits measured separately) | rerun zlib, cJSON, jsmn, tinyexpr, cxxopts at the pinned commits with `--no-llm` | the "after" outcomes hold |
| Houdini cost controls are faster | `perf6` series | time `tools/conformance.py` on `main` against `027e49e7c`, same machine, same `-j`, cold solver cache (`~/.cache/prism/solver` removed) | `main` is not slower |
| refinement: `mismatch=0` | `wip/README.md` | `python tools/pir_lean_check.py tests/pir testdata` | 0 mismatches (already checked on the owner's PC; CI rechecks) |

If a number went up, update the document. If it went down, or there is any
wrong proof or incorrect answer, stop and find the cause before changing
the document.

## Step 3: record

- Update `docs/ROADMAP_STATUS.md` (headline table, rows 6.3 and 6.7),
  `docs/SVCOMP.md` and `docs/EVALUATION.md` with the numbers from `main`,
  the commit and the date.
- Fill in the results table below.
- Delete `wip/` once every row is confirmed: the commits on `main` are the
  record.

## Owner actions (not in this plan's scope)

The history rewrite and force-push (`scripts/rewrite_history.sh`), legal
review of the AGPL-3.0-only licence, pushing the first `v*` tag for a signed
release, and GPU/LoRA work on the owner's hardware.

## Results

| step | commit | date | result |
|---|---|---|---|
| 1 sanitizers | | | |
| 1 self-scan triage (real / false alarm / out of scope) | | | |
| 2 headline table | | | |
| 2 SV-COMP subset | | | |
| 2 real-world rerun | | | |
| 2 Houdini timing | | | |
