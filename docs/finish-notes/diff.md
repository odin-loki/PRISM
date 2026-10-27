# diff

Closed the three `prism/diff.py` gaps in `src/prism/stages/diff.cpp`. Statuses are unchanged: a disagreement stays `FAILED`, agreement stays `CLEAN`, and a timeout or crash stays `TIMEOUT` or `CRASH`.

## Sandbox on a disagreement

A `FAILED` pair sets `extra["sandbox"]` to `sandbox::kind()`. The harness run is the step inside the sandbox (`sandbox::wrap_argv`). Compilation stays on `run_argv` with the compiler's own argv, the same as the Python harness, which compiled outside the sandbox and wrapped only the built binary.

## Stderr evidence

Disagree evidence is `tail(rr.err, 800)`, the last 800 characters of the harness stderr. The `DIFF` line is printed after the two calls, so a long write in front of it stays out of the finding. `tests/cpp/test_diff.cpp` ("a disagreement keeps the last 800 characters of stderr") writes a prefix marker, 900 filler bytes, and a tail marker, then checks the finding is 800 characters, has lost the prefix marker, and still holds `PRISMDIFFTAIL` and `DIFF 0 1`.

## C type width

`c_type_nbytes_key` collapses internal whitespace (`collapse_ws`, the same rule as `" ".join(typ.split())`) before the `kCTypeSize` lookup. The harness declaration uses that collapsed key, so `unsigned \t  char` is one byte. The doctest expects the counterexample `2a` for that parameter type.

## Tests

`tests/cpp/test_diff.cpp` (already in the `prism_tests` sources):

- a disagreeing `*_a`/`*_b` pair records `sandbox::kind()`
- stderr longer than 800 characters keeps the tail, including the `DIFF` line
- `unsigned \t  char` disagrees on the one-byte input `2a`
