# feof and ferror

Branch `finish/libc-stdio-eof`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`feof` and `ferror` are new functions in `src/prism/pir/models/libc/stdio.cpp`, after `fflush` and still inside `extern "C"`. Both return `prism_file_flag`. That helper calls the existing `prism_file_ok` check. When `f` is null after that check, the path is `__prism_assume(0)` and `return 0`. It does not return 1. A non-null `FILE` returns `__VERIFIER_nondet_int() ? 1 : 0`. The model does not read a user buffer.

The comment above the helper says this is not a proof that the stream is at end of file and not a proof that the stream has an error, and that feof and ferror are not PROVED and not CLEAN.

`fread`, `fwrite`, `fgets`, `fclose`, `fputs`, and `puts` are unchanged. The `fwrite` overflow arm is still `__prism_check` with class `MEM-OOB-READ` and message `fwrite size * nmemb overflows`, then `__prism_assume(0)`, then `return 0`. The `fread` overflow arm is unchanged.

## Tests

`tests/cpp/test_stdio_buffer.cpp` reads `stdio.cpp` from `PRISM_SOURCE_DIR`. It locks the comment and the null path: `prism_file_ok`, then `if (f == 0)` with `__prism_assume(0)` and `return 0`, and not `return 1`. The path after that arm is `return __VERIFIER_nondet_int() ? 1 : 0`. That path does not call `__prism_havoc_bytes` or `__prism_read_range`. `feof` and `ferror` return the helper and do not return 1.

`testdata/conformance/libc-models/stdio_contracts.cpp` calls `feof` and `ferror` on the open stream inside `fopen_true` and discards the values, so the libc harness scan sees the names. No yml task was added. `stdio_contracts.yml` does not claim `feof` or `ferror` is proved. `tests/cpp/test_qa_conformance.cpp` was not edited.

`prism_tests` was not executed. This slice does not configure or build.
