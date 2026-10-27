# stdio buffer checks

Branch `finish/stdio-buffer`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.

## fgets

`fgets` in `src/prism/pir/models/libc/stdio.cpp` returns 0 when `n <= 0` and does not write. When `n > 0`, a null `buf` is `__prism_check` with class `STR-NULL-ARG` and message `fgets() called with unchecked pointer parameter buf`. That check is before `__prism_havoc_bytes` and the NUL store. A null buffer then returns 0. It does not return a made-up string.

## fread

`size * nmemb` overflow is `__prism_check` with class `MEM-OOB-WRITE` and message `fread size * nmemb overflows`. That arm does not `return 0`. A 0 return is a short read, not an overflow. `__prism_assume(0)` ends the arm, so the following return is not a completed read. A product of 0 returns 0 and does not call `__prism_havoc_bytes`. A positive byte count checks a null destination (`STR-NULL-ARG`, message names `fread`) before havoc, and does not write through it. A short read may still return 0.

## fwrite

The overflow check message is still `fwrite size * nmemb overflows`. `__prism_read_range` already rejects a null source when the byte count is not 0: `read_range` calls `access_checks` with the guard `n != 0`, and that check is class `PTR-NULL-DEREF`. No second check was added in front of it. The return is still `k` with `k <= nmemb`, so a short write is not reported as a full count.

`fopen` messages are unchanged and still contain `fopen`. `fputs` and `puts` messages are unchanged.

## Tests

`tests/cpp/test_stdio_buffer.cpp` reads `stdio.cpp` from `PRISM_SOURCE_DIR`. It checks that the `fgets` check names `fgets`, that the `fread` overflow arm is a `__prism_check` and does not contain `return 0`, and that each `fopen` check message contains `fopen`. One line was added to `_prism_test_sources`. `prism_tests` was not executed. This slice does not configure or build.
