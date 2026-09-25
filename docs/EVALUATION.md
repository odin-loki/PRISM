# PRISM on real code: evaluation (2026-09-25)

PRISM is meant to be one command that finds every error it can and says
honestly what it could not check. This page records what happened when the
C++ engine (`prism PATH --no-llm`) was run on five small, real open-source
C/C++ projects, what broke, what was fixed (branch `claude/w6-realw`), and
what it still misses. All numbers below were measured; nothing is estimated.

## Setup

| project | commit (tag) | C/C++ lines | units | compile_commands.json |
|---|---|---:|---:|---|
| zlib | `21767c654d31` (v1.2.12, 2022-03-27) | 41,991 | 44 | yes (`cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, in `build/`) |
| cJSON | `cb8693b058ba` (v1.7.16, 2023-07-05) | 22,147 | 76 | yes (`build/`) |
| jsmn | `25647e692c79` (2021-10-14) | 1,168 | 3 | no (Makefile) |
| tinyexpr | `c3b2f32eee61` (2026-09-20) | 2,499 | 7 | no (Makefile) |
| cxxopts | `60286ded02a1` (2026-09-22) | 23,610 | 6 | yes (`build/`) |

Line and unit counts include each project's tests, examples and vendored
test frameworks (cJSON ships Unity, cxxopts ships a 17,000-line
`catch.hpp`), because PRISM scans everything under the path it is given
except `build*/`, `third_party/` and the like (the inventory stage says which
directories it skipped). zlib 1.2.12 and cJSON 1.7.16 were chosen because
they carry CVEs fixed later (see "Known CVEs" below).

Tools present: clang/clang++ 18, opt 18, gcc 13, clang-tidy 18, bwrap,
Z3 (vendored), Bitwuzla, CaDiCaL, Kissat. Absent, and reported `NOTRUN` in
every run: cppcheck, ESBMC, Dafny, CBMC, KLEE, AFL++, Frama-C, Infer,
Semgrep, Coccinelle (`spatch`), strix, ParanoidBSD. No run used
`--allow-exec`, so sanitize, the compiled fuzz harness and pir translation
validation are `NOTRUN` by Law 9. No LLM (`--no-llm`).

**Load.** The machine has 4 cores shared with five other agents' builds and
test runs; the load average was 15–20 throughout. Wall times are therefore
3–5x what an idle 4-core machine would show, and the same stage on the same
input varied by up to 4x between runs (the `optional` stage on jsmn: 4 s in
one run, 18 s in another, same binary). Read the times for their order of
magnitude and for the structural changes (a stage that went from minutes to
seconds because it stopped repeating work), not as benchmarks. The solver
query cache (`~/.cache/prism/solver`) was shared by all runs, so pir times
also depend on what earlier runs had cached.

"Before" is the branch base `c28ae3faf`; "after" is `1cad5e4ee`. Four later
commits were measured on their own, not by a full rerun: `a0b962721` and
`d1d16394e` (lints, on the reproducers and a lints-only rerun of tinyexpr),
`0f775f3a3` and `9bd128f94` (fuzz, back to back on zlib).

## Before: what broke

| project | wall | outcome |
|---|---:|---|
| jsmn | 27 s | ran; `jsmn_parse` and `jsmn_init` (the whole library) were `PARSE-GAP` (`JSMN_API int jsmn_parse(`), so no lint ever read them |
| tinyexpr | 277 s | ran; **19 fuzz `CRASH` and 16 `execute` "confirmed" `CRASH` rows on functions of `double`** (`double add(double a, double b) { return a + b; }` reported as signed-int overflow with `a=2147483647`), 22 interval `INT-SIGNED-OVF`/`INT-DIV-ZERO` on `double` arithmetic |
| cJSON | 964 s | ran; all 78 public `CJSON_PUBLIC(type) name(...)` functions were `PARSE-GAP` (not linted), 325 `ERROR` rows (230 concolic, 47 bmc, 41 pir, 7 inventory), fuzz alone 772 s |
| cxxopts | 499 s | ran; every unit under `src/` and `test/` pir `ERROR` ("clang front end failed": `cxxopts.hpp` is in `include/`), fuzz 256 s, 2 fuzz + 2 execute `CRASH` on Catch2's `marginComparison(double, double, double)` |
| zlib | **> 3600 s, no report** | killed by the 1-hour limit inside the pir stage; `optional` (clang-tidy, serial) alone took 218 s. Half the bodies of `deflate.c`, `trees.c` and `crc32.c` were silently dropped by the parser: neither a function nor a `PARSE-GAP` row (a Law 7 hole) |

## Fixes (each with a regression test and a reduced reproducer)

Reproducers are in `testdata_fp/realworld_*.c` (correct code that must give
no `FAILED` lint and no parse gap, both engines) and
`testdata_tp/realworld_twins.c`, `testdata_tp/knr_gap.c` (the buggy twins,
which must still fire). They are a few lines each, reduced from jsmn (MIT),
tinyexpr (zlib licence), cJSON (MIT) and zlib (zlib licence); each file names
its source. Tests: `tests/test_false_positives.py::TestRealWorldEvaluation`
and the `real-world:` cases in `tests/cpp/test_main.cpp`; the lint parity
test runs both engines on the new files.

Parser (both engines; identical inventory/classify rows on all five projects
and on `testdata/`, which is unchanged):

1. A leading ALL_CAPS export macro, `JSMN_API int jsmn_parse(`.
2. A function-like export macro wrapping the return type,
   `CJSON_PUBLIC(cJSON *) cJSON_Parse(`.
3. ALL_CAPS macro bodies such as Unity's / GoogleTest's `TEST(Suite, Name) {`
   are parsed as `OTHER` functions, so the lints see test bodies.
4. zlib's lowercase storage macro, `local block_state deflate_stored(s, flush)`.
5. K&R parameter declarations that are function pointers, `void (*init)(void);`.
6. An `#if`/`#else` whose arms each open a brace (`#ifdef FORCE_STORED if (..) {
   #else if (..) { #endif`, zlib `trees.c`): brace matching keeps the first arm
   (the ctags rule) when an arm is unbalanced; bodies keep every arm.
7. A top-level `{` whose head is K&R text the patterns cannot read is now a
   `PARSE-GAP` at the head's line instead of a silently skipped body.

zlib, parser only: `deflate.c` 15 → 29 of 30 bodies found, `trees.c` 6 → 23,
`crc32.c` 9 → 22 (+1 honest gap: `const z_crc_t FAR * ZEXPORT get_crc_table()`).

False alarms (both engines unless marked C++):

| class | cause on real code | fix |
|---|---|---|
| `CTRL-FALLTHROUGH` | `#ifdef` line between stacked labels; `case '}':` char literal closing the switch body; GCC `/* Falls through. */` comments; the last arm of a switch | preprocessor lines are not statements, char literals are blanked, GCC comment forms (and `falls through` in the AST layer) are annotations, the last arm is skipped |
| `MEM-VLA-SIZE` | `char buf[BUFSIZ]`, `buf[MAX_LEN * 2 + 1]`, `buf[sizeof(int) * 4]` | literals, `sizeof` and never-assigned ALL_CAPS names are constant |
| `PTR-NULL-DEREF` | `if (!n) return;` took the next line as its branch; a braced branch ran on into its `else` | the then-branch ends where it ends |
| `INT-BOOL-AS-BIT` (40 on cJSON) | any line holding a comparison and a `&` (`(a->type & 0xFF) != (b->type & 0xFF)`), `->` counted as `>`, `case '&':` | an operand of the `&`/`|` must itself be a comparison; char literals blanked |
| `CTRL-MISSING-RETURN` | `#endif` taken as the last statement after `#else return '.';` | preprocessor lines skipped |
| `STR-NULL-ARG` | a NULL test inside a larger condition (`if (object->valuestring == NULL \|\| valuestring == NULL)`) not seen | tests after `\|\|`, `&&`, `(` count |
| `LOCK-DOUBLE-LOCK` | two `clock()` calls "acquired twice" | `clock*`/`*block*` are not locks |
| `UNINIT-BRANCH` | `te_interp(expr, &err); if (err)` | an `&var` before the use assigns it |
| `INT-TRUNC` | `isxdigit((unsigned char)p[2])` | the `<ctype.h>` argument cast is not a narrowing |
| interval `INT-SIGNED-OVF` | `double` parameters/locals treated as `int`; `ULL` literal split off by the tokenizer; `unsigned long int un, ur` left untracked (signed full range) | floats and unparsed declarations are unencoded for that function (no finding; pir still checks it); `U` literals are unsigned, `L`/`LL` unencoded |
| fuzz / execute `CRASH` | the concrete interpreter ran `double` functions as `int` | float/double functions are `NEEDS-HARNESS` there (as concolic and bmc already were) |
| pir `MEM-OOB-WRITE` (C++) | `sprintf(version, "%i.%i.%i", 1, 7, 16)` into `char[15]` modelled with the type maximum (36 bytes) | a literal argument renders to its exact length |

Errors and gaps that are not code defects:

| where | before | after |
|---|---|---|
| warnings (C++) | a missing header (`'unity.h' file not found`) was `FAILED compiler-error`, and gcc's "No such file or directory" tripped the missing-tool check (`gcc unusable: failed to start`) | `NOTRUN "does not compile standalone: header 'unity.h' not found (include path unknown)"`; units now get their `compile_commands.json` `-I/-D/-std=` (else `-I root`, `-I root/include`) |
| clang-tidy | the same header errors were `FAILED clang-tidy` rows | `NOTRUN` (both engines) |
| pir (C++) | no include path at all: every cxxopts unit `ERROR` | `compile_commands.json` flags (never its `-std=`), else `-idirafter root` / `root/include`; a missing header is `NOTRUN` |
| concolic | `"test issue #22"` read as a preprocessor line (`#` anywhere); `cJSON *root = ...` read as a multiplication ("trailing tokens", 169 rows on cJSON); Catch2's `ResultDisposition::FalseTest` | directive lines only; a pointer-to-typedef local is "unknown typedef local unencoded" (all four front-end copies: bmc, interval, interpreter, Python); a C++ qualified name is `NEEDS-HARNESS` |

Performance:

| stage | cause | fix |
|---|---|---|
| fuzz (772 s on cJSON, 256 s on cxxopts) | the FuSeBMC concrete loop re-ran identical inputs: a no-parameter Unity/Catch2 test function has one input (padded to one byte, so up to 256 "different" ones) and was run for every seed and havoc step, and the second round repeated the first | an input already run is not run again (the oracle is deterministic; inputs are compared exactly, and a no-parameter function has one); without `--allow-exec` a round with unchanged seeds is skipped |
| fuzz on zlib (746 s for 12 fuzzable functions) | the FuSeBMC goal loop ran one BMC query per uncovered branch regardless of the fuzz budget | goals run while the round's budget lasts; the rest are counted in `extra.bmc_goals_skipped` (a goal only makes seeds; the bmc stage checks the function in full) |
| warnings (C++) | one compiler process at a time | `--jobs` threads (the Python engine already did this) |
| optional / clang-tidy (218 s on zlib, 94 s on cJSON) | one clang-tidy at a time | `--jobs` threads (the Python engine already did this) |
| pir on zlib `crc32.c` | the solver cache key normalisation ran Z3's simplifier with no time bound (one call > 15 min, past every `--timeout`) | 10 s bound; past it the unsimplified formula keys the cache |

## After

Full runs with the "after" binary (`1cad5e4ee`), same command, same
machine, run one after another. Counts are rows in `report.json`.

| project | wall before → after | ERROR | CRASH | NOTRUN | PARSE-GAP | functions found | lint FAILED |
|---|---|---|---|---|---|---|---|
| jsmn | 27 s → 86 s ¹ | 1 → 0 | 0 → 0 | 22 → 20 | 2 → 0 | 30 → 32 | 10 → 7 |
| tinyexpr | 277 s → 300 s ² | 1 → 1 | **35 → 0** | 19 → 19 | 0 → 0 | 95 → 95 | 48 → 36 (30 at `d1d16394e`) |
| cJSON | 964 s → 1763 s ³ | **325 → 251** | 0 → 0 | 262 → 141 | **160 → 0** | 1010 → 1170 | 88 → 51 |
| cxxopts | 499 s → 421 s | 12 → 3 | **4 → 0** | 99 → 46 | 76 → 26 | 1510 → 1561 | 108 → 55 |
| zlib | > 3600 s → > 3600 s ⁴ | 2 → 3 | 0 → 0 | 147 → 26 (before pir) | **120 → 2** | 423 → 604 | 272 → 175 |

¹ pir 16 s → 63 s with the same verdicts on the same 32 functions: load
(a pytest run of this repository ran beside it). ² pir 253 s in both
(`npr`, below). ³ pir 73 s → 1313 s: 38 more cJSON/Unity units now compile
and are checked (pir `ERROR` 41 → 3), and the machine was swapping
(1.6 GB available of 16 GB, other agents' runs); fuzz 772 s → 99 s. ⁴ Killed
by the one-hour limit inside pir both times. Before pir the run took 580 s
(optional 272 s, bmc 249 s: bmc now sees the 180 functions the parser used
to drop; before, it took 0.3 s because it saw almost nothing).

Back-to-back stage timings (same load, base binary then new binary, only
`inventory,classify` and the named stages):

| project, stages | before | after |
|---|---:|---:|
| cJSON fuzz | 1378 s | **94 s** |
| cJSON warnings / optional | 15 s / 283 s | 13 s / 244 s |
| zlib fuzz (after the goal budget, `9bd128f94`) | 10 s (on the 10 functions it could parse) | 255 s (on 12 fuzzable of 604; 746 s before the goal budget) |
| zlib warnings / optional | 11 s / 269 s | 10 s / 254 s |

The parallel clang-tidy barely shows under this load (the run has two
`--jobs` on four cores that were each already four times oversubscribed);
the fuzz fixes are structural (inputs no longer re-run) and show at any
load. The remaining ERROR rows on cJSON are Unity's own test suite
(194 of 201 concolic rows: `int p[] = {..}`, `ULL` literals, struct member
calls such as `global_hooks.allocate(size)` that the concrete interpreter
does not read) and bmc front-end gaps (47).

## Findings: true and false positives (sample)

The sample is every `FAILED`/`CRASH` row of the `lints`, `interval`,
`taint`, `pbsd`, `pir`, `fuzz` and `execute` stages on jsmn and tinyexpr,
and the most frequent classes on cJSON and zlib. "Precondition" means the
report is right only if a caller can pass the value (a NULL or zero
argument): the function does not check, but the project's contract may
forbid it. Compiler warnings (`-Wconversion`, `-Wsign-conversion`: 60 on
jsmn, 110 on tinyexpr, 561 on zlib) and yamllint/ruff rows on the projects'
CI files are not classified.

| project | row | verdict |
|---|---|---|
| jsmn | `test/testutil.h:84` `parse` MEM-LEAK: `t = malloc(...)`, `return 0` on status mismatch without `free(t)` | **TP** (test helper) |
| jsmn | `example/simple.c:16` STR-NULL-ARG `strlen(s)` | precondition |
| jsmn | `example/jsondump.c:87` INT-WRAP-ALLOC `malloc(sizeof(*tok) * tokcount)` | FP in context (`tokcount` is 2, grows by doubling under a realloc check) |
| jsmn | `jsmn.h:148/155` CTRL-FALLTHROUGH, `jsondump.c:77` MEM-VLA-SIZE `buf[BUFSIZ]`, concolic ERROR on `tests.c main` | FP / PRISM bug — **fixed** |
| tinyexpr | pir `smoke.c:844 number_rand` INT-DIV-ZERO `% modulus`, cex `modulus=0` | precondition (callers pass a nonzero modulus) |
| tinyexpr | lints `smoke.c:829` STR-MISSING-NUL `memcpy(expr + j*4, "sin ", 4)` | FP (the terminator is written after the loop) |
| tinyexpr | taint `repl.c:34` `strcpy(line, buf)` | FP (`line = malloc(strlen(buf) + 1)`) |
| tinyexpr | `tinyexpr.c:882/896` CTRL-FALLTHROUGH (an arm that ends in a nested `switch` whose every arm returns) | FP, **open** |
| tinyexpr | `tinyexpr.c:209/228` MEM-PTR-ARITH `name[len]` | precondition |
| tinyexpr | `tinyexpr.c:97` PTR-UNCHECKED-ALLOC `new_expr`: `ret = malloc(size); CHECK_NULL(ret);` | FP, **open** (the check is inside a function-like macro the lint does not expand) |
| tinyexpr | 22 interval, 19 fuzz CRASH, 16 execute CRASH on `double` functions; `te_free_parameters` fallthrough ×7 and NULL-deref; LOCK-DOUBLE-LOCK on `clock()`; UNINIT-BRANCH ×3; INT-BOOL-AS-BIT ×2; INT-TRUNC ×7 | FP — **fixed** |
| cJSON | pir `tests/unity/src/unity.c:184` `UnityPrintNumber` INT-SIGNED-OVF `-number_to_print` with `INT64_MIN` | **TP** (Unity's own comment says "including MIN negative"; the negation is UB) |
| cJSON | pir `cJSON.c` `cJSON_CreateNumber` PTR-NULL-DEREF "call through a null function pointer", "FAILED also with the globals' initial values" | FP, **open**: `global_hooks = { malloc, free, realloc }`; a 3-member hooks struct reached through `const internal_hooks * const hooks` plus a `memset` of the new item reproduces it (a 2-member struct does not); handed to the pir false-alarm work |
| cJSON | pir `cJSON_Version` MEM-OOB-WRITE | FP — **fixed** (sprintf model) |
| cJSON | lints `unity_fixture_Test.c:496..540` MEM-UAF ×6 (new: `TEST()` bodies are parsed now): `free(m); TEST_ASSERT_NOT_NULL(m);` | pedantic TP: the freed pointer's value is read, never dereferenced (indeterminate after `free`, C17 6.2.4p2); harmless in practice |
| cJSON | INT-BOOL-AS-BIT ×40, CTRL-MISSING-RETURN `get_decimal_point` | FP — **fixed** |
| zlib | interval `gzlib.c:627 gz_intmax` INT-SIGNED-OVF (`unsigned p, q;`) | FP — **fixed** |
| zlib | pbsd `examples/zran.c:110` MEM-CAPACITY-FIRST (`index->gzip <<= 1` before `realloc`, but the failure arm frees the whole index) | FP, **open** |
| zlib | taint `examples/gun.c:689` `memcpy(outname, *argv, len)` with `outname = malloc(len + 1)`, `len = strlen(*argv)` | FP, **open** |

## Known CVEs

| CVE | where | fixed in | found? |
|---|---|---|---|
| CVE-2024-31755 | cJSON `cJSON_SetValuestring`: `strlen(valuestring)` with `valuestring == NULL` | 1.7.18 | **yes, after the parser fix only**: `STR-NULL-ARG strlen() called with unchecked pointer parameter valuestring` (`cJSON.c:408`) on 1.7.16; not reported on 1.7.18, where the fix added `valuestring == NULL` to a larger condition (that the lint did not see this test was itself a false alarm, fixed). Before, the function was a `PARSE-GAP` and nothing read it. It is a lint (FINDS), not a proof: the model checker gives `NEEDS-HARNESS` to every pointer-parameter function (Law 6) |
| CVE-2023-50472 | cJSON `cJSON_SetValuestring`: `strlen(object->valuestring)` with a NULL member | 1.7.17 | **no** (a field of a pointer parameter; Law 6 `NEEDS-HARNESS`, no lint for field NULL-ness) |
| CVE-2023-50471 | cJSON `cJSON_InsertItemInArray`: `newitem->prev->next` with `after_inserted->prev == NULL` on a corrupted list | 1.7.17 | **no** (same reason) |
| CVE-2022-37434 | zlib `inflate()`: heap overflow copying the gzip header extra field when `len > extra_max` | 1.2.12.1 (`eff308af`) | **no**: `inflate(z_streamp strm, int flush)` is a pointer-parameter function (pir `NEEDS-HARNESS`, Law 6), and no lint models the `state->head->extra + len` arithmetic. Finding it needs a harness that builds a `z_stream` and a gzip header (`--allow-exec` + a fuzz harness, or a written pir contract) |

So on this sample PRISM found one of four CVEs, and only because the parser
fix let a lint read the function. The model-checking stages cannot reach
library APIs that take pointers without a harness; they say so on every
such function (`NEEDS-HARNESS`), which is honest but is the main thing
standing between PRISM and these bugs.

## What is still open

- **zlib does not finish in an hour.** After the normalisation bound, pir on
  `crc32.c` alone still ran past 25 minutes (stack samples: Z3's simplifier
  and solver on the braided-CRC verification conditions, 64-bit, unwind 8).
  Each query is bounded (`--timeout`), but a function has many of them and
  nothing bounds the function. A per-function (or per-run) pir budget that
  reports the rest `TIMEOUT`/`UNKNOWN` is needed; the Houdini run budget in
  progress on another branch covers only loop invariants. With `--skip pir` zlib finishes: 1365 s
  (fuzz 763 s before the goal budget, optional 280 s, bmc 260 s); the
  one-hour run leaves `stages.jsonl` (resumable with `--resume`) but no
  `report.json`.
- tinyexpr `npr` (inlines `ncr` and `fac`, `double` in and out) takes most of
  its 250 s pir time in Houdini and ends `BOUNDED` ("Houdini ran out of
  time"); pir runs the functions of one unit one after another.
- The concrete interpreter still has front-end gaps that surface as `ERROR`
  (by design: an unmapped parse failure is an error, not a silent skip):
  `unsigned long long` locals, unsized array initialisers `int p[] = {..}`
  (Unity's own tests), `ULL` literals. They are PRISM gaps, listed per
  function.
- Open false alarms listed in the table above (cJSON_CreateNumber null
  function pointer, tinyexpr nested-switch fallthrough, zran capacity, gun
  taint, STR-MISSING-NUL on a later terminator).
- cppcheck, ESBMC, CBMC, Infer, Frama-C, KLEE, AFL++, Semgrep were not
  installed; their rows are `NOTRUN` and every number here is without them.
