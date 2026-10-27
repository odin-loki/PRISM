# CLI --skip accumulation

Slice `finish/cli-skip`. `parse_cli` (`src/prism/cli.cpp`) fills
`Config.skip`. `Config::want` is false for a name on that list. An unset
`Config.stages` still means every name in `STAGE_ORDER`. `main` returns 2
when `parse_cli` yields `CliError`. `--fail-on` is unchanged.

## Behaviors

1. Repeating `--skip` appends. `--skip bmc --skip pir` keeps both `bmc`
   and `pir`, so `want` is false for those two and true for the other
   stages. The previous assignment replaced the vector, so a second
   `--skip` dropped the first names.
2. An empty `--skip` (`--skip=`) does not clear names already skipped.
   `--skip bmc --skip=` still skips `bmc`. A lone empty `--skip` leaves
   the list empty, the same as omitting the flag. It does not mean "skip
   every stage".
3. An empty `--stage` still does not clear a selection already made and
   does not mean "run nothing". `--stage bmc --stage=` keeps only `bmc`.
   A lone empty `--stage` leaves `cfg.stages` unset, which is every stage.
4. An unknown `--skip` name is `CliError`
   (`argument --skip: unknown stage '…'`), the same path as `--stage`.
   There is no new flag that turns a check off quietly.

## Tests

`tests/cpp/test_cli_stages.cpp` (already registered):

- `cli skip: repeating --skip accumulates and an empty --skip keeps names`
- `cli stages: repeating --stage accumulates` (`--stage bmc --stage=`)
- `cli skip: an unknown skip name is CliError`

`prism_tests` was not executed in this slice.
