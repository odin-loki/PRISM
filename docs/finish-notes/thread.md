# Thread stage doctest

`tests/cpp/test_thread.cpp` locks `prism::run_thread` (`src/prism/stages/thread.cpp`). The stage source was not changed. `tests/cpp/test_thread_taint.cpp` already covers the ISO `thrd_*` plants, `mtx_lock` / `mtx_timedlock`, `jthread_race.cpp`, the `race_global.c` writer names, and the taint stage; those cases are not repeated.

## What the tests lock

- The only defect class is `RACE-SHARED`: status `FAILED`, strength `FINDS`, `evidence` and `extra["global"]` hold the name, `extra["writers"]` is the unsynchronised writers in source order, and the finding is anchored on the first of those writers. Not `CONC-ATOMICITY` or `CONC-DATA-RACE` (those belong to other stages).
- Planted positive: `testdata/planted/race_global.c` (`pthread_create`, global `g`, writers `t1` and `t2`), both from `run_thread` and from `run_pipeline` with stages `inventory`, `classify`, `thread`. The pipeline row stays `ok` (the finding is `FAILED`, so the stage is not `NOTRUN`).
- `CreateThread` with `static` / `extern` globals, compound assignment, and a subscript write. `std::thread` with two writers.
- No thread API, including two functions that write the same global (`abs_ok.c` and a synthetic file): no findings. The pipeline stage is `ok` with `records == 0`, not `NOTRUN`, `skipped`, or `failed`.
- A thread API only in a comment or a file-scope string is ignored.
- A local, and a pure read of a global, are not a file-scope race.
- One of two writers calling `mtx_lock` is not a race: the stage needs two unsynchronised writers.
- `std_thread.cpp`, `sthread_unenc.cpp`, and `jthread.cpp` start a C++ thread and write no shared global. This stage is silent. It does not report `NEEDS-HARNESS` (that status is the conc stage's, for threads it cannot encode).
- A threaded file whose source cannot be opened is one `NOTRUN` row, strength `READS`, empty class, naming the missing file. A second file with no thread API stays silent even when its path is also missing. `NOTRUN` is not `NEEDS-HARNESS`, `CLEAN`, or `FAILED`.

No model and no network. The pipeline cases set `llm` off and run only `inventory`, `classify`, and `thread`.

## CMake

`tests/cpp/*.cpp` is not globbed. `CMakeLists.txt` gained one line, `tests/cpp/test_thread.cpp`, in `_prism_test_sources` (the list shared by every platform).

## Left as the stage implements them

These are gaps in the checker, not statuses the new tests require:

- Two threads that run the same function (`tests/conc/racy_counter.c`, `testdata/conformance/concurrency/counter_race_false.c`) are not `RACE-SHARED`. The stage counts functions, not creates.
- The mutex regex is `\bpthread_mutex\b`, so `pthread_mutex_lock` does not match and does not suppress a race. `mtx_lock` and `mtx_timedlock` do; `test_thread_taint.cpp` locks those.
- A pointer global (`int *p`) is not taken as a file-scope name.
- `g++` / `++g` is not treated as a write.
