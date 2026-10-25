# Test data layout

All checker corpora live under `testdata/`. Doctest and `prism-qa` resolve paths
from `PRISM_SOURCE_DIR` (the repository root at configure time).

| Directory | Purpose |
|-----------|---------|
| `planted/` | Planted-bug C/C++ sources for lints, BMC, fuzz, and most engine doctests (`tests/cpp/test_lint_corpus.cpp`, `test_main.cpp`, …). One file per defect shape; basenames are stable. |
| `tp/` | Buggy twins of patterns that must **fire** a lint (`testdata/tp/` was `testdata_tp/`). |
| `fp/` | Correct twins that must stay **silent** (`testdata_fp/`). |
| `pir/` | Table-driven PIR stage corpus (`test_pir_e2e.cpp`, Lean `pir-lean-check`, …). |
| `conformance/` | Scored true/false tasks for `prism-qa conformance` (prism, sv-comp, concurrency, esbmc-cpp, libc-models). See `conformance/SOURCES.md`. |
| `fixtures/` | Non-source artefacts: `verdict_tables.json`, ask-measure JSONL, `selfscan_min/`, etc. |

Adding a planted lint check: put the new source in `planted/`, add a row to
`tests/cpp/lint_corpus_rows.inc`, and add `fp/` / `tp/` twins when the rule
needs negative control.
