# SV-COMP `--data-model`

Slice `finish/svcomp-datamodel`. `prism svcomp --data-model` already accepts only `LP64` and `ILP32`. This slice leaves the parser in `src/prism/cli_svcomp.cpp` as it is and locks that check in `tests/cpp/test_svcomp_datamodel.cpp`.

## The check

`upper()` is the file-local function in the anonymous namespace of `src/prism/cli_svcomp.cpp`. It ASCII-uppercases the flag value. `svcomp_main` compares that result with equality:

```cpp
if (upper(*v) != "LP64" && upper(*v) != "ILP32") {
    usage();
    std::cerr << "prism svcomp: --data-model expects LP64 or ILP32\n";
    return 2;
}
opt.data_model = *v;
```

Accepted strings are `LP64` and `ILP32` in any case (`lp64`, `IlP32`, and the `--data-model=` form). The comparison is exact after `upper()`, so `ILP64`, `LP32`, `LLP64`, and an empty string (`--data-model=` or a following empty argument) are not accepted.

A rejected value returns 2 before `opt.data_model` is assigned and before `solve()`. It does not select ILP32. There is no `PRISM-SVCOMP-RESULT` line on that path.

When the flag is omitted, `svcomp_main` keeps `SolveOptions opt` and `SolveOptions::data_model` defaults to `"LP64"` (`include/prism/svcomp.hpp`).

`upper()` stays file-local. It is not exported.

## Left alone

`decide.cpp`, `witness.cpp`, `replay.cpp`, `stage.cpp`, `bmc.cpp`, and `abi_model.cpp` were not edited. `abi_from_data_model` / `is_ilp32` still treat only the string `ILP32` (any case) as ILP32, and every other string, including empty, as LP64. The CLI parser is the stricter gate: an unknown `--data-model` is an error, not a silent LP64 run.

## Tests

`tests/cpp/test_svcomp_datamodel.cpp` calls `prism::svcomp::svcomp_main` in process. It does not spawn `prism`.

- Omitted flag: `SolveOptions{}.data_model` is `LP64`, and `svcomp_main` with no arguments asks for `TASK` and `--prop` instead of rejecting a data model.
- `LP64` and `ILP32`, any case, spaced or joined with `=`, pass the check and then stop on the missing task. They do not print `PRISM-SVCOMP-RESULT`.
- `ILP64`, `LP32`, and an empty string, with `--prop` and a task present, exit 2 with `--data-model expects LP64 or ILP32` and do not print `PRISM-SVCOMP-RESULT`.

`prism_tests` was not executed in this slice.
