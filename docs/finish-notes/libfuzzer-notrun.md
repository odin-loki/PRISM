# libFuzzer launch that never starts

Slice `finish/libfuzzer-notrun`. `run_libfuzzer` in `src/prism/stages/fuse.cpp` stored the `ProcRun` from `run_argv` (`-max_total_time`) in `r`, then ignored `r.failed`. When the libFuzzer binary never started, the crash directory stayed empty and the function returned `CLEAN` (`no libFuzzer crash in Ns (not a proof)`). Law 1: a missing tool is `NOTRUN`, never a clean result. Law 3: fuzzer `CLEAN` is not a proof even when the fuzzer did run.

## Status

`libfuzzer_launch_status` maps that `ProcRun` and whether a crash file was left. An AddressSanitizer or UndefinedBehaviorSanitizer report is read from the run log (`r.err` + `r.out`). `run_libfuzzer` still collects `crash-*` files the same way and returns the helper's finding. A started run also keeps `extra["exe"]` (the clang path) and `extra["sandbox"]`.

| launch | status |
|---|---|
| `ProcRun::failed` (never started) | `NOTRUN` on stage `libfuzzer`. Message is `run.err`, or `failed to start` when `err` is empty. `extra["install"]` is `clang libFuzzer did not start`. Not `CLEAN`, not `PROVED`. `engine` is not set. A crash file or sanitizer text does not override this. |
| started, no crash file, no sanitizer report (`failed == false`), including a timeout and a non-zero exit (including 127) | `CLEAN`. Message is `no libFuzzer crash in Ns (not a proof)`. `laws::is_proof` is false. |
| started, and a crash file was left | `CRASH`, class `LIBFUZZER-CRASH`. Message is `libFuzzer crash on` plus the first 16 bytes as hex. The counterexample is the whole file. `extra["libfuzzer_crashes"]` is the file count. `engine` is `libfuzzer`. |
| started, no crash file, ASan or UBSan text in the log | `CRASH`, class `LIBFUZZER-CRASH`. Message is `libFuzzer sanitizer crash`. |

Only `ProcRun::failed` means unstarted. A timeout, or exit code 127 from a child that did start, stays `CLEAN` when there is no crash file and no sanitizer report. AFL++ (`afl_launch_status`, `run_afl_fuzz`) and `platform.cpp` are unchanged.

## Tests

`tests/cpp/test_libfuzzer_notrun.cpp` calls `libfuzzer_launch_status` only. It does not spawn libFuzzer.

- An unstarted launch (`failed`, with `err` and without) is `NOTRUN`, not `CLEAN` or `PROVED`, and carries the install hint. A leftover crash-file sample or an AddressSanitizer line in `err` stays `NOTRUN`.
- A started exit, a timeout, a non-zero exit, and exit 127 with no crash file and no sanitizer report are `CLEAN`. The message contains `not a proof`, and `is_proof` is false.
- A started run with a crash-file sample (17 bytes, two files) is `CRASH` (`LIBFUZZER-CRASH`): the message keeps the first 16 bytes, the counterexample keeps the whole file.
- A started run whose log contains AddressSanitizer, or UndefinedBehaviorSanitizer with exit 127, is `CRASH`.

`prism_tests` was not executed. This checkout stays off the OneDrive build path.
