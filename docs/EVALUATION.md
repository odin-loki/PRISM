# PRISM on real code

This page says how PRISM is measured on real projects and what the last
measurement found.

## The benchmark: `prism-qa bench`

Branch port/bench (landing) adds `prism-qa bench`
(`src/tools/qa/bench.cpp`). It runs PRISM, cppcheck and clang-tidy on
twelve real C and C++ projects, each pinned by git commit in
`testdata/bench/projects.json`:

| project | shape |
|---|---|
| jsmn | header-only C tokenizer with two example programs |
| inih | small C parser plus a C++ wrapper |
| tinyexpr | recursive-descent expression evaluator, floating point |
| cJSON | JSON parser and printer over heap trees |
| stb_image | single-header image decoder, built as one C unit |
| zlib | compression library with K&R-era macros |
| miniz | deflate, inflate and zip archives |
| libyaml | state-machine scanner, parser and emitter |
| lua | interpreter: VM, GC, parser and standard library |
| yyjson | one large, heavily optimised C file |
| cxxopts | header-only C++ option parser |
| fmt | template-heavy C++ formatting library |

For each project the tool:

1. fetches the pinned commit into a cache (`--cache`, default
   `~/.cache/prism/bench`);
2. copies the manifest's files into a scan tree and writes a
   `compile_commands.json` that all three tools read;
3. runs `prism TREE --no-llm`, `cppcheck --project=compile_commands.json`
   and `clang-tidy` with a fixed check list;
4. records CBMC, ESBMC and Infer as `NOTRUN` with the reason (Law 1);
5. measures wall time and the machine's load average during each run.

It reports PRISM's rows by stage and status, a final status per function,
defects per thousand lines, and the overlap of PRISM's defect rows with
cppcheck's and clang-tidy's by file, line and kind. It also writes a
fixed-seed sample of defect rows with source excerpts for hand triage.

```
build/prism-qa bench [--prism BIN] [--only cJSON,zlib] [--out bench-out] [--jobs 12]
```

Output: `bench.json`, `bench.md`, `sample-prism.md`, `sample-cppcheck.md`,
and each tool's raw output under `<out>/<project>/`.

## Results

No `prism-qa bench` run is recorded yet. This page will carry its table,
with the date, the commit and the machine load, once one is.

The previous evaluation (zlib, cJSON, jsmn, tinyexpr and cxxopts; written
2026-09-25, rerun on commit 02d7da9be on 2026-09-26) measured a binary that
still had the deleted `bmc` stage, so its numbers no longer describe PRISM.
It is in git history. The rerun recorded 251 `ERROR` rows on cJSON.

## What the earlier evaluation left in the tree

The fixes it led to have regression tests that still run:

- `tests/cpp/test_main.cpp` "real-world forms: export macros and TEST()
  bodies are parsed", "real-world: zlib K&R forms parse", "real-world:
  interval, fuzz and replay leave floating point alone", "real-world: a
  missing header is NOTRUN, not a compiler error", "real-world: pir finds
  root/include headers; a missing header is NOTRUN" and "real-world: fuzz
  runs a no-parameter function's one input once": reduced reproducers from
  jsmn, tinyexpr, cJSON, cxxopts and zlib.
- `tests/cpp/test_pir_mem.cpp`: a literal `sprintf` argument renders to its
  exact length (cJSON's `sprintf(version, "%i.%i.%i", 1, 7, 16)` into
  `char[15]`).
- `testdata/fp/realworld_switch.c` and `testdata/fp/realworld_forms.c`:
  real-code forms that caused lint false alarms; they must stay silent.
  `testdata/tp/realworld_twins.c` holds their true-positive twins, which must
  still fire (`tests/cpp/test_main.cpp`).
