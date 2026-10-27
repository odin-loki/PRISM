# triage statuses

Slice `finish/triage-status`. `src/prism/ai/triage.cpp` was read through. No assignment writes `Finding::status`.

## What triage writes

`triage` and `write_triage` take a `const RunReport`. `write_triage` creates `triage.json` and appends the `## Clusters` section of `report.md`. `triage_main` loads `report.json`, writes `triage.json`, and prints the same section. The loaded report stays in memory; `report.json` is left as it was.

`TriageCluster::top_status` is a copy of the representative finding's status string, used as the cluster label. The finding on the report is left as it was.

## Statuses on the report

A report that holds `PROVED`, `HYPOTHESIS`, `NOTRUN`, and `BOUNDED` still holds those four findings with those four statuses after `write_triage`. `HYPOTHESIS` stays `HYPOTHESIS`. `NOTRUN` stays `NOTRUN`. The finding list stays length 4. `RunReport::dumps()` is unchanged.

`triage_statuses()` clusters defect rows: `CRASH`, `FAILED`, `SANFAIL`, `ERROR`, `TIMEOUT`, `UNKNOWN`, `NEEDS-HARNESS`, `BOUNDED`, `HYPOTHESIS`. `PROVED` and `NOTRUN` stay on the report and are absent from the cluster list, so the Clusters section does not assign them another status. Each clustered row in this fixture is its own cluster (separate files, so no similarity join). The markdown and `triage.json` `top_status` for `bmc#1` is `HYPOTHESIS`. For `bmc#3` it is `BOUNDED`.

## Model

The public entry runs with `TriageOptions::use_embedder = false`. That is the deterministic TF-IDF core. It still builds the two clusters. A missing embedding model is an embedder note on that path when the embedder is requested; it is not a finding status, and this test does not replace the clusters with a `NOTRUN` row.

## Tests

`tests/cpp/test_triage_status.cpp` (added to `_prism_test_sources` in `CMakeLists.txt`; the test target lists sources and does not glob):

- `triage: PROVED, HYPOTHESIS, NOTRUN, and BOUNDED keep their statuses` — `write_triage` on the four statuses. The report bytes, the four statuses, and the finding count stay put. The Clusters section keeps `` `HYPOTHESIS` `` on `bmc#1` and `` `BOUNDED` `` on `bmc#3`, and has no `` `PROVED` ``, `` `NOTRUN` ``, or `` `CLEAN` `` status token.

`src/prism/ai/triage.cpp` is unchanged. `prism_tests` was not executed in this worktree: this slice does not configure or build.
