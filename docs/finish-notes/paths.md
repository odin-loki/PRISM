# Corpus path leftovers

The planted corpus lives under `testdata/planted/`. One test file still names
two planted specs by their pre-move paths. The lookups are right; only the
words are stale.

## `tests/cpp/test_ltl.cpp`

Lookups already use `td_root()` (`testdata/planted`).

| Line | Text | Replacement |
|---|---|---|
| 437 | comment `testdata/gf_recur.ltl` | `testdata/planted/gf_recur.ltl` |
| 571 | TEST_CASE title `testdata/comments_only.ltl` | `testdata/planted/comments_only.ltl` |
