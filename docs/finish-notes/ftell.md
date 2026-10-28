# ftell

Branch `finish/libc-ftell`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`ftell` is a new function in `src/prism/pir/models/libc/stdio.cpp`, after `ferror` and still inside `extern "C"`. It calls the existing `prism_file_ok`. When `f` is null after that check, the path is `__prism_assume(0)` and `return -1`. It does not return 0. A non-null `FILE` returns `__VERIFIER_nondet_long()`. That unknown long is not a proof of the file position. The model does not read a user buffer.

The comment above `ftell` says this is not a proof of the file position, and that ftell is not PROVED and not CLEAN.

`fread` and `fwrite` are unchanged. The `fwrite` overflow arm is still `__prism_check` with class `MEM-OOB-READ` and message `fwrite size * nmemb overflows`, then `__prism_assume(0)`, then `return 0`. `feof`, `ferror`, `fgetc`, `fopen`, and `fclose` are unchanged. `fseek`, `rewind`, `snprintf`, `sprintf`, and `printf` were not added.

## Tests

`tests/cpp/test_stdio_buffer.cpp` reads `stdio.cpp` from `PRISM_SOURCE_DIR`. It locks the comment and the null path: `prism_file_ok`, then `if (f == 0)` with `__prism_assume(0)` and `return -1`, and not `return 0`. The path after that arm is `return __VERIFIER_nondet_long()`. That path does not call `__prism_havoc_bytes` or `__prism_read_range`. The body does not call `fseek`, `rewind`, `snprintf`, `sprintf`, or `printf`.

`testdata/conformance/libc-models/stdio_contracts.cpp` calls `ftell` on the open stream inside `fopen_true` and discards the value, so the libc harness scan sees the name. No yml task was added. `stdio_contracts.yml` does not expect a specific file position. `tests/cpp/test_qa_conformance.cpp` was not edited. `CMakeLists.txt` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
