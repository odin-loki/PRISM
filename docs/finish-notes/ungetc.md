# ungetc

Branch `finish/libc-ungetc`, base `436916a8bd85fbc67b935e68ae9254259c2853d1`.

`ungetc` is a new function in `src/prism/pir/models/libc/stdio.cpp`, after `clearerr` and still inside `extern "C"`. The file defines `EOF` as `-1` when it is not already defined. A null `FILE` is `__prism_check` with class `PTR-NULL-DEREF` and message `ungetc null FILE`, then `if (f == 0)` and `__builtin_trap`. That trap is before any store. The arm does not return the character. `EOF` as the character returns `EOF` and does not store a character. Any other call returns `prism_eof_or((unsigned char)c)`. The model does not store a pushback buffer and does not make a later read return that character. `fgetc` is still a nondeterministic byte in `-1..255`.

The comment above `ungetc` says this is not a proof a character was pushed back, that a live `FILE` does not store a pushback buffer, that a later read is not that character, and that ungetc is not PROVED and not CLEAN.

`fflush`, `feof`, `ferror`, `fputc`, `putc`, `putchar`, `fread`, `fwrite`, `fseek`, `ftell`, `rewind`, and `clearerr` are unchanged. `fflush(NULL)` stays a real call: `if (f) prism_file_ok(f);` then `return prism_eof_or(0);`. The `fwrite` overflow arm is still `__prism_check` with class `MEM-OOB-READ` and message `fwrite size * nmemb overflows`, then `__prism_assume(0)`, then `return 0`. `snprintf`, `sprintf`, and `printf` were not added.

## Tests

`tests/cpp/test_stdio_buffer.cpp` reads `stdio.cpp` from `PRISM_SOURCE_DIR`. It locks the comment and the null path: `__prism_check` with `PTR-NULL-DEREF` and message `ungetc null FILE`, then `if (f == 0)` with `__builtin_trap` and no return. That arm does not name the character and does not call `prism_eof_or`. The text before the trap does not return and does not assign. The `EOF` arm is `return EOF;` and does not store, does not convert to `unsigned char`, and does not call `prism_eof_or`. The path after that arm is `return prism_eof_or((unsigned char)c);`. The body does not assign, does not use a subscript, and does not call `fgetc`, `fread`, `fwrite`, `fflush`, `fputc`, `putc`, `putchar`, `fseek`, `ftell`, `rewind`, `clearerr`, `feof`, `ferror`, `prism_file_ok`, `__prism_havoc_bytes`, `__prism_read_range`, or `__prism_memset`. The `clearerr` case now ends at `int ungetc(`, so the `clearerr` body still does not call `ungetc`. `fgetc` still returns the nondeterministic byte. `fflush` still has `if (f) prism_file_ok(f);` and `return prism_eof_or(0);`.

`testdata/conformance/libc-models/stdio_contracts.cpp` calls `ungetc` on the open stream inside `fopen_true` and discards the value, so the libc harness scan sees the name. No yml task was added. `stdio_contracts.yml` does not expect a character to have been pushed back. `fflush((FILE *)0)` is still in that harness. `tests/cpp/test_qa_conformance.cpp` was not edited. `CMakeLists.txt` was not edited. No new `tests/cpp` file was added.

`prism_tests` was not executed. This slice does not configure or build.
