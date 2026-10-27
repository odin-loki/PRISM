# AFL launch that never starts

Slice `finish/afl-notrun`. `run_afl_fuzz` in `src/prism/stages/fuse.cpp` discarded the `ProcRun` from `run_spec`. When afl-fuzz could not start, the crash directories stayed empty and the function returned `CLEAN` (`no AFL crash in Ns (not a proof)`). Law 1: a missing tool is `NOTRUN`, never a clean result. Law 3: fuzzer `CLEAN` is not a proof even when the fuzzer did run.

## Status

`afl_launch_status` maps that `ProcRun` and whether a crash file was left. `run_afl_fuzz` keeps the `ProcRun`, scans `out/crashes` and `out/default/crashes` the same way (skip `README.txt`, first directory that has a file), and returns the helper's finding.

| launch | status |
|---|---|
| `ProcRun::failed` (never started) | `NOTRUN` on stage `fuse`. Message is `run.err`, or `failed to start` when `err` is empty. `extra["install"]` is `install afl-fuzz`. Not `CLEAN`, not `PROVED`, and not an empty list. `engine` is not set. |
| started, no crash file (`failed == false`), including a timeout and an exit of 127 | `CLEAN`. Message is `no AFL crash in Ns (not a proof)`. `laws::is_proof` is false. |
| started, and a crash file was left | `CRASH`, class `AFL-CRASH`. Message is `AFL crash on` plus the first 16 bytes as hex. The counterexample is the whole file. `extra["afl_crashes"]` is the file count. `engine` is `afl`. |

Only `ProcRun::failed` means unstarted. A timeout, or exit code 127 from a child that did start, stays `CLEAN` when there is no crash file. libFuzzer is unchanged. `platform.cpp` is unchanged; `notrun_if_unstarted` is the same idea for other tools.

## Tests

`tests/cpp/test_afl_notrun.cpp` calls `afl_launch_status` only. It does not spawn afl-fuzz.

- An unstarted launch (`failed`, with `err` and without) is `NOTRUN`, not `CLEAN` or `PROVED`, and carries the install hint.
- A started exit, a timeout, and exit 127 with no crash file are `CLEAN`. The message contains `not a proof`, and `is_proof` is false.
- A started run with a crash-file sample (17 bytes, two files) is `CRASH` (`AFL-CRASH`): the message keeps the first 16 bytes, the counterexample keeps the whole file.

`prism_tests` was not executed. This checkout stays off the OneDrive build path.
