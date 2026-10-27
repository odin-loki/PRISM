# ltl NOTRUN rows use READS

Slice `finish/ltl-notrun-strength`. `run_ltl` in `src/prism/stages/ltl.cpp` built its `NOTRUN` findings with `laws::STRENGTH_PROVES`. A row that did not run uses `laws::STRENGTH_READS`. Status stays `NOTRUN`. `laws::is_proof` is false for that status.

## Rows

Two findings in `run_ltl` set `strength` to `laws::STRENGTH_READS`:

- The spec list yields no formula (no `.ltl` file, or only `#` / `//` lines). Message stays `no .ltl spec next to the sources`. `extra["install"]` stays `add a file with G (...)`.
- A formula the stage does not decide. That is a formula outside the safety fragment, a safety formula with no extracted `switch (state)` machine, or a formula whose predicates cannot be evaluated on the machine. The message is unchanged. `extra["formula"]`, `extra["install"]` (`https://github.com/meyerphi/strix`), `extra["strix"]`, `extra["strix_not_proved"]` (`true`), and `extra["strix_note"]` stay as they were. Strix is still only recorded. Its output is still not a verdict.

## Unchanged verdicts

`ltl_finding` still defaults to `laws::STRENGTH_PROVES`. A `G` formula that holds on an extracted switch FSM stays `PROVED` with that strength. An unbounded `F` approximation still goes through `approx_finding`, which rewrites a holding check from `PROVED` to `BOUNDED`. `tests/cpp/test_ltl.cpp` already locks those verdicts and was not edited.

## Tests

`tests/cpp/test_ltl_notrun_strength.cpp` is one new line in `CMakeLists.txt` `_prism_test_sources` (the test target lists sources and does not glob).

- `run_ltl` with an empty spec list: one finding, status `NOTRUN`, strength `READS`, `is_proof` false, message contains `no .ltl spec`.
- Bare `F (state == ST_IDLE)` beside `testdata/planted/fsm.c` function `fsm_step` (the plant `tests/cpp/test_ltl.cpp` uses): status `NOTRUN`, strength `READS`, status is not `PROVED` and not `BOUNDED`, `strix_not_proved` stays `true`.
- On that same machine, `G (state == ST_IDLE -> X (state == ST_IDLE || state == ST_WORK))` stays `PROVED` with strength `PROVES`. `G (state == ST_IDLE -> F (state == ST_IDLE))` stays `BOUNDED` (`approx_kind` `F`).

`prism_tests` was not executed. This checkout stays off the OneDrive build path.
