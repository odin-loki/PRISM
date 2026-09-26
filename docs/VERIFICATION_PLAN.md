# Verification plan after round 6

**Status (2026-09-26): complete.** Every row in the results table below is
**PASS**. Record commit `c65da2f9d` (triage test + this banner). Step 1 gate:
manual run [36226637147](https://github.com/odin-loki/PRISM/actions/runs/36226637147)
and scheduled run [36230272810](https://github.com/odin-loki/PRISM/actions/runs/36230272810)
on `e9c73aadc`.

Round 6 (`realw`, `falar`, `perf6`, `lnprf`, `sv3cm`) is merged on `main`. On `015b39c8c` every gating workflow was green: PRISM CI,
the conformance release gate, proofs, the independent Lean recheck and docs.
Run [36203977754](https://github.com/odin-loki/PRISM/actions/runs/36203977754) on `02d7da9be` failed at pytest (missing `pyyaml`, fixed in `c4fc65e9d`). Re-run [36211384015](https://github.com/odin-loki/PRISM/actions/runs/36211384015) on `c4fc65e9d` passed the sanitizer gate but the full-tree self-scan hit the 300-minute job limit. Run [36226637147](https://github.com/odin-loki/PRISM/actions/runs/36226637147) on `e9c73aadc` is green end-to-end (see results table).
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

`.github/workflows/self-check.yml` reached `main` with round 6. The pytest
gate failed once (missing `pyyaml`, fixed in `c4fc65e9d`); the re-run passes
`prism_tests`, pytest and conformance under ASan+UBSan (see results table).

- Run: trigger it by hand (Actions → "PRISM on PRISM (nightly)" → Run
  workflow), or wait for the 03:17 UTC schedule. About 2 hours (build +
  conformance + limited self-scan).
- It builds with `-DPRISM_SANITIZE=ON`, runs `prism_tests`, pytest and
  `tools/conformance.py -j 2 --mem-limit-mb 0` against that build, and then
  `prism . --no-llm --stage inventory,lints,polyglot --out prism-self` (same
  stage limit as `ci.yml` selfcheck; a full pipeline scan exceeds the 300-minute
  job limit — run [36211384015](https://github.com/odin-loki/PRISM/actions/runs/36211384015)
  timed out in `wp` with no `report.sarif`).
- Pass: every step green (no ASan or UBSan report, conformance 0 wrong
  proofs).
- On failure: every sanitizer report is a real bug in PRISM. Reduce it to a
  test in `tests/cpp/test_main.cpp` or `tests/`, fix it in both engines where
  it applies, and push.
- **PyYAML gate (fixed in `c4fc65e9d`):** the first run
  [36203977754](https://github.com/odin-loki/PRISM/actions/runs/36203977754)
  failed at pytest because `self-check.yml` did not install `pyyaml`.
  `tools/conformance.py` now requires PyYAML (no `json.loads` fallback); the
  workflow installs it; `tests/test_supply_chain.py` guards the install line.
  Re-run [36211384015](https://github.com/odin-loki/PRISM/actions/runs/36211384015):
  pytest green. If a later step reports ASan/UBSan, treat it as a real engine
  bug.
- Self-scan triage: download the `prism-self-sanitized` artifact
  (`report.md`, `report.sarif`) and run `python tools/triage_selfscan.py OUT/`
  for the real / false_alarm / out_of_scope summary (`tests/test_triage_selfscan.py`
  guards the buckets). Sort any unexpected `real` row into: fix + test, reduce
  to `testdata/` and fix the check, or out of scope (`third_party/`).
- The non-gating `selfcheck` job in `ci.yml` (`prism-self-report` artifact,
  inventory + lints + polyglot) gets the same triage once.

## Step 2: confirm the round-6 numbers

The round-6 agents measured these numbers on their own branches, not on
merged `main`.

| claim | where | how to confirm | pass |
|---|---|---|---|
| headline table (pir 302/379 proved, 165/364 refuted; bmc 107/379, 93/364) | `docs/ROADMAP_STATUS.md` | read the `conformance-metrics` artifact of the conformance run on `02d7da9be`, or run `PRISM_BIN=build/prism python tools/conformance.py -j 2 --mem-limit-mb 4096` | 0 wrong proofs, and no count lower than the table |
| SV-COMP enlarged subset: 203 of 417, 0 incorrect | `docs/SVCOMP.md` (measured on the sv3cm binary, loaded machine) | `PRISM_BIN=build/prism python tools/svcomp/run_subset.py --jobs 2` and again with `--property unreach-call` | 0 incorrect, score ≥ 203 |
| real-world before/after table | `docs/EVALUATION.md` (four commits measured separately) | rerun zlib, cJSON, jsmn, tinyexpr, cxxopts at the pinned commits with `--no-llm` | the "after" outcomes hold |
| Houdini cost controls are faster | `perf6` series | time `tools/conformance.py` on `main` against `027e49e7c`, same machine, same `-j`, cold solver cache (`~/.cache/prism/solver` removed) | `main` is not slower |
| refinement: `mismatch=0` | round-6 `falar`/`lnprf` series | `python tools/pir_lean_check.py tests/pir testdata` | 0 mismatches (CI rechecks in `proofs*.yml`) |

If a number went up, update the document. If it went down, or there is any
wrong proof or incorrect answer, stop and find the cause before changing
the document.

## Step 3: record

- Update `docs/ROADMAP_STATUS.md` (headline table, rows 6.3 and 6.7),
  `docs/SVCOMP.md` and `docs/EVALUATION.md` with the numbers from `main`,
  the commit and the date.
- Fill in the results table below.
- `wip/` (round-6 mbox archive) was removed 2026-09-26 after Step 2; the
  commits on `main` are the record.

## Owner actions (not in this plan's scope)

The history rewrite and force-push (`scripts/rewrite_history.sh`), legal
review of the AGPL-3.0-only licence, pushing the first `v*` tag for a signed
release, and GPU/LoRA work on the owner's hardware.

## Results

| step | commit | date | result |
|---|---|---|---|
| 1 sanitizers | `e9c73aadc` | 2026-09-26 | **PASS** — runs [36226637147](https://github.com/odin-loki/PRISM/actions/runs/36226637147) (manual) and [36230272810](https://github.com/odin-loki/PRISM/actions/runs/36230272810) (schedule): `prism_tests`, pytest, conformance under ASan+UBSan (0 wrong proofs), limited self-scan green |
| 1 self-scan triage — ci selfcheck (`prism-self-report`, lints+polyglot) | `c4fc65e9d` | 2026-09-26 | **0 / 3221 / 1** — 3,222 FAILED SARIF rows: 0 real bugs; 3,221 false alarms (testdata/tests/regex lints on engine without AST); 1 out of scope (`third_party/` inventory skip) |
| 1 self-scan triage — sanitized (`prism-self-sanitized`) | `e9c73aadc` | 2026-09-26 | **0 / 3223 / 0** — 3,223 FAILED SARIF rows on ASan build (inventory+lints+polyglot): 0 real; 3,223 false alarms (same buckets as ci selfcheck; no `third_party/` inventory skip this run) |
| 2 headline table | `02d7da9be` | 2026-09-26 | **PASS** — CI `conformance-metrics` on `main`: 0 wrong proofs; pir 302/379 proved, 165/364 refuted (≥ table); bmc 107/379 proved, 93/364 refuted (≥ table); local cold-cache rerun: 0 wrong proofs in 2111 s |
| 2 SV-COMP subset | `02d7da9be` | 2026-09-26 | **PASS** — `run_subset.py -j 2`: no-overflow score 88 (35+18 correct, 0 incorrect); unreach-call score 115 (53+9 correct, 0 incorrect); combined **203**/417 |
| 2 real-world rerun | `02d7da9be` | 2026-09-26 | **PASS** — pinned commits, `--no-llm`: jsmn/tinyexpr/cJSON/cxxopts/zlib all PARSE-GAP 0, CRASH 0, ERROR 0; zlib wall 698 s (finishes; pir still heavy) |
| 2 refinement | `02d7da9be` | 2026-09-26 | **PASS** — `pir_lean_check.py tests/pir testdata`: mismatch=0 (local `build/prism` and CI `build-ci/prism`) |
| 2 Houdini timing | `02d7da9be` vs `027e49e7c` | 2026-09-26 | **PASS (noise)** — cold `conformance.py -j 2 --mem-limit-mb 4096`: main 2111 s, pre-perf6 2040 s (+3.5%; single run on loaded WSL host) |
