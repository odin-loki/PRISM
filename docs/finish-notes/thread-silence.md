# Thread stage silence

Slice `finish/thread-silence`. `run_thread` (`src/prism/stages/thread.cpp`) was not changed. There is no new doctest.

## Give-up

The only give-up is a file that mentions a thread API and whose source cannot be opened. `run_thread` still returns one finding for that file. The finding is the value `nr` returns: status `NOTRUN`, strength `READS`, empty class, file set to the missing path. The message is `source of <file> not found: file-scope globals unknown, shared-write races not checked`. It is not `CLEAN`, not `PROVED`, and not `NEEDS-HARNESS`.

That row is pushed before `file_globals`. A later `continue` when the bodies hold no file-scope names does not drop it. `run_thread` returns the vector that contains it.

Already locked, so `tests/cpp/test_thread_silence.cpp` would duplicate them:

- `tests/cpp/test_thread.cpp` — `thread: a missing threaded source is NOTRUN, not NEEDS-HARNESS; a missing quiet source stays silent`
- `tests/cpp/test_thread_taint.cpp` — `thread: a threaded file whose source cannot be found is NOTRUN, not silent`

## Empty results that are not give-ups

An empty list is a finished check when there is nothing to report:

- The function list is empty.
- The file has no thread API (`pthread_create`, `std::thread`, `std::jthread`, `CreateThread`, `thrd_create`). A second file with no thread API stays silent even when its path is also missing.
- The source was read and `file_globals` finds no file-scope name, including a thread whose only `g` is a local.
- Fewer than two unsynchronised writers. One writer, a pure read, or a writer that mentions `mtx_lock` / `mtx_timedlock` / `pthread_mutex` is not `RACE-SHARED`.

`RACE-SHARED` stays a finding when it applies: status `FAILED`, strength `FINDS`. A thread with no shared write (`std_thread.cpp`, `sthread_unenc.cpp`, `jthread.cpp`) stays silent. This stage does not turn that into `NEEDS-HARNESS`.
