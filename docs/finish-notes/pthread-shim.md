# pthread.h shim

Slice `finish/pthread-shim`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.

`src/prism/pir/models/libc/shim/pthread.h` is the Windows LP64 `<pthread.h>` header used when PIR lowers `tests/conc`. It declares `pthread_create`, `pthread_join`, and the `pthread_mutex_*` operations. The comment above the include guard states that these are declarations only, not a concurrency proof, and that a missing body stays unmodeled rather than success.

No function body was added. A body that returns 0 would be a fake successful lock and could be read as a proof. Each declaration still ends in a semicolon. `pthread_mutex_t`, `pthread_cond_t`, and `PTHREAD_MUTEX_INITIALIZER` still use braces. Those braces are the type and the initializer, not a function body.

`stdatomic.h`, `assert.h`, `string.h`, and `src/prism/conc/` are unchanged.

## Tests

`tests/cpp/test_pthread_shim.cpp` reads the header as text from `PRISM_SOURCE_DIR`. It does not include the header, so the shim is not compiled as a C++ translation unit.

The test fails when:

- a line returns a value
- `pthread_mutex_lock` (or any other function in this header) is defined with a brace body
- `pthread_mutex_lock`, `pthread_mutex_unlock`, or `pthread_create` has no declaration ending in a semicolon
- the header text contains `PROVED` or `CLEAN`

`CMakeLists.txt` gained one line in `_prism_test_sources`: `tests/cpp/test_pthread_shim.cpp`. The test target lists sources and does not glob.

`prism_tests` was not executed. This slice does not configure or build.
