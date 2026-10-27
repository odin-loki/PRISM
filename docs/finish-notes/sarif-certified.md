# SARIF and PROVED-CERTIFIED

`to_sarif` in `src/prism/sarif.cpp` leaves `PROVED-CERTIFIED` out of `runs[0].results`. A certificate-checked proof has nothing to fix. `level_of` returns a level only for `FAILED`, `CRASH`, `SANFAIL`, and `HYPOTHESIS`, so proof statuses are omitted. `PROVED` is omitted on that same path, and `BOUNDED` is omitted with it, so a bounded result is not exported as a proof result (Law 2).

The run records the count of `PROVED-CERTIFIED` findings in `runs[0].properties.certified`.

`FAILED` stays a result. With default severity and strength `FINDS` its level is `error`.

A missing tool stays visible as a tool execution notification. A stage whose status is `NOTRUN` is appended to `invocations[0].toolExecutionNotifications` at level `warning`. That is the existing choice; finding-level `NOTRUN` is not a result. A stage status `failed` stays an error notification and sets `executionSuccessful` false. `src/prism/sarif.cpp` was not edited.

## Tests

`tests/cpp/test_sarif_certified.cpp` (one new line in `_prism_test_sources`; the test target lists sources and does not glob). `tests/cpp/test_sarif.cpp` was not edited.

The case builds a report with one `PROVED-CERTIFIED` finding and one `FAILED` finding. The SARIF JSON has the `FAILED` result at level `error`. No result message names the certified status or the proof text, and no result level treats that proof as a defect (`error` or `warning`). When `runs[0].properties.certified` is present it is `1`.

`prism_tests` was not executed in this slice.
