# RLEF repair score

A repair candidate with no BMC proof stays `HYPOTHESIS`. Strength stays `READS`. The score and the history extras stay on the row.

## Status

`rlef_repair_with` still returns early when BMC proved the candidate. That row is `HYPOTHESIS` with strength `READS`, and `extra.patch_verdict` holds the BMC status. That return is unchanged.

When the loop ends without that proof, the fall-through status is `HYPOTHESIS` for every score, including `best_score >= 3`. A sandbox pass is a score, not a proof of the scanned code and not a proof of the patch (Law 4). The message is still `RLEF best score N over M rounds (not a proof)`. `extra.history` and `extra.best` are still set. This stage does not use `PROVED` or `PROVED-UNBOUNDED` as its own status.

## Tests

`tests/cpp/test_agent.cpp` ("a proof is about the patch only"):

- BMC `PROVED` on the candidate stays `HYPOTHESIS` / `READS` with `patch_verdict` `PROVED`.
- BMC `BOUNDED` (the candidate ran, score 3, no proof) is `HYPOTHESIS` / `READS`. The message still carries the score and `not a proof`. `extra.history` and `extra.best` are present. The status is neither `PROVED` nor `PROVED-UNBOUNDED`.

`tests/cpp/test_repair.cpp` still requires the verified-fix row to be `HYPOTHESIS` with a proof-valued `patch_verdict`. That check is unchanged.
