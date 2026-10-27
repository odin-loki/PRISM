# functions.json read

Slice `finish/journal-resume`. `journal_read_functions` (`src/prism/journal.cpp`)
already returns an empty vector when any `functions.json` element is
malformed, and reports that count through the optional `std::size_t*`. This
slice locks that reader. The all-or-nothing return is unchanged, and
`src/prism/journal.cpp` is unchanged.

## Contract

`journal_write_functions` writes a JSON array of `FunctionInfo` objects
(`to_json` in `src/prism/models.cpp`). `journal_read_functions` walks the
whole array.

- A file of only well-typed objects returns those functions. The malformed
  count is 0.
- One valid object beside one non-object (a JSON string) yields an empty
  vector. The malformed count is 1.
- The count is stored before the empty return, so a caller can tell a
  rejected file from a missing one.

`--resume` keeps a stage whose journal row is `ok` or `NOTRUN`, except
classify when the function list is empty (`src/prism/pipeline.cpp`, the
`classify && functions.empty()` skip). An empty list from a bad
`functions.json` therefore reruns classify. A non-zero count also blocks the
fallback that would copy functions out of `report.json` when `stages.jsonl`
is absent (`fns.empty() && !malformed`). The pipeline writes
`functions.json: N malformed entries; classify rerun` (Law 7). A partial
list would look like a finished classify and those functions would never be
analysed.

`pipeline.cpp` is owned by another slice and was not edited. The resume
behavior above is what that code already does with this reader.

## Tests

Added at the end of `tests/cpp/test_journal_confidence.cpp`. Existing cases
were left as they are, including `journal: one malformed function entry
discards the list, never a partial one` (hand-written mix, count 2).

- `journal: a file of only valid functions is returned` — two `FunctionInfo`
  values through `journal_write_functions`, then `journal_read_functions`.
  The vector is those functions (file, name, kind, line, signature, params,
  return type, static, body, span, body position) and the count is 0.
- `journal: one valid function and one non-object is an empty read` — the
  same writer, read back, then one JSON string appended. The vector is empty
  and the malformed count is 1.

Not executed in this worktree: no `prism_tests` binary is here, and the
build stays off this tree.

## Stale sentence

`docs/CPP_PORT_PLAN.md` still says the reader stops at the first malformed
element because one try wraps the loop. The reader counts every bad element,
then returns nothing when the count is non-zero. That file was not edited.
