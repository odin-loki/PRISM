# laws status strings

Slice `finish/laws-status`. `include/prism/laws.hpp` names the status
vocabulary. `prism::laws::is_proof` forwards to the verdict lattice.

## What is a proof

`is_proof` is true for:

- `PROVED` (`laws::PROVED`)
- `PROVED-UNBOUNDED` (`laws::PROVED_UNBOUNDED`)
- `PROVED-CERTIFIED` (`laws::PROVED_CERTIFIED`)
- `PROVED-ASSUMING` (`laws::PROVED_ASSUMING`)

`is_proof` is false for `BOUNDED`, `CLEAN`, `NOTRUN`, `HYPOTHESIS`,
`FAILED`, `TIMEOUT`, `UNKNOWN`, and `NEEDS-HARNESS`.

The constants stay distinct strings: `PROVED` and `BOUNDED`, `CLEAN` and
`PROVED`, `NOTRUN` and `CLEAN`, `HYPOTHESIS` and `PROVED`.

## Tests

`tests/cpp/test_laws_status.cpp` (one new line in `_prism_test_sources`;
the test target lists sources and does not glob):

- `laws status: the four PROVED strings are proofs` — `is_proof` on the
  four proof constants and on `BOUNDED`, `CLEAN`, `NOTRUN`, `HYPOTHESIS`,
  `FAILED`, `TIMEOUT`, `UNKNOWN`, and `NEEDS-HARNESS`, plus the four
  string inequalities above.

`include/prism/laws.hpp` already had every constant. `prism_tests` was
not executed in this worktree: this slice does not configure or build.
