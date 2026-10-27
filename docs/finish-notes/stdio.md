# stdio fopen failure

Branch `finish/stdio-fopen`, base `136f174bb2087d61c797b968631e37284eef8f87`.

`fopen` in `src/prism/pir/models/libc/stdio.cpp` returns a null `FILE` when the open fails. That path is the nondeterministic branch that returns `0` before `__prism_alloc`. The success path is the only place a `FILE` object is allocated (`PRISM_FILEK`, `PRISM_FILE_SIZE`), so a failed open leaves no usable `FILE`.

A later `fread` or `fwrite` of that null pointer calls `prism_file_ok`. Its existing check is `__prism_check(f != 0, "PTR-NULL-DEREF", "stdio call on a NULL FILE pointer")`. `fclose` uses its own existing check, `__prism_check(f != 0, "PTR-NULL-DEREF", "fclose of a NULL FILE pointer")`. Both stay `PTR-NULL-DEREF`. `__prism_havoc_bytes` on the `fread` success path is the same call as before.

## Tests

`stdio.cpp` is embedded as source (`PRISM_PIR_MODELS` in `CMakeLists.txt`) and lowered by clang when the pir stage runs. `__prism_check`, `__prism_alloc`, and `__VERIFIER_nondet_int` are PIR intrinsics declared in `prism_model.h` and are not defined as C functions. `tests/cpp` therefore cannot call this `fopen`. No doctest was added, and this note did not execute the model.
