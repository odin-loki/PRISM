# CLI stage selection

Slice `finish/cli-all-stages`. `parse_cli` (`src/prism/cli.cpp`) fills
`Config.stages`. `Config::want` treats an unset selection as every name in
`STAGE_ORDER` (`include/prism/pipeline.hpp`). `main` returns 2 when
`parse_cli` yields `CliError`, before `run_pipeline`. A stage `want`
rejects is still written as `skipped` (`excluded by --stage/--skip`).

## Behaviors

1. Omitting `--stage` leaves `cfg.stages` unset. That is every stage, not
   none. An empty `--stage` (`""`, `" , "`, `--stage=`) does the same.
   An engaged empty list would make `want` false for the whole order;
   parse does not produce one.
2. An unknown stage name was already `CliError`
   (`argument --stage: unknown stage '…'`), which `main` turns into exit
   2. It was not ignored. A later unknown name fails the parse the same
   way. This slice did not change that path.
3. Repeating `--stage` appends. `--stage bmc --stage pir --stage=fuzz`
   keeps `bmc`, `pir`, and `fuzz`. The previous assignment replaced the
   vector, so an earlier name was dropped. A later empty `--stage` does
   not clear names already chosen.
4. There is no `--disable` or `--skip-stage` flag. `--no-bmc`,
   `--skip-stage`, and `--disable` are `unrecognized arguments` (`CliError`,
   exit 2). They do not turn a stage off.

## Tests

`tests/cpp/test_cli_stages.cpp` (one new line in `_prism_test_sources`):

- `cli stages: omitting --stage selects every stage`
- `cli stages: an unknown stage name is CliError`
- `cli stages: repeating --stage accumulates`
- `cli stages: --no-bmc, --skip-stage and --disable are CliError`

`prism_tests` was not executed in this slice.
