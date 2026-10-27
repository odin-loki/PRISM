# journal line

Slice `finish/journal-line`. `journal_read_stages` (`src/prism/journal.cpp`)
already skips a `stages.jsonl` line that does not parse as a stage object
and keeps every later line. This slice locks that reader. `src/prism/journal.cpp`
is unchanged.

## Contract

`journal_append_stage` writes one JSON object per line (`to_json` of
`StageResult`, then a newline). `journal_read_stages` reads a line at a
time. A line that throws is skipped. The next line is still read, and its
`status` is the string that was written.

- Two written lines, the first replaced with `{not json`: the second line
  remains. `PROVED` stays `PROVED`. `NOTRUN` stays `NOTRUN`. Name, records,
  and detail of that line stay with it. The kept row is not `CLEAN`.
- A file of only lines that do not parse (`{not json`, `1`, `[]`, the JSON
  string `"CLEAN"`, and a truncated object whose status text is `CLEAN`) is
  an empty vector. No `CLEAN` stage is invented.

`journal_read_functions` is a different rule: one malformed `functions.json`
element returns an empty function list so `--resume` cannot skip classify
(Law 7). That reader was not changed. It is already locked in
`tests/cpp/test_journal_confidence.cpp` (`journal: one malformed function
entry discards the list, never a partial one`, and the resume note case).

## Tests

`tests/cpp/test_journal_line.cpp` (one new line in `_prism_test_sources`;
the test target lists sources and does not glob).
`tests/cpp/test_journal_confidence.cpp` was not edited.

- `journal line: a corrupt first line keeps the second status` — two
  `journal_append_stage` lines, the first line corrupted, the second read
  back once as `PROVED` and once as `NOTRUN`.
- `journal line: only corrupt lines are an empty stage list` — five bad
  lines, empty vector, no `CLEAN` row.

Not executed in this worktree: no `prism_tests` binary is here, and this
slice does not configure or build.
