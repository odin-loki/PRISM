# CLI argv

Slice `finish/cli-argv`. `main` (`src/prism/main.cpp`) sends the scan
arguments to `parse_cli` (`src/prism/cli.cpp`) and returns 2 on a parse
error, before `run_pipeline`.

## Behaviors

1. `--flag=value` applies for value-taking flags, including `--fail-on`,
   `--stage`, `--out`, `--skip`, `--unwind`, and `--jobs` (and `-jN`).
   `--fail-on=defect` sets the fail-on policy, so a reported defect exits 1.
2. An unknown flag such as `--bogus-flag` is
   `unrecognized arguments: --bogus-flag`. `main` prints
   `prism: error: …` and exits 2 before `run_pipeline`.
3. A value-taking flag with no following value (`prism --stage`, and the
   same for `--fail-on`, `--out`, `--skip`, `--unwind`, `--jobs`) is
   `argument <flag>: expected one argument` and exit 2, before
   `run_pipeline`.
4. A valid invocation keeps the same flags, defaults, and exit codes
   (`--fail-on never`, `--out prism-out`, `--unwind 8`, and the rest of
   `default_config`). Law 8 holds: a typo is an error.

An empty `--stage ""` still means every stage. `--gui` is still handed to
`prism_gui` before scan parsing. Subcommands (`prove`, `regress`, `ask`,
`draft`, `svcomp`, `triage`) are unchanged.

Parsing landed in `d8d7f278f` (`Parse the command line strictly in a
testable parse_cli`). This slice locks the three cases above in
`tests/cpp/test_cli.cpp`. `src/prism/cli.cpp` is unchanged.

## Tests

`tests/cpp/test_cli.cpp`

- `cli: --flag=value, unknown flags, and a missing value` — in-process
  `parse_cli` for the joined forms, `--bogus-flag`, `--fail-on=bogus`, and
  a bare `--fail-on` / `--stage` / `--out` / `--skip` / `--unwind` / `--jobs`.
- Existing in-process cases for both spellings, unknown options, malformed
  values, `--fail-on` choices, and stage names.
- `prism binary: exit 2 with a message on bad arguments, never an abort or a scan`
  — process exit 2, including bare `--fail-on`, `--out`, `--skip`, `--jobs`,
  `--unwind`, and `--jobs=`. The captured text is the error line, with no
  confidence line from a scan.
- `prism binary: --fail-on=defect is honoured (conflict marker -> exit 1)`.

Not executed in this worktree: no `prism` / `prism_tests` binary is
present, and the build stays off this tree (OneDrive and `C:\Temp`).
