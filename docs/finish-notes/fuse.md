# fuse counterexamples and AFL

Closed the `prism/fuzz.py` / `prism/afl.py` gaps named for this slice. Performance caches (the harness build cache, the parallel binary window, the syntax memo) were already in `src/prism/stages/fuse.cpp` and were left as they are. `decode_args` stays in `common.cpp` and was not edited. `platform.cpp` was not edited: `run_argv` still has no working directory, and its default is still the process cwd.

## Counterexample bytes

`bytes_from_cex` does not call `std::stoi`. `cex_value` parses a decimal, `0x` / `#x` hex, or `0b` / `#b` numeral with `std::from_chars` into a `uint64_t` (a leading minus wraps in unsigned). A value that does not fit in 64 bits, a leading-zero decimal, or trailing junk is `nullopt`, not an exception. Each parameter is then stored little-endian at its `kCTypeSize` width, so the harness `memcpy` sees the low bytes. `x=4294967295` and `x=0xFFFFFFFF` on an `unsigned` or `int` parameter are the four bytes `ff ff ff ff`. On `int`, `decode_args` reads that pattern as `-1`.

## AFL child environment

`afl_env()` adds `AFL_NO_UI`, `AFL_SKIP_CPUFREQ`, and `AFL_NO_AFFINITY` only when the variable is unset or empty. A value already in the environment is left alone. That list is `RunSpec::env`. `detail::run` copies the process environment, applies the overlay, and passes the copy to the child. It does not call `setenv` / `_putenv_s` on PRISM. The parent's `getenv` is unchanged after the run.

## AFL working directory

`run_afl_fuzz` sets `RunSpec::cwd` to the work directory (the caller's `work` argument, or the temporary directory when `work` is empty) and starts afl-fuzz through `run_spec`. An empty `cwd` on any other `run_argv` caller still means no `chdir`.

## Sandbox on AFL rows

`run_afl_fuzz` sets `extra["engine"]` to `afl` and `extra["sandbox"]` to `sandbox::kind()` on every row that is not `NOTRUN`, including `CLEAN`, `CRASH`, and `ERROR`. `NOTRUN` (no `--allow-exec`, or no C compiler) does not claim the engine. `sandbox::kind()` is the existing helper (`bwrap`, `rlimits-only`, or `none`).

A `CRASH` or `ERROR` from AFL is returned as that finding. A quiet AFL run is folded into the FuSeBMC `CLEAN` row; that row now copies `extra["sandbox"]` from the AFL finding, so it still names the sandbox when the binary oracle did not.

## Tests

`tests/cpp/test_fuse.cpp` (already listed in `CMakeLists.txt`):

- `bytes_from_cex keeps values above INT_MAX` — no solver. `4294967295`, `0xFFFFFFFF`, and `#xffffffff` pack to `ff ff ff ff`; the `int` pattern decodes as `-1`.
- `afl: compile failure is ERROR with engine and sandbox` — needs a C compiler on `PATH`, skipped with a message otherwise. A syntax error is `ERROR` with `engine=afl` and `sandbox` equal to `sandbox::kind()`, and the message says compile failed. afl-fuzz is not started.

Already in the suite, not edited here:

- `tests/cpp/test_concrete.cpp` — the wider `bytes_from_cex` case (named order, negatives, `long long`).
- `tests/cpp/test_adapters.cpp` — a planted crash is `CRASH` and a quiet run is `CLEAN`, both with `engine=afl` and `sandbox::kind()`; the child sees `AFL_NO_UI` `AFL_SKIP_CPUFREQ` `AFL_NO_AFFINITY` as `1 1 1` (or the user's `AFL_NO_UI`) and its cwd is the work directory; the parent environment is unchanged.

`prism_tests` was not executed in this slice: no existing binary, and no ext4 `~/PRISM` build tree.
