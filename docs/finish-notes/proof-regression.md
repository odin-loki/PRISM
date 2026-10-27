# Proof regression

Slice on `src/prism/ai/regress.cpp` and `src/prism/ai/proof_repair.cpp`. Both files already report an honest status when the store or the checker is missing, so this slice only locks that behavior. No Lean install is required.

## Proof store (`proof_repair.cpp`)

`run_proof_regression` loads `<out>/proof_store.json`, and `$PRISM_PROOF_CACHE` when that variable is set. A missing file, an unreadable file, or JSON that is not a store loads as no entries.

The missing-store result is an empty finding list. The check still runs and writes the store from this run's proof-class findings. A `CLEAN` finding is outside that class, so it stays out of the store. That empty list is the locked status: the check ran, and there was no stored proof to re-check. `docs/AI.md` describes the same first run.

An artefact the checker cannot re-apply (invariants that are not JSON) comes back from `recheck_artefact` as `laws::UNKNOWN`. `run_proof_regression` reports one `UNKNOWN` row, class `PROOF-REGRESSION`, `extra.regression = "true"`. With no model bound, `extra.repair` starts with `NOTRUN`.

On a build without Z3, `run_bmc` returns `laws::NOTRUN` (`"z3 not built"`). Re-checking a stored BMC artefact keeps that `NOTRUN` on the re-check, and the regression row is `PROOF-REGRESSION`. Builds with Z3 keep the existing contract and Houdini cases.

`proof_repair.cpp` does not call Lean. A missing lake is `prove_theorem` in `src/prism/ai/proof_search.cpp`: status `laws::NOTRUN`, an empty proof, and a reason that names lake. An explicit `PRISM_LAKE` that is not a file stays missing; `PATH` does not replace it.

## `prism regress` (`regress.cpp`)

`generate_regression_tests` uses its own test statuses.

- `--run` without `--allow-exec` is `NOTRUN` (Law 9).
- A compiler that starts and whose sanitizer runtime is missing is `NOTRUN` (Law 1).
- A compiler that cannot be started (`b.failed`) is `compile-error`, with detail `"<cc> not found"`.

## Tests

`tests/cpp/test_ai9.cpp`

- `ai9 lean: a missing lake is NOTRUN, never PROVED or CLEAN` — `PRISM_LAKE` points at a missing file.
- `ai9 proof store: a missing store returns no finding` — no `proof_store.json`, plus a `CLEAN` stage finding. Result is empty; the written store is empty.
- `ai9 proof store: an artefact the checker cannot re-apply is PROOF-REGRESSION` — non-JSON invariants. No Z3 and no Lean.
- `ai9 proof store: without Z3 a stored BMC proof is not re-proved` — compiled only when `PRISM_HAS_Z3` is off. Re-check is `NOTRUN`; the row is `PROOF-REGRESSION`.

`tests/cpp/test_ai_assist.cpp`

- `ai-assist regress: a missing compiler is not a pass` — `--run --allow-exec` with `prism-no-such-cc`. Status `compile-error`.
