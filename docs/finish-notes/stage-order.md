# `--stage` and `STAGE_ORDER`

Slice `finish/stage-order`. `parse_cli` (`src/prism/cli.cpp`) accepts a
`--stage` name when `unknown_stage` finds it in `STAGE_ORDER`
(`include/prism/pipeline.hpp`). This slice does not change `cli.cpp` or
`pipeline.hpp`. `--skip` is left to another change.

## Behaviors

1. Every name in `STAGE_ORDER` parses. The list has 31 names:

   inventory, classify, lints, taint, thread, interval, warnings,
   cppcheck, pbsd, sanitize, optional, polyglot, esbmc, dafny, contracts,
   wp, bmc, pir, conc, harness, review, concolic, fuzz, diff, rapid,
   muttest, ltl, llm, execute, repair, unify.

   The allow-list is that array. Each of those names is accepted.
   `--stage NAME` sets `cfg.stages` to that one name, and `want(NAME)`
   is true.
2. An unknown name is `CliError`. `--stage not-a-stage` yields
   `argument --stage: unknown stage 'not-a-stage' (stages: …)`, which
   `main` turns into exit 2.
3. Omitting `--stage` leaves `cfg.stages` unset. `Config::want` treats
   an unset selection as every stage (`src/prism/config.cpp`).

`parse_cli` takes the scan arguments after the program name
(`include/prism/cli.hpp`). The test uses the helper shape from
`tests/cpp/test_cli_stages.cpp`: `{"--stage", name}`. The default path
stays `testdata`.

## Tests

`tests/cpp/test_stage_order.cpp` (one new line in `_prism_test_sources`):

- `stage order: every STAGE_ORDER name is accepted by --stage`
- `stage order: an unknown --stage name is CliError`
- `stage order: omitting --stage leaves the selection unset`

`prism_tests` was not executed in this slice.
