# Fuzzer CLEAN is not a proof

A fuzzer that finds no crash returns `laws::CLEAN`. The message contains `not a proof`. `laws::is_proof` is false for that status: `verdict::is_proof` (`src/prism/verdict/verdict.cpp`) is true only for `PROVED-CERTIFIED`, `PROVED-UNBOUNDED`, `PROVED`, and `PROVED-ASSUMING`, and `laws::is_proof` forwards to it.

`src/prism/stages/fuse.cpp` already uses `laws::CLEAN` on every no-crash path below, and each message contains `not a proof`. The file was left unchanged.

## AFL — `run_afl_fuzz`

`prism::stages_detail::run_afl_fuzz` builds the row with the local lambda `afl_base`. The quiet campaign is:

```cpp
return afl_base(laws::CLEAN, "", "no AFL crash in " + std::to_string(vsec) + "s (not a proof)");
```

`afl_base` is declared inside `run_afl_fuzz` and has no other caller. That return runs after `run_spec` starts afl-fuzz. With no afl-fuzz binary, or when the function is not `SCALAR`, `run_afl_fuzz` returns `nullopt` and does not build this finding. Without `--allow-exec` the same function returns `NOTRUN`.

## libFuzzer — `run_libfuzzer`

`run_libfuzzer` lives in an anonymous namespace in `fuse.cpp`. The quiet run is the local lambda `lf_base`:

```cpp
auto f = lf_base(laws::CLEAN, "", "no libFuzzer crash in " + std::to_string(vsec) + "s (not a proof)");
```

`lf_base` and `run_libfuzzer` are not declared in a header. The `CLEAN` return is after the libFuzzer binary is compiled and run.

## FuSeBMC row — `fuse_one`

`fuse_one` (same anonymous namespace) closes the loop with:

```cpp
auto f = make_find("fuse", laws::CLEAN, fn, "",
                   "no crash in FuSeBMC loop (" + extra["rounds"] + " rounds; not a proof)",
                   laws::STRENGTH_FINDS);
```

That statement is the end of the loop. The loop calls `fuzz_function`, and when `PRISM_AFL=1` and afl-fuzz is present it also calls `run_afl_fuzz`.

## Concrete oracle — `fuzz_function`

The same file's `no_crash` lambda inside `fuzz_function` is the concrete no-crash row:

```cpp
auto f = make_find("fuzz", laws::CLEAN, fn, "",
                   "no crash in " + std::to_string(i) + " iters / " + std::format("{:.1f}", budget) +
                       "s (not a proof)",
                   laws::STRENGTH_FINDS);
```

`no_crash` is local to `fuzz_function`. Calling `fuzz_function` runs the concrete oracle.

## No new test

None of `afl_base`, `lf_base`, `fuse_one`, or `no_crash` can be called on its own. Reaching the `CLEAN` message means entering an entry that runs a fuzzer (`run_afl_fuzz` starts afl-fuzz before the quiet return). This slice does not add `tests/cpp/test_fuzz_not_proof.cpp` and does not start a fuzzer.
