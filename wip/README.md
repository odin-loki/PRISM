# Round 6 series, applied on main

These five series were saved as `git format-patch` files against
`027e49e7c` and have been applied onto `main`, in this order: `realw`,
`falar`, `perf6`, `lnprf`, `sv3cm`. The mbox files are the series as saved.
The commits on `main` are the result.

Resolutions while applying:

- `perf6`: `basic_string.tcc` already had the libstdc++ 14 pointer-range
  guard, so that hunk was left as it was.
- `lnprf`: the refinement docs keep both the findings 2/5–7 rows and the
  pointer/heap measurements.
- `sv3cm`: `tools/svcomp/run_subset.py` uses the existing `PROCTREE` helper
  to kill the process group, and does not start the command twice.

| Series | Commits | State |
|---|---|---|
| `realw.mbox` | 15 | Real-world evaluation (zlib, cJSON, …): engine fixes, `docs/EVALUATION.md`. Nearly finished; the agent's own checks were partial. |
| `falar.mbox` | 9 | Refinement findings 2, 5, 6, 7 fixed in `translate` and mirrored in Lean; whole process trees killed on timeout. Stopped at the full pytest run. |
| `perf6.mbox` | 9 | Houdini cost controls (optimistic query, outcome cache, run budget), memory value sets for map/set, doctests pass (340). Stopped before its conformance run. |
| `lnprf.mbox` | 6 | Lean: loop-cut soundness with Houdini invariants and the S9 sequential-assume guard (proofs/techniques); refinement extended to pointers and the libc models' heap. Unverified: `proofs/check.sh` was not run at the end. |
| `sv3cm.mbox` | 5 | bmc reports reachable `reach_error()` and covers unreach-call; enlarged pinned SV-COMP subset (most of the 1.6 MB); `run_subset.py` kills process groups. Last WIP commit is untested. |

Measured numbers claimed inside these series are the agents' own. The
pointer fixture was exported before the memcpy self-copy guard (finding 6:
the offsets must differ) landed in the translator, so the Lean recheck
reported 3 mismatches. It has been regenerated from `tests/pir/mem_ptr.c`,
`tests/pir/mem_contract.c` and `testdata/stack_escape.c`. `pir_lean_check`
now reports `agree=0 agree-ext=24 agree-reject=4 outside=5 mismatch=0`.
