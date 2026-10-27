# Weak atomics and escaping throws

Slice `finish/conc-relaxed`. Relaxed, acquire, and release atomics are not
sequentially consistent, so the conc stage must not report them `PROVED` or
`BOUNDED`. An exception that leaves a thread must not disappear into a clean
result either.

## How a refusal becomes a finding

`Unenc` in `src/prism/conc/extract.cpp` is caught by `build_program`. The
build starts as `NEEDS-HARNESS` and that status is cleared only when a
program is produced. The reason (`UNENCODED: ...`) is copied onto the harness
result in `analyse_ir` (`src/prism/conc/stage.cpp`).

`Fail` in `src/prism/conc/lazy.cpp` is caught by `check`. Its status and
reason become the `Result`. `run_conc` writes one finding for that result.
A `FAILED` result is expanded into one finding per violation instead.

## Behaviors

1. `memory_order_relaxed` (`monotonic` or `unordered`), `memory_order_acquire`
   (and consume), `memory_order_release`, and `memory_order_acq_rel` are
   `NEEDS-HARNESS`. The message starts with `UNENCODED:` and names the order.
   `seq_cst` is unchanged. Weak orders are not encoded.
2. The same refusal applies when the source names those orders but lowering
   has already dropped them (the Windows LP64 `<stdatomic.h>` shim stores and
   loads as plain integers). A clean result, or a result that is only a data
   race, becomes `NEEDS-HARNESS`. A deadlock or a failed assertion is kept.
3. A throw that leaves a thread with no `CXX-UNCAUGHT` / `throw-unmodelled`
   check was a `Stop` block. `check` treated that as a quiet exit and could
   return `BOUNDED`. It is now `NEEDS-HARNESS` with
   `UNENCODED: throw leaves ...`. A `throw-unmodelled` check is still
   `NEEDS-HARNESS`. An `uncaught` check is still `FAILED`.

## Tests

`tests/cpp/test_conc.cpp`

- `conc: relaxed atomics are NEEDS-HARNESS (SC only)` — `tests/conc/relaxed.c`
  through `check_file` and `run_conc`.
- `conc: relaxed, acquire and release atomics are NEEDS-HARNESS` — direct
  `build_program` on LLVM IR (`monotonic`, `unordered`, `acquire`, `release`,
  `acq_rel`, a release fence, a `seq_cst`/`monotonic` cmpxchg). A `seq_cst`
  store is not refused for its order.
- `conc: an escaping throw is not a clean or proved result` — direct `check`
  of a thread whose `throws` list is set and that has no exception check, a
  `throw-unmodelled` check, and an `uncaught` check.

Ran off this tree (MSVC Release, `-DPRISM_Z3=OFF`,
`-DPRISM_TESTS_WIN_SUBSET=ON`): `prism_tests --test-case=conc:*` — 13 cases
passed, 52 assertions. The direct `build_program` cases and the escaping-throw
case (no solver) ran. `tests/conc/relaxed.c` did not: `have_frontend()` is
false without Z3, and `run_conc` stays `NOTRUN` until Z3 is built. The
`throw-unmodelled` and `uncaught` checks sit behind `z3_available()` and did
not run.
