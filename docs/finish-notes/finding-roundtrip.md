# finding roundtrip

Slice `finish/finding-roundtrip`. `src/prism/models.cpp` was read through. `to_json` / `from_json` copy `Finding::status` and `StageResult::status` as JSON strings. Nothing rewrites one status word into another.

`BOUNDED` stays `BOUNDED` (it does not come back as `PROVED`). `NOTRUN` stays `NOTRUN` (it does not come back as `CLEAN`). `HYPOTHESIS` stays `HYPOTHESIS` (it does not come back as `PROVED`). `PROVED` stays `PROVED`. A stage status `CLEAN` stays `CLEAN` beside a `NOTRUN` finding.

`RunReport::load` still replaces an ill-formed UTF-8 byte with U+FFFD and returns the report. That replacement was left in place. `models.cpp` was not changed.

## Tests

`tests/cpp/test_finding_roundtrip.cpp` (added to `_prism_test_sources` in `CMakeLists.txt`; the test target lists sources and does not glob):

- one stage (`CLEAN`) and four findings (`BOUNDED`, `NOTRUN`, `HYPOTHESIS`, `PROVED`) written with `RunReport::dumps` and loaded back; each status is the word that was stored
- a `0xFF` byte inside a message still loads (`not nullopt`); that finding's status is unchanged; the byte is stored as U+FFFD

`prism_tests` was not executed in this worktree: this slice does not configure or build.
