# Corpus path leftovers

Files this slice must not edit. Paths below still name a pre-move location
or a file that is not where the text says. Replacements are the paths that
exist after the move under `testdata/`.

## `docs/PIR.md`

| Line | Text | Replacement |
|---|---|---|
| 762 | `testdata/conformance/test_conformance_suite.py` | `tests/cpp/test_qa_conformance.cpp` (`qa conformance: every libc model function is called by a harness`). Consolidation rewrote `tests/conformance/test_conformance_suite.py`; that file was not corpus data and is not in the tree. |
| 1051 | `conformance/prism/kindmem/kindmem_overwrite_true` | `testdata/conformance/prism/kindmem/kindmem_overwrite_true` |
| 1057 | `array/arr_loop_true` | `testdata/conformance/prism/array/arr_loop_true` |
| 1247 | `conformance/prism/virt` | `testdata/conformance/prism/virt` |
| 1248 | `conformance/prism/eh` | `testdata/conformance/prism/eh` |
| 1249 | `conformance/prism/coro` | `testdata/conformance/prism/coro` |
| 1251 | `conformance/prism/fp` | `testdata/conformance/prism/fp` |
| 1254 | `conformance/prism/asm` | `testdata/conformance/prism/asm` |
| 1255 | `conformance/prism/sjlj` | `testdata/conformance/prism/sjlj` |
| 1256 | `conformance/prism/kindmem` | `testdata/conformance/prism/kindmem` |
| 1504, 1507 | `testdata/interval_ops.c` | `testdata/planted/interval_ops.c` |
| 1508 | `testdata/trunc.c` | `testdata/planted/trunc.c` |

## `docs/SVCOMP.md`

| Line | Text | Replacement |
|---|---|---|
| 186 | `testdata/goto_structured.c` | `testdata/planted/goto_structured.c` |

## `tests/cpp/test_ltl.cpp`

Lookups already use `td_root()` (`testdata/planted`). Only the words are stale.

| Line | Text | Replacement |
|---|---|---|
| 410 | comment `testdata/gf_recur.ltl` | `testdata/planted/gf_recur.ltl` |
| 544 | TEST_CASE title `testdata/comments_only.ltl` | `testdata/planted/comments_only.ltl` |

## `docs/ENGINEERING_BACKLOG.md`

Line 11 names `tests/conformance` inside a changelog sentence for the fix that
already landed (`tests/conformance` to `testdata/conformance`). It is not a
live lookup.
