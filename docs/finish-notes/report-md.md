# report.md statuses

`write_report_md` in `src/prism/pipeline.cpp` already prints each finding's `f.status` as itself. `PROVED` and `BOUNDED` are separate words (Law 2). `TIMEOUT` and `UNKNOWN` are not filtered. A `NOTRUN` or `CLEAN` finding is omitted only when its stage is `inventory`, `classify`, or `unify`. Any other stage's `NOTRUN` line stays (Law 7). The confidence paragraph is the fixed sentence "Confidence is a product. 0 means no data, not clean." `pipeline.cpp` was not edited.

## Tests

`tests/cpp/test_report_md.cpp` (one new line in `_prism_test_sources`):

- `write_report_md keeps PROVED and BOUNDED and drops inventory NOTRUN` — a `bmc` stage with `PROVED`, `BOUNDED`, `TIMEOUT`, `UNKNOWN`, and `NOTRUN`, plus an `inventory` stage with `NOTRUN`. The markdown contains the four bmc statuses and the bmc `NOTRUN` line, does not contain the inventory `NOTRUN` line, and contains the confidence sentence that 0 means no data, not clean.

`prism_tests` was not executed in this slice.
