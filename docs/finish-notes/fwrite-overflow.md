# fwrite size overflow

Branch `finish/libc-fwrite-overflow`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`fwrite` in `src/prism/pir/models/libc/stdio.cpp` still reports `size * nmemb` overflow as `__prism_check` with class `MEM-OOB-READ` and message `fwrite size * nmemb overflows`. That arm no longer falls through. It calls `__prism_assume(0)` and `return 0`. The arm does not call `__prism_read_range` on `p` and does not return a count of elements written. `__prism_assume(0)` makes the path infeasible, so the `return 0` is not a successful write.

A product of 0 (`size` or `nmemb` is 0) does not overflow. That path is unchanged: `__prism_read_range(p, t)` with `t == 0`, then `k` with `k <= nmemb`. A short write is still a returned count, not the overflow arm.

`fread`, `fgets`, `fclose`, `fputs`, and `puts` are unchanged. `string.cpp` and `stdlib.cpp` are unchanged.

## Tests

`tests/cpp/test_stdio_buffer.cpp` reads `stdio.cpp` from `PRISM_SOURCE_DIR`. The fwrite case checks that the overflow arm is `__prism_check` with `MEM-OOB-READ` and message `fwrite size * nmemb overflows`, that the arm is `__prism_assume(0)` and `return 0`, and that the arm does not call `__prism_read_range` or `return k`. The condition of that arm is the multiply overflow, not `size == 0` or `nmemb == 0`. The code after the arm still reads `p` and returns `k`.

`prism_tests` was not executed. This slice does not configure or build.
