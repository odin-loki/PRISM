# stdio fseek, rewind, and clearerr

`fseek`, `rewind`, and `clearerr` live in `src/prism/pir/models/libc/stdio.cpp`
next to `ftell`. A null `FILE` is infeasible after the null check
(`__prism_assume(0)`). A live `FILE` is not a proof that the position or
the error and eof flags changed.

`fseek` returns `-1` or `0`. It does not store `off` or `whence`.
`rewind` and `clearerr` return without writing a position of 0 or a cleared
flag. None of the three is `PROVED` or `CLEAN`.
