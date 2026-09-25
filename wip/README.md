# Unmerged work (round 6): what exists and what has to be done

These five branches were stopped before they were tested and merged. Each is
saved here as a `git format-patch` series against
`claude/prism-code-checker-x7538r` at `027e49e7c`, the last commit that is
green on every GitHub workflow. Nothing in `wip/` is built or run.

To restore a branch:

```
git switch -c <name> 027e49e7c
git am wip/<name>.mbox
```

| Series | Commits | State |
|---|---|---|
| `realw.mbox` | 15 | Real-world evaluation (zlib, cJSON, …): engine fixes, `docs/EVALUATION.md`. Nearly finished; the agent's own checks were partial. |
| `falar.mbox` | 9 | Refinement findings 2, 5, 6, 7 fixed in `translate` and mirrored in Lean; whole process trees killed on timeout. Stopped at the full pytest run. |
| `perf6.mbox` | 9 | Houdini cost controls (optimistic query, outcome cache, run budget), memory value sets for map/set, doctests pass (340). Stopped before its conformance run. |
| `lnprf.mbox` | 6 | Lean: loop-cut soundness with Houdini invariants and the S9 sequential-assume guard (proofs/techniques); refinement extended to pointers and the libc models' heap. Unverified: `proofs/check.sh` was not run at the end. |
| `sv3cm.mbox` | 5 | bmc reports reachable `reach_error()` and covers unreach-call; enlarged pinned SV-COMP subset (most of the 1.6 MB); `run_subset.py` kills process groups. Last WIP commit is untested. |

## What has to be done before any of this is merged

For each series, on a branch from `027e49e7c` (or the current tip, resolving
conflicts):

1. `git am wip/<name>.mbox`, then build (`cmake --build build`).
2. `./build/prism_tests`.
3. `PRISM_BIN=build/prism python -m pytest -q tests`, `ruff check .`,
   `mypy prism`.
4. `PRISM_BIN=build/prism python tools/conformance.py -j 2
   --mem-limit-mb 4096`. It must report 0 wrong proofs.
5. Extra checks for some series:
   - `falar` and `lnprf`: in `proofs/refinement` (and `proofs/techniques`
     for lnprf), run `lake build`, then `proofs/check.sh` and
     `proofs/refinement/check.sh` (only the axioms propext, Classical.choice
     and Quot.sound), then `tools/pir_lean_check.py tests/pir testdata`
     (0 mismatches).
   - `sv3cm`: keep the enlarged task set out of the default conformance
     suite (opt-in only), or CI time grows. Also re-score the SV-COMP subset.
   - `falar` and `sv3cm` both change process-group killing. `027e49e7c`
     already kills process groups in `tools/conformance.py`, so merge those
     parts by hand.
6. Merge in this order: `realw`, `falar`, `perf6`, `lnprf`, `sv3cm`. Push
   each one and confirm every GitHub workflow is green before the next.

Measured numbers claimed inside these series are the agents' own and are not
yet confirmed by a gate run.
