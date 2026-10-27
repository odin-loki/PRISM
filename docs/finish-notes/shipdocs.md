# shipped verdict definitions

`write_shipped_docs` in `src/prism/shipdocs.cpp` copies `docs/VERDICTS.md` and `docs/TRUSTED_BASE.md` next to `report.md`. Those files are the shipped strings (`kDoc_VERDICTS`, `kDoc_TRUSTED_BASE`).

## Sentences

The lattice rows already keep the laws:

- `PROVED` (`id="verdict-proved"`, rank 3, class proof) is "All properties hold, loops closed within k." `BOUNDED` (`id="verdict-bounded"`, rank 1, class "formal, not a proof") is "Nothing found within k. That is all it says."
- `CLEAN` is "The fuzzer found nothing. Not a proof (Law 3)."
- `HYPOTHESIS` is "LLM output. It cannot cover a defect class (Law 4)." `READS` is the other model verdict. The defect predicate is `FAILED`, `CRASH`, and `SANFAIL`.
- `NOTRUN` is "The tool is missing or the step was not allowed to run (Law 1, Law 9)." The merge table's `notrun-clean` row keeps it apart from `CLEAN`.

`docs/VERDICTS.md` and `docs/TRUSTED_BASE.md` stay as they were. `src/prism/pipeline.cpp` stays as it was.

## Tests

`tests/cpp/test_shipdocs.cpp` is listed in `_prism_test_sources` in `CMakeLists.txt` (the test target lists sources and does not glob). It calls `write_shipped_docs` into a temp directory and checks the written `VERDICTS.md`:

- anchors `verdict-proved` and `verdict-bounded` are both present, and the two rows differ
- the `BOUNDED` row does not name `PROVED`, and the `PROVED` row does not name `BOUNDED`
- the `CLEAN` row names the fuzzer and says it is not a proof

`prism_tests` was not run in this slice.
