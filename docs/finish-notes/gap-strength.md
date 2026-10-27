# Gap strength

Slice `finish/gap-strength`. A finding that did not prove anything uses
`laws::STRENGTH_READS`. Status text is unchanged. `laws::is_proof` is
unchanged and is false for `ERROR` and `NEEDS-HARNESS`.

## Rows

`bmc_one` in `src/prism/stages/common.cpp`. When `run_bmc` returns no
finding, the `ERROR` row whose message is `BMC produced no result` uses
`laws::STRENGTH_READS`.

`fail` inside `bmc_with_assume` (`src/prism/stages/contracts.cpp`) builds
every `ERROR` for that helper with `laws::STRENGTH_READS`. That covers an
invariant or a decreases clause that cannot be encoded or instrumented, and
a value-returning function whose body has `return;` under an ensures.

`prove_with_contract`. A function whose kind is not `SCALAR`, or whose body
needs a pointer harness, is `NEEDS-HARNESS`. The message contains
`not a proof`. Strength is `laws::STRENGTH_READS`. A `POINTER` parameter
takes this path (`POINTER: contract scalar subset only; not a proof`).

`run_wp` in `src/prism/stages/wp.cpp` uses `laws::STRENGTH_READS` on:

- a `POINTER` function, or a body that needs a pointer harness
  (`POINTER: WP would invent a buffer; Frama-C WP binary is not a proof either`)
- a kind other than `SCALAR` (message contains `not a proof`)
- a scrubbed byte in a scalar body (`NEEDS-HARNESS`)
- a requires or ensures the predicate encoder refuses (`ERROR`, message
  `cannot encode WP predicate`)

## Unchanged verdicts

`PROVED`, `PROVED-UNBOUNDED`, `PROVED-CERTIFIED`, and `PROVED-ASSUMING`
stay `laws::STRENGTH_PROVES`. A tautological WP obligation is still
`PROVED-ASSUMING` with that strength. `prove_contracts` keeps its own
`NEEDS-HARNESS` call. `src/prism/stages/ltl.cpp`, `src/prism/adapters.cpp`,
`laws::is_proof`, `tests/cpp/test_contracts.cpp`, `tests/cpp/test_adapters.cpp`,
and `tests/cpp/test_ltl.cpp` were not edited.

## Tests

`tests/cpp/test_gap_strength.cpp` is one new line in `CMakeLists.txt`
`_prism_test_sources` (the test target lists sources and does not glob).

- One `POINTER` function, `gap_ptr(int *p)`, is `NEEDS-HARNESS` from both
  `prove_with_contract` and `run_wp`. Strength is `READS`. `is_proof` is
  false. The messages contain `not a proof`.
- `run_wp` on a `VOID` function, on a scalar body with a scrubbed byte, and
  on an ensures of `\valid(&x)`: the first two are `NEEDS-HARNESS`, the
  third is `ERROR`. All three are `READS`.
- `prove_contracts` on a scalar whose decreases clause is `n * 2` hits
  `fail`: status `ERROR`, strength `READS`, `decreases_unencoded` is `true`.
- `run_bmc` on one function does not return an empty list without Z3 (the
  missing backend is one `NOTRUN` row). The test reads
  `src/prism/stages/common.cpp` and checks the `BMC produced no result`
  `make_find` passes `laws::STRENGTH_READS`.
- `testdata/planted/contract_add.c` function `inc` stays `PROVED-ASSUMING`
  with `STRENGTH_PROVES`, and `is_proof` is true.

`prism_tests` was not executed. This checkout stays off the OneDrive build path.
