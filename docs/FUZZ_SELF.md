# Fuzzing PRISM's own input handling

Roadmap 6.2 ("fuzzing PRISM's own input handling"). PRISM reads files it does
not control: the sources it checks, and the `report.json`, `stages.jsonl`,
`functions.json` and `MANIFEST.toml` files that `--resume`, the GUI and the
tool lookup read back. None of them may crash PRISM, hang it or corrupt its
memory. This file records the harnesses, the campaigns run so far and every
finding with a reproducer. **Findings are reported here, not fixed**: fixing
engine code was out of scope for the change that added the harnesses.

## Harnesses

| harness | engine | parsers under test | contract checked |
|---|---|---|---|
| `tests/fuzz/fuzz_cparse.cpp` (libFuzzer, ASan + UBSan) | C++ | `prism::extract_functions` and `parse_gaps` (C/C++ sources), `prism::pir::ir::parse_module` (LLVM IR text), `prism::RunReport::load`, `journal_read_stages`, `journal_read_functions` | no sanitizer report, no abort, no uncaught exception (`parse_module` may reject input by throwing `std::exception`), no input slower than the timeout |
| `tools/fuzz_self/fuzz_py.py` (mutation fuzzer) | Python | `prism.cparse.extract_functions(_from_text)`, `prism.models.RunReport.load`, `prism.journal.read_stages` / `read_functions` / `completed_ok`, `prism.config.load_manifest` | returns its documented type (`list`, `RunReport` or `None`, `dict`) and never raises; no input slower than 5 s |

Seeds (`tools/fuzz_self/make_corpus.py`): the C/C++ files of `testdata/` and
`tests/conformance/prism/`, their LLVM IR compiled the way the `pir` stage
compiles it (`clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone`, then
`opt -passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer`), the
`report.json` / `stages.jsonl` / `functions.json` of a PRISM run on
`testdata/`, hand-written JSON edge cases, `third_party/MANIFEST.toml` and
`pyproject.toml`.

## How to run

```
python tools/fuzz_self/make_corpus.py /tmp/fuzz-seeds            # seeds (PRISM_BIN for the report seeds)
python tools/fuzz_self/fuzz_py.py /tmp/fuzz-seeds --seconds 300  # Python engine, 300 s per target
tools/fuzz_self/run_cpp.sh /tmp/prism-fuzz-build 600             # C++: configure -DPRISM_FUZZ=ON, build, run
PRISM_FUZZ_TARGET=pir /tmp/prism-fuzz-build/prism_fuzz_self -max_total_time=600 corpus/   # one parser only
```

`-DPRISM_FUZZ=ON` needs a compiler that accepts `-fsanitize=fuzzer` (clang);
with any other compiler CMake prints that `prism_fuzz_self` is NOTRUN. The
target is built from the parser sources only (no Z3).

## Campaigns run (2026-09-23, 4-core machine shared with other jobs)

| campaign | duration | executions | result |
|---|---|---|---|
| C++, all three parsers, single process | 600 s budget; libFuzzer stops at the first timeout | 11,862 | 1 timeout (F1) |
| C++, all three parsers, `-fork=1 -ignore_timeouts=1` | 1,521 s | 32,261 | 3 more timeouts (F1); 0 crashes, 0 sanitizer reports, 0 out-of-memory |
| C++, `PRISM_FUZZ_TARGET=pir` (CMake-built `prism_fuzz_self`) | 626 s | 81,558 | nothing: 0 crashes, 0 sanitizer reports, 0 timeouts, 0 out-of-memory |
| C++, `PRISM_FUZZ_TARGET=report` (CMake-built `prism_fuzz_self`) | 610 s | 16,638 | nothing: 0 crashes, 0 sanitizer reports, 0 timeouts, 0 out-of-memory |
| Python, seed 1, 150 s per target | 600 s | cparse 174,853; report 445,428; journal 124,113; manifest 357,181 | F2, F3, F4; no slow input |
| Python, seed 2, 300 s per target | 1,200 s | cparse 359,230; report 521,638; journal 293,428; manifest 749,968 | the same F2, F3, F4 signatures, nothing new; no slow input |

All four C++ timeouts were in the C parser. Because slow C inputs dominate
the time of a mixed campaign, the IR parser and the report readers were
also fuzzed on their own (`PRISM_FUZZ_TARGET`).

## Findings

### F1 (C++): the C parser takes super-linear time on crafted input (denial of service)

- **Where:** `src/prism/cparse.cpp`, `match_at_start` (called from
  `Parser::scan_definition`; the libFuzzer stack is
  `scan_definition` → `match_at_start` → `Regex::search_match` →
  `pcre2_match`). Reached by the `inventory` stage, which runs first on
  every file of every run.
- **Reproducer (synthetic, 32 KB):**

  ```
  python3 -c "print('int f ' + 'z' * 32000 + ' (x) { }')" > zrun.c
  ./build/prism zrun.c --no-llm --stage inventory
  ```

  Release build, `inventory` only: 1,000 → 0.05 s, 2,000 → 0.09 s,
  4,000 → 0.22 s, 8,000 → 0.89 s, 32,000 → 5.9 s CPU. The Python engine
  parses the same file in 0.24 s CPU.
- **Fuzzer inputs** (libFuzzer artifact names; the first byte is the target
  selector, the rest is the C file). CPU time of the release `prism` binary
  on `--stage inventory`:

  | artifact | size | CPU |
  |---|---|---|
  | `timeout-45114cfa780058a2f0b937736f1c4af5f969d7f5` | 7.6 KB | 2.7 s |
  | `timeout-1608ac1ae4675f456b3205b489c129400e6d314c` | 15.5 KB | 52 s |
  | `timeout-a1e35d668a20b45f1eeb2165d80846b7a1219f65` | 28.8 KB | 84 s |
  | `timeout-300b39423789bdf3f2bf4482ad94fc3bd30c19d7` | 36.9 KB | 317 s |

  The larger ones are an unterminated `/*@` contract comment followed by
  LLVM IR text (a splice of two seeds), which the parser scans as code.
- **Cause:** `match_at_start` emulates Python's `re.match` by running an
  *unanchored* PCRE2 search and discarding a match that does not start at
  offset 0. When the declarator patterns (for example `FUNC_PTR_DECL_PAT`,
  which starts `\s*` and then `[A-Za-z_]\w*`) fail at offset 0, PCRE2 retries
  at every later offset, each retry scanning the rest of the head, so one
  failed match costs O(n²) in the length of the text before a `{`, and the
  scan does this for every brace. Python's `re.match` is anchored, which is
  why the Python engine is unaffected.
- **Validated fix direction (not applied):** in a local experiment outside
  the repository, passing `PCRE2_ANCHORED` to `pcre2_match` for
  `match_at_start` brought all four artifacts to 0.06 s or less and the
  32 KB synthetic file to 0.01 s, and `extract_functions` + `parse_gaps`
  returned identical results (function count, names and bodies hashed, gap
  count) on all 2,223 C/C++/`.i` files of `testdata/`, `tests/conformance/`
  and the seed corpus (4,351 functions). `full_match` has the same
  unanchored-search shape and should get the same treatment.
- **Impact:** anyone who can put a file into a tree PRISM checks (for
  example a pull request in CI) can make the run take minutes to hours.
  Nothing executes, so it is availability only.

### F2 (Python): `RunReport.load` raises on well-formed JSON of the wrong shape

- **Contract:** `prism/models.py` `RunReport.load` returns a report or
  `None`; it catches `OSError`, `JSONDecodeError`, `TypeError` and
  `ValueError` only.
- **Reproducers** (as the whole content of `report.json`):
  `true` and `[]` (`AttributeError`, `models.py:116`, `data.get` on a
  non-object); `{"stages":"x"}` (`models.py:169`); `{"functions":[2]}`
  (`models.py:126`); `{"stages":[{"findings":[null]}]}` (`models.py:154`).
- **Reached from:** `python -m prism PATH --resume` when `stages.jsonl` is
  absent (`prism/pipeline.py`, the `report.json` fallback): verified to exit
  1 with a traceback on `{"stages":"x"}`. Also `prism/gui.py` when it opens a
  report.
- The C++ `RunReport::load` and journal readers handle every F2 and F3
  reproducer (each was run through `prism_fuzz_self` with the `report`
  selector: no exception, no sanitizer report).

### F3 (Python): `journal.read_stages` raises on a journal line that is JSON but not an object

- **Contract:** `prism/journal.py` `read_stages` skips unreadable lines; it
  catches `JSONDecodeError`, `TypeError` and `ValueError`.
- **Reproducers** (one line of `stages.jsonl`): `""`, `1`, `[]`
  (`AttributeError`, `models.py:169`, `stage_from_dict`), and
  `{"findings":[1]}` (`models.py:154`, `finding_from_dict`).
- **Reached from:** `python -m prism PATH --resume`: verified to crash with
  `AttributeError: 'str' object has no attribute 'get'` when the journal
  holds the line `""`. The C++ engine resumes normally on the same journal.
- `read_functions` handles the equivalent inputs (it returned a list for
  `[1]`, `{"a":1}`, `"x"` and `[{"params":[1]}]`).

### F4 (Python, low): `config.load_manifest` raises on a manifest that is not UTF-8

- **Contract:** `prism/config.py` `load_manifest` returns `{}` when the
  manifest is unreadable; it catches `OSError` and `tomllib.TOMLDecodeError`.
- **Reproducer:** `third_party/MANIFEST.toml` containing the single byte
  `0xF3`: `tomllib` raises `UnicodeDecodeError`, which is not a
  `TOMLDecodeError`.
- **Impact:** low; the manifest ships with PRISM and is checked in CI.

## Not covered yet

- AFL++ as an external fuzzer (roadmap 6.2 names it): not run here.
- The polyglot parsers, the SARIF writer, the GUI and the `prove`
  subcommand's Lean output parser are not fuzzed.
- The harnesses are not run in CI; the campaigns above are the only
  evidence.
