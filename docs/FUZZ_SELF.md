# Fuzzing PRISM's own parsers

PRISM reads files it does not control: the sources it checks, LLVM IR from
clang, and the `report.json`, `stages.jsonl` and `functions.json` that
`--resume` and the GUI read back. None of them may crash PRISM, hang it or
corrupt its memory. This page describes the libFuzzer harness, how to run
it, and every finding with its ID (F1–F4). Tests and source comments cite
the IDs.

## Harness

One libFuzzer target, `prism_fuzz_self` (`tests/fuzz/fuzz_cparse.cpp`),
built with ASan + UBSan. The first input byte picks the parser; the rest
is the input.

| Byte % 3 | `PRISM_FUZZ_TARGET` | Parsers under test |
|---|---|---|
| 0 | `cparse` | `prism::extract_functions`, `parse_gaps`, `extract_functions_from_text` (C/C++ source) |
| 1 | `pir` | `prism::pir::ir::parse_module` (LLVM IR text) |
| 2 | `report` | `prism::RunReport::load`, `journal_read_stages`, `journal_completed_ok`, `journal_read_functions` (the same bytes as each file) |

`PRISM_FUZZ_TARGET` forces one parser so a campaign spends its whole budget
there.

The contract: no sanitizer report, no abort, no uncaught exception, no
input slower than the timeout. A parser may reject input by returning
empty / `nullopt`, or by throwing a `std::exception` (`parse_module` does
this for malformed IR). Anything else is a finding for this page.

The target is built from the parser sources only (`cparse`, `scope`,
`laws`, `regex`, `models`, `journal`, `pir/ir_parser`, `pir/fp`,
`verdict/verdict`), so only they carry the instrumentation. Z3 is not
linked.

## Seed corpus

`prism_fuzz_corpus OUT_DIR [--prism BIN] [--limit N] [--repo DIR]`
(`src/tools/fuzz_corpus.cpp`) writes the seeds from files already in the
repository:

| Directory | Seeds |
|---|---|
| `c/` | C and C++ files from `testdata/planted/` and `testdata/conformance/prism/`, at most 64 KB each and N files (default 200) |
| `ll/` | those files lowered the way `pir` lowers them (`clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone`, then `opt -passes=mem2reg,…`) |
| `report/` | `report.json`, `stages.jsonl`, `functions.json` of a PRISM run on `testdata/planted/` (`--no-llm`, no `--allow-exec`), plus hand-written JSON edge cases |
| `toml/` | `third_party/MANIFEST.toml` and `proofs/lakefile.toml` |
| `libfuzzer/` | the `c/`, `ll/` and `report/` seeds with the selector byte |

The tool compiles seeds but never runs them. A missing clang, opt or
`prism` binary prints `NOTRUN` for that part. No harness reads the `toml/`
seeds. `tests/cpp/test_repo_tools.cpp` tests the corpus maker.

## How to run

Linux, clang (the harness includes `<unistd.h>`):

```
tools/fuzz_self/run_cpp.sh BUILD_DIR SECONDS        # configure -DPRISM_FUZZ=ON, seeds, one campaign
cmake --build BUILD_DIR --target fuzz-self-run       # the same in a configured tree (-DPRISM_FUZZ_SECONDS=N, default 300)
PRISM_FUZZ_TARGET=pir BUILD_DIR/prism_fuzz_self -max_total_time=600 CORPUS_DIR   # one parser
```

`run_cpp.sh` configures with `PRISM_Z3=OFF PRISM_TESTS=OFF`, builds
`prism` for the report seeds, then runs `fuzz-self-run`. It exits 3
(NOTRUN) when the compiler has no libFuzzer.

`fuzz-self-run` runs libFuzzer in fork mode
(`-fork=1 -ignore_timeouts=1 -ignore_ooms=1 -ignore_crashes=1`,
`-timeout=25`, `-rss_limit_mb=2048`, `-max_len=65536`), so one finding
does not hide the next. Artifacts land in `BUILD_DIR/fuzz-artifacts/`.

With a compiler that does not accept `-fsanitize=fuzzer`, CMake warns that
`prism_fuzz_self` is NOTRUN, and `fuzz-self-run` prints NOTRUN and fails.

No CI job runs the harness.

## Findings

All four findings are fixed (2026-09-23, commit 30f0bb432). F2–F4 were
found in the deleted Python engine; the IDs are kept because tests and
code cite them.

### F1: the C parser took super-linear time on crafted input

- **Where:** `src/prism/cparse.cpp`, `match_at_start`, reached from the
  `inventory` stage on every file.
- **Cause:** `match_at_start` ran an unanchored PCRE2 search and dropped a
  match that did not start at offset 0. When a declarator pattern failed
  at offset 0, PCRE2 retried at every later offset, so one failed match
  cost O(n²) in the text before a `{`.
- **Reproducer:** `python3 -c "print('int f ' + 'z' * 32000 + ' (x) { }')" > zrun.c`,
  then `prism zrun.c --no-llm --stage inventory`.
- **Fix:** `match_at_start` calls `Regex::match_prefix`
  (`include/prism/regex.hpp`), an anchored match (`PCRE2_ANCHORED`).
- **Still open:** `full_match` in `cparse.cpp` still runs an unanchored
  search and checks the span. There is no F1 regression test.

### F2: a report of the wrong shape broke the report loader

- **Contract:** `RunReport::load` (`src/prism/models.cpp`) returns a report
  or `nullopt`; it never throws on well-formed JSON of the wrong shape.
- **Reproducers** (the whole of `report.json`): `{"stages": "x"}`,
  `{"functions": [1]}`, `{"stages": [1]}`, `[]`, `"x"`.
- **Test:** `tests/cpp/test_concrete.cpp`, "F2: RunReport::load returns
  nullopt on a report of the wrong shape".

### F3: a journal line of the wrong shape broke the journal reader

- **Contract:** `journal_read_stages` (`src/prism/journal.cpp`) skips lines
  it cannot read.
- **Reproducers** (one line of `stages.jsonl`): `""`, `[1]`, `5`.
- **Test:** `tests/cpp/test_concrete.cpp`, "F3: journal lines of the wrong
  shape are skipped" (the valid line after them is still read, and
  `journal_completed_ok` still sees it).

### F4: a manifest that is not UTF-8 broke the manifest loader

- **Reproducer:** `third_party/MANIFEST.toml` holding the single byte
  `0xF3`.
- The Python loader that raised is deleted. No harness fuzzes the C++
  TOML reader.

## Not covered

- AFL++ as an external fuzzer of PRISM itself.
- The TOML reader, the polyglot output parsers, the SARIF writer, the GUI
  and the `prove` subcommand's Lean output parser.
- Any CI run of the harness.
