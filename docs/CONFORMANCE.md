# PRISM conformance: metrics, release gate, known issues

PRISM's verdicts are graded against tasks whose answer is known. This file
describes the grader (`prism-qa conformance` and `prism-qa soundness`), the
task format, how a run is scored, the release gate and every wrong proof or
false alarm found so far. Where each task set comes from, with its pin and
licence, is in
[`testdata/conformance/SOURCES.md`](../testdata/conformance/SOURCES.md).
Verdicts are defined in [VERDICTS.md](VERDICTS.md).

## The grader: `prism-qa`

`prism-qa` (`src/tools/qa/`) is built with the engine (`cmake --build build`).
It is not part of the release archive. It grades whichever PRISM binary it is
given: `--prism BIN`, else `$PRISM_BIN`, else `build/prism`.

| command | what it does |
|---|---|
| `prism-qa conformance` | runs PRISM on the conformance suite, scores every verdict, applies the release gate |
| `prism-qa conformance --self-check` | checks the suite's own labels with sanitizers; PRISM does not run |
| `prism-qa soundness` | generates random C programs and executes every function PRISM proves |

`prism-qa --help` lists the other commands (`libc-bounds`, `solver-bench`,
`pir-lean-check`, `llvm-sem-vs-lli`, `triage-selfscan`, `assurance-check`,
`docs-check`).

```
build/prism-qa conformance --self-check                 # labels only (clang with UBSan/ASan runtimes)
build/prism-qa conformance                              # the suite; exit 1 on any wrong proof
build/prism-qa conformance --certified                  # also score pir under --certified
build/prism-qa conformance --fetch-juliet /tmp/juliet   # NIST Juliet 1.3 (sha256-checked)
build/prism-qa conformance --juliet /tmp/juliet         # add the Juliet tasks
build/prism-qa conformance --fetch-esbmc /tmp/esbmc     # ESBMC C++ regression tests at the pinned commit
build/prism-qa conformance --esbmc /tmp/esbmc           # add the fetched ESBMC tasks
build/prism-qa soundness -n 300                         # random programs, in-house generator
build/prism-qa soundness --generator csmith -n 100
```

### `prism-qa conformance` options

| option | meaning |
|---|---|
| `--prism BIN` | engine to grade (default `$PRISM_BIN`, else `build/prism`) |
| `--suite DIR` (repeatable) | task roots (default `testdata/conformance/{prism,sv-comp,concurrency,esbmc-cpp,libc-models}`) |
| `--juliet DIR`, `--fetch-juliet DIR`, `--juliet-flows LIST` | Juliet tasks; flows default `01` |
| `--esbmc DIR`, `--fetch-esbmc DIR`, `--esbmc-thorough`, `--curate-esbmc DIR` | ESBMC C++ tasks; `--curate-esbmc` rewrites the committed subset |
| `--stages LIST` | verdict stages to score (default: `pir` and `conc`, when `prism --list-stages` lists them) |
| `--certified` | run `pir` a second time with `prism --certified` and score it as the stage `pir-certified` |
| `--timeout S` | seconds per PRISM run (default 180); the certified run gets twice that |
| `--unwind N` | `--unwind` for every PRISM run |
| `--jobs N`, `-j N` | parallel PRISM runs (default: CPU count; cut so each run has at least 2500 MB of memory budget) |
| `--mem-limit-mb MB` | address-space cap per PRISM run (default 0: none); a run over it is no answer, never a proof |
| `--filter REGEX` | only tasks whose ident matches |
| `--no-replay` | do not replay counterexamples |
| `--solver-cache DIR` | solver query cache (default: a fresh one in the work dir, so no answer comes from an earlier run) |
| `--out DIR` | output directory (default `conformance-out`) |

Each PRISM run gets a share of the machine's memory as `PRISM_MEM_BUDGET_MB`;
PRISM enforces it per function as `UNKNOWN`. Set `PRISM_CONF_KEEP` to keep
the work directory.

Outputs in `--out`: `metrics.json` (run metadata and metrics per stage and
origin), `metrics.md` (the same as tables, plus lists of wrong proofs, false
alarms, Law 6 rows, stage crashes, functions never reported and
refutations not replayed), `results.json` (one row per function and
stage: findings, outcome, replay). `--self-check` writes `selfcheck.json`.

`prism-qa` reads task files with its own YAML-subset reader
(`src/tools/qa/support/yaml_subset.cpp`: block maps and sequences, flow lists
of scalars, quoted scalars, comments). Anything else is an error naming the
file and line. Random inputs come from a Python-compatible Mersenne Twister
(`src/tools/qa/support/pyrandom.hpp`), so a seed names the same programs and
input grids it named before the scorers were C++.

## The corpus

| root | origin | what it holds |
|---|---|---|
| `testdata/conformance/prism/` | `prism` | in-house tasks: one `.c`/`.cpp` file and one `.yml` sidecar each, true/false pairs grouped by construct |
| `testdata/conformance/sv-comp/` | `sv-comp` | the pinned SV-COMP subset (upstream task-definition files, read as they are) |
| `testdata/conformance/concurrency/` | `concurrency` | whole thread programs, scored by the `conc` stage only |
| `testdata/conformance/esbmc-cpp/` | `esbmc-cpp` | a label-checked subset of ESBMC's C++ regression tests |
| `testdata/conformance/libc-models/` | `libc-models` | contract harnesses for PRISM's own C and C++ library models |
| `--juliet DIR` (not in git) | `juliet` | NIST Juliet 1.3, CWE190/191/369/476/680 |
| `--esbmc DIR` (not in git) | `esbmc-cpp` | every convertible ESBMC C++ regression test |

The origin is the first directory below `testdata/conformance/`; the category
is the second (`prism/overflow` has origin `prism`, category `overflow`).
`SOURCES.md` lists the in-house categories and the upstream pins.

## Task format

A task is a `.yml` file next to its source:

```yaml
format_version: 1
input_files: add_false.c
language: C                 # or C++
property: no-overflow
expected:                   # function -> true (no violation for any input) | false
  add_false: false
witness:                    # false functions: inputs that trigger the violation
  add_false: [2147483647, 1]
```

| key | meaning |
|---|---|
| `input_files` | the source file (a list takes its first entry) |
| `language` | `C` (default) or `C++` |
| `property` | the property the labels speak for (table below) |
| `expected` | function → `true` / `false`, in file order |
| `witness` | function → argument list that triggers the violation (self-check and label replay) |
| `expect_status` | function → the exact status the laws demand (pointer parameters: `NEEDS-HARNESS`, Law 6) |
| `expect_class` | false function → `A\|B`: only a `FAILED` of these classes counts as its refutation |
| `sanitizer_blind` | functions whose violation no sanitizer reports (an uninitialised read, `memcpy(s, s)`); a clean replay is not a failure |
| `native_unsupported` | function → reason the native toolchain cannot build it; the self-check skips it with that reason |
| `timeout` | seconds per PRISM run for this file; raises, never lowers, `--timeout` |
| `prism_timeout` | PRISM's own `--timeout` (per solver query) for this file |
| `unwind` | PRISM `--unwind` for this file |
| `target` | `--target TRIPLE` PRISM analyses this file for (see "Data-model pairs") |
| `deterministic`, `std` | whole-program tasks (`esbmc-cpp`): one native run is the only behaviour; language standard of that run |

Properties and the finding classes that witness them
(`PROPERTY_CLASSES` in `src/tools/qa/support/task.cpp`):

| property | classes |
|---|---|
| `no-overflow` | `INT-SIGNED-OVF` |
| `no-div0` | `INT-DIV-ZERO` |
| `no-shift-ub` | `INT-SHIFT-UB` |
| `no-oob` | `MEM-OOB-READ`, `MEM-OOB-WRITE`, `MEM-PTR-ARITH` |
| `no-null-deref` | `MEM-NULL-DEREF`, `NULL-DEREF`, `PTR-NULL-DEREF` |
| `memsafety` | the pir memory-model classes (docs/PIR.md "Memory model") and `UNINIT-READ` |
| `no-fp-cast` | `FLOAT-CAST-OVF` |
| `no-uncaught` | `CXX-UNCAUGHT`, `CXX-THROW-NOEXCEPT`, `CXX-TERMINATE` |
| `norace`, `noassert`, `nodeadlock` | `CONC-DATA-RACE`, `FUNC-CONTRACT`, `CONC-DEADLOCK` (concurrency tasks) |
| `esbmc-cpp` | ESBMC's default checks: assertions, bounds, pointer safety, division by zero, uncaught exceptions |

A property not in this table (the libc harnesses use `libc-contract`) is
not property-scoped: any `FAILED` counts.

SV-COMP tasks keep the upstream task-definition format 2.0. The scorer reads
`input_files`, `options.language`, `options.data_model` and the first
`properties` entry, in the order `no-overflow.prp`, `valid-memsafety.prp`,
that has an `expected_verdict`. The verdict is about `main`. A task with
neither property is not a task here (`prism svcomp score` scores
`unreach-call`; see [SVCOMP.md](SVCOMP.md)).

### Data-model pairs

`prism/datamodel/` holds four true/false pairs whose answer flips between
LP64 (`x86_64-unknown-linux-gnu`) and LLP64 (`x86_64-pc-windows-msvc`):
`long + 1` at 2^31 − 1, `1L << 32`, `a[sizeof(long)]` and a `memcpy` of a
`wchar_t`. Each sidecar names its `target:`; the scorer passes it to PRISM
as `--target`. The self-check skips a task whose target has another data
model than the host toolchain. Counterexample replay builds for the host.

## Scoring

The scorer runs PRISM on each task file with
`--no-llm --stage inventory,classify,<verdict stages> --out DIR`, plus the
task's `--unwind`, `--timeout` and `--target`. Every function in `expected`
gets one row per verdict stage. `conc` is scored on `concurrency/` tasks
only, and those tasks only by `conc`.

Each row gets one outcome (`classify` in `src/tools/qa/support/report.cpp`),
checked in this order:

| outcome | when |
|---|---|
| `missing` | the stage reported nothing for the function (listed separately, Law 7) |
| `wrong-proof` | `PROVED`, `PROVED-UNBOUNDED`, `PROVED-ASSUMING` or `PROVED-CERTIFIED` on a `false` function |
| `law-ok` / `law6-violation` / `unexpected-status` | the task has `expect_status`: exactly that status, a proof or refutation where `NEEDS-HARNESS` was demanded, anything else |
| `false-alarm` | `FAILED` of the property's class on a `true` function |
| `failed-other-property` | `FAILED` of another class on a property-scoped task (`sv-comp`, `concurrency`, `esbmc-cpp`) |
| `proved` | a proof of a `true` function |
| `refuted` | `FAILED` of the property's class (and of an `expect_class` class, if given) on a `false` function |
| `bounded` | `BOUNDED` |
| `no-answer` | anything else (`NEEDS-HARNESS`, `UNKNOWN`, `TIMEOUT`, `ERROR`, a crashed run) |

Metrics, per stage and origin:

| metric | definition | target |
|---|---|---|
| **Soundness** | wrong proofs | **0**; any wrong proof fails the release gate |
| Completeness | `proved` / `true` functions | grows |
| Detection | `refuted` with a replayed counterexample / `false` functions | grows |
| Detection, not replayed | `refuted` / `false` functions | — |
| False alarms | `false-alarm` rows | 0 |
| Law 6 | `law-ok` / tasks with `expect_status` | all |

Rules that keep the numbers honest:

- `BOUNDED` is neither a proof nor a refutation (Law 2). It never counts
  toward completeness, detection or soundness.
- A `FAILED` whose counterexample does not replay is not detection. The
  verdict may be right for a wrong reason.
- Replay covers `prism` tasks with scalar parameters. The task is compiled
  with `-g -O0 -fno-wrapv -fsanitize=undefined,address
  -fno-sanitize-recover=all`, the counterexample's arguments are passed to
  the function, and a sanitizer must fire. The run is inside `bwrap` (`/`
  read-only, private `/tmp`, all namespaces unshared) when bwrap works here;
  `metrics.json` records which. SV-COMP, concurrency, ESBMC and Juliet tasks
  are whole programs with nondeterministic inputs or schedules: their
  refutations are "refuted, not replayed".
- `conc` never proves (a context-switch bound is not a proof), so its
  completeness is 0 by definition and every true label is at best `BOUNDED`
  (docs/CONCURRENCY.md).
- `--certified` adds the stage `pir-certified`. Its rows count toward the
  release gate like any other. `metrics.json` then also says how many
  loop-free `true` functions became `PROVED-CERTIFIED`, why the others did
  not, and which certified runs timed out.

### Self-check of the labels

`--self-check` validates the labels of `prism` and `esbmc-cpp` tasks without
PRISM:

- every `false` function's `witness` must trip UBSan/ASan
  (`sanitizer_blind` functions may run clean);
- every `true` function must run clean on an edge-value grid (type limits,
  0, ±1, powers of two, 46340/46341, …) plus 2000 random inputs;
- a deterministic ESBMC program is run once natively with assertions on; the
  result must match its label. Nondeterministic ones are skipped.

Functions with non-scalar parameters, `expect_status` tasks,
`native_unsupported` functions and tasks for another target are skipped and
listed with the reason. Any `FAIL` exits 1.

<a id="random-programs-prism-qa-soundness-in-house-generator-300-programs-900-functions"></a>

## Random programs (`prism-qa soundness`)

`prism-qa soundness` (`src/tools/qa/soundness.cpp`) generates small C
programs, runs PRISM's stages `inventory,classify,pir` on them and executes
every function PRISM proves under UBSan+ASan on the edge-value grid plus
3000 random inputs. A sanitizer report on a proved function is a wrong
proof: the program, function, inputs and sanitizer message go to
`OUT/bugs/`, and the exit status is 1. A `FAILED` whose counterexample does
not replay and whose grid shows no violation is listed as a suspected false
alarm.

| option | meaning |
|---|---|
| `--programs N`, `-n N` | programs (default 300) |
| `--funcs N` | functions per in-house program (default 3) |
| `--seed N` | seed of the first program (default 1) |
| `--generator G` | `inhouse` (default: scalar expressions, guards, bounded loops), `inhouse-ptr` (pointer programs for the pir memory model), `inhouse-loop` (symbolic-bound loops for loop invariants), `csmith` (scalar-only configuration with `--no-safe-math`; `NOTRUN`, exit 3, when `csmith` is not on `PATH`) |
| `--prism`, `--out`, `--jobs`, `--timeout` | as for `conformance` (`--out` default `soundness-out`, `--timeout` default 120) |

Only programs this tool generates are compiled and run, inside `bwrap` when
it works.

## Release gate

`prism-qa conformance` exits 1 while any row of any stage and origin is a
wrong proof, and prints `RELEASE GATE FAILED: N wrong proof(s)`.
`metrics.json` records `release_gate: PASS|FAIL`.
`.github/workflows/conformance.yml` runs the label self-check and the suite
(`-j 2 --mem-limit-mb 4096`) on every push and pull request; a wrong proof
fails the workflow and blocks a release. The same workflow runs nightly the
random-program campaign (in-house generator and Csmith) and the Juliet
tasks. The gate is never relaxed to make it pass: a wrong proof is fixed in
the engine, or the label is shown wrong by the self-check.

## Measured results

<a id="current-numbers-2026-09-23-after-the-encoder-soundness-fixes"></a>

Commit 991c4eafa, 2026-11-05: the `conformance-metrics` artifact of CI run
41968877547 (`.github/workflows/conformance.yml`, stages
`inventory,classify,pir,conc`, 1068 files, 1430 scored functions). Release
gate: PASS, 0 wrong proofs. The metrics record "sandbox for counterexample
replay: none (bwrap unavailable)".

| stage | origin | true | false | wrong proofs | proved true | refuted with replay | refuted, not replayed | false alarms | BOUNDED | no answer |
|---|---|---|---|---|---|---|---|---|---|---|
| pir | esbmc-cpp | 32 | 32 | 0 | 22 | 0 | 25 | 0 | 0 | 17 |
| pir | libc-models | 219 | 216 | 0 | 180 | 0 | 208 | 0 | 2 | 43 |
| pir | prism | 388 | 392 | 0 | 334 | 318 | 26 | 0 | 12 | 90 |
| pir | sv-comp | 75 | 42 | 0 | 37 | 0 | 30 | 0 | 26 | 7 |
| pir | all | 714 | 682 | 0 | 573 (80.3%) | 318 | 289 | 0 | 40 | 157 |
| conc | concurrency | 13 | 9 | 0 | 0 | 0 | 8 | 0 | 14 | 0 |

`conc` never proves; its best answer on a `true` task is `BOUNDED`.
Run `prism-qa conformance` on the commit you care about and read
`conformance-out/metrics.md`. Results quoted elsewhere must name their date
and commit.

## Known issues

Every wrong proof and false alarm found so far, each with a minimal
reproducer. Run one with
`./build/prism FILE --no-llm --stage inventory,classify,pir`. Each is a
regression test: a doctest named below and, where listed, a conformance
task under `testdata/conformance/prism/`. `pir` must refute every
soundness reproducer.

<a id="before-the-fixes-2026-09-23-24a72d40b"></a>

S1–S7 were found by the first measurement (2026-09-23, commit 24a72d40b)
against the `bmc` stage, PRISM's hand-written Z3 encoder at the time: 11
wrong proofs in the suite, 6 in Juliet, and 57 of 304 proofs in 300 random
programs. The `bmc` stage is deleted (commit c66284cde); the `pir` stage
(Clang/LLVM IR → PIR → Z3) does all bounded model checking and k-induction
now. The `regress/` tasks keep one `_true`/`_false` pair for each further
encoder bug found then (call arguments, `INT_MIN` constant, promoted
comparisons, helper early return, loop early exit, `break`/`continue`,
dangling `else`, code after a loop under k-induction, enumerator values,
cast aliasing, side effects under `?:`/`&&`, `long long` width, mixed
widths).

IDs are never reused. R1, R2, F1–F6 and G1–G4 were defects or coverage
gaps of the `bmc` front end and encoder only; they were removed with it.

### Soundness (wrong proofs)

The doctest "pir soundness: known wrong proofs are refuted (S1-S6)" in
`tests/cpp/test_main.cpp` holds the S1–S6 reproducers and the class each
must be refuted with.

**S1. Signed subtraction overflowing upward.** `a - b` with `b` negative.
```c
int sub_false(int a, int b) { if (a < 0) return 0; return a - b; }   /* a=5, b=-2147483647 */
```
Refuted: `INT-SIGNED-OVF`. Task: `overflow/sub_false`.

**S2. `INT_MIN % -1`** is undefined (C11 6.5.5p6).
```c
int r(int a, int b) { if (b == 0) return 0; return a % b; }
```
Refuted: `INT-SIGNED-OVF`. Task: `overflow/mod_intmin_false`.

**S3. Signed left shift of a negative value, or whose result does not fit.**
```c
int a(int x) { if (x < -1000 || x > 1000) return 0; return x << 2; }   /* x=-1 */
int b(int x) { if (x < 0 || x > 4) return 0; return x << 30; }         /* x=2 */
int c(int x) { if (x < 0 || x > 7) return 0; return 2147483647 << x; } /* x=1 */
```
Refuted: `INT-SHIFT-UB`. Tasks: `shift/shl_negative_false`,
`shift/shl_overflow_false`.

**S4. Integer promotion of `unsigned char` / `unsigned short` to `int`.**
```c
unsigned m(unsigned short a, unsigned short b) { return a * b; }   /* 65535*65535 overflows int */
int s(unsigned char c) { return c << 24; }                         /* 255<<24 overflows int */
int add(int a, unsigned char b) { return a + b; }                  /* INT_MAX+1 */
int v(unsigned char c, int d) {                                    /* c < -10 is a signed compare */
    if (c < -10 || c > 100) return 0;
    return 20 / d;                                                 /* d=0 */
}
```
Refuted: `INT-SIGNED-OVF`, `INT-SHIFT-UB`, `INT-SIGNED-OVF`,
`INT-DIV-ZERO`. Tasks: `overflow/ushort_mul_false`,
`overflow/mixed_sign_false`, `shift/char_shift_false`.

**S5. `int >> unsigned` is an arithmetic shift**: the result type is the
promoted left operand's.
```c
int f(int a, unsigned s) { if (s < 1 || s > 7) return 0; return (a >> s) - 2147483647; }   /* a=-8, s=1 */
```
Refuted: `INT-SIGNED-OVF`. Task: `shift/shift_by_unsigned_false`.

**S6. Library calls, macros and unknown callees are not UB-free.**
```c
#include <stdlib.h>
int a(int x) { return abs(x); }                            /* abs(INT_MIN) */
void sink(int);
int c(int d) { sink(100 / d); return 0; }                  /* d=0 in a call argument */
#include <limits.h>
int e(void) { int v = INT_MIN; if (v < 0) return v * 2; return 0; }
```
`a` and `e` are refuted with `INT-SIGNED-OVF`. `c` calls a function with no
definition and no model, so it is never a proof. Tasks: `overflow/abs_libc_false`, `macro/fn_macro_false`,
`cxx/cxx_template_false`.

**S7. Reading a local array element that was never written** is an
indeterminate value (C11 6.3.2.1p2).
```c
int f(int i) { int a[4]; a[0] = 1; return a[i & 3]; }     /* i=1 reads a[1] */
int g(int n) { int a[4]; for (int k = 0; k < n && k < 4; k++) a[k] = k; return a[0]; }  /* n=0 */
```
Refuted: `UNINIT-READ`. Aggregate initialisers (`= {0}`, `= {1, 2}`), string
literals and a full `memset` count as written; a partial `memset` does not.
UBSan and ASan cannot see an uninitialised read, so these tasks are
`sanitizer_blind`. Doctest: "pir soundness: unwritten array elements are
UNINIT-READ (S7)". Tasks: `memory/mem_uninit_false`,
`regress/uninit_elem_{loop,branch,unbounded}`, `regress/uninit_memset`,
`regress/array_init_{list,zero}`.

**S8. `main` checked as if the program started at `main`.** C++ dynamic
initialisation (constructors of globals, non-constant initialisers,
`llvm.global_ctors`) runs first.
```c++
struct C { C() { throw 1; } };
C global_c;                   // throws before main: std::terminate
int main() { return 0; }

struct B { int i; B() : i(5) {} };
B g;
int main() { assert(g.i == 0); }   // the constructor set 5
```
`pir` (`src/prism/pir/stage_mem.cpp`) checks every static-initialisation
function of the unit. Unless each is proved, `main` is `NEEDS-HARNESS`
naming it. When they are proved, `main` keeps a verdict only if it holds for
arbitrary values of the mutable globals; otherwise `NEEDS-HARNESS`. Doctest:
"pir soundness: dynamic initialisation before main (S8)";
`testdata/pir/static_init_{throw,value,ok}.cpp`; ESBMC tasks
`try_catch/lower-exceptions_static_init_fail`,
`cpp11/constructors/Constructor9-1`.

**S9. An `assume` constrained checks before it in the same block.**
```c
int x = __VERIFIER_nondet_int(); int q = 100 / x; __VERIFIER_assume(x != 0);   /* divides by zero first */
int f(int x) { __builtin_assume(x > 0); return 100 / x; }                      /* false assume */
```
`pir`'s encoder (`src/prism/pir/encode.cpp`) threads a running guard through
each node: an `assume` strengthens it for later statements and outgoing
edges only. `llvm.assume` is a `FUNC-CONTRACT` check before it is assumed.
The guard is proved equal to sequential semantics in Lean
(`proofs/techniques/PrismTechniques/LoopCut.lean`: `encViol_iff`,
`encExit_iff`; `s9_old_encoding_misses` is this program under the old
encoding). Doctest: "pir: a false llvm.assume is a checked violation, not a
silent path cut (refinement finding 8)".

### False alarms

**F7. Shift rules follow the language and standard of the unit.** A C file
or header uses C11 6.5.7p4 (a negative left operand, or `E1 × 2^E2` not
representable, is undefined). C++11 to C++17 define `E1 << E2` for
`E1 >= 0` when the result fits the unsigned type (CWG 1457), so `1 << 31` is
defined and `3 << 31`, `-1 << 1` are not. C++20 defines every signed left
shift with an in-range count (P1236). The standard is the unit's `-std=` from
`compile_commands.json` (`with_cxx_std`); without one, `gnu++17`. Doctests:
"pir shift rules follow the C++ standard of the unit (F7)" and "with_cxx_std
reads -std from compile_commands.json". Task: `cxx/cxx_shift_cpp20_true`.
**Open:** under `-std=c++03`, `1 << 31` is undefined, but clang's
`shift-base` check applies the C++11 rule in every C++ mode, so `pir`
proves it. The doctest records this as a `WARN`, not a `CHECK`.

**F8. Zero-initialised large global.** `struct uint3 a[1024];
assert(a[0].x == 0);` in `main` was `FAILED` (ESBMC `bug_fixes/1006_aggregate`).
Only a large global whose initialiser needs more than 256 non-zero stores is
havocked (`MemTr::init_stores` in `src/prism/pir/translate_mem.cpp`); the zero
fill is one memory entry whatever the size. The over-approximation left can
only cost proofs. Doctest: "pir mem: large zero-initialised globals keep
their contents (F8; …)" in `tests/cpp/test_pir_mem.cpp`. Tasks:
`regress/global_zero_table_{true,false}`.

**F9. `abort()` after a failed allocation.** PRISM's `malloc` model may
return NULL, so `if (!buf) abort();` is reachable (ESBMC
`cpp/github_6464_placement_new_incremental`). `abort()` stays a
`FUNC-CONTRACT` violation, except on an execution where a library allocation
already failed: there it is the program's out-of-memory handling. The
`malloc`/`calloc`/`realloc` models record a failure in a hidden flag
(`__prism_alloc_failed`), and the `abort` check is "flag is zero".
Unchanged: a dereference of an unchecked NULL result (`PTR-NULL-DEREF`) and
`assert(p)` after `malloc` (`FUNC-CONTRACT`). Doctest: "pir mem: abort()
after a failed allocation is OOM handling, not a defect (F9; …)". Tasks:
`regress/malloc_abort_{true,false}`.

**F10. Function-try-blocks and unmodelled C++ callees.**
`void f(int &x) try { throw 10; } catch (const int &i) { x = i; }` then
`assert(v == 10)` after `f(v)` (ESBMC `try_catch/try-catch_tryblock_08`). The
front end (`src/prism/cparse.cpp`) extracts a function-try-block with every
handler; `pir` models `try`/`catch` (LLVM `invoke` and landing pads) and
proves `main`. A call to an unmodelled C++ callee, which may write through a
reference argument, makes the function never a proof. Doctests: "cparse
extracts a function-try-block with its handlers (F10)", "pir soundness: an
unmodelled C++ callee may write reference arguments (F10)". Tasks:
`regress/fn_try_block_{true,false}`.

**F11. Typeinfo and exceptions of class type with bases.**
`typeid(...).name()` and `std::any` read the C++ runtime's typeinfo objects;
a thrown object caught by pointer or through a non-first base, or rethrown
from a nested handler, must be adjusted and kept alive. `pir` models a
declared-only typeinfo object as the Itanium `std::type_info`
`{ vptr, __name }`; the exception header holds the pointer the current
handler binds (the caught base subobject, or the thrown pointer);
`__cxa_begin_catch` / `__cxa_end_catch` / `__cxa_rethrow` follow the Itanium
handler count. A base behind a virtual base has a run-time offset and is
`NEEDS-HARNESS`. Tasks:
`regress/{eh_catch_base_offset,eh_catch_pointer,eh_nested_rethrow,typeid_fundamental,eh_catch_virtual_base}_{true,false}`
(the virtual-base pair is `NEEDS-HARNESS` by design).

**F12. IR the LangRef defines was reported.** `freeze poison`,
`llvm.memcpy(p, p, n)` (clang emits it for a struct assignment that may be a
self-assignment) and `llvm.lifetime.start` after `llvm.lifetime.end` are
defined; `icmp samesign` poison was missed. The fix is in `translate.cpp` and
`translate_mem.cpp` (docs/PROOFS_REFINEMENT.md findings 2, 5, 6, 7). The
`pir` stage compiles with `-fno-builtin-memcpy`, so a C `memcpy` call keeps
C's rule (any overlap is undefined, `d == s` included) through its library
model. Doctest: "pir: LangRef-defined IR is not reported, samesign poison is
(refinement findings 2, 5, 6, 7)". Tasks:
`memory/mem_struct_self_copy_{true,false}` (the `false` task is
`sanitizer_blind`: ASan skips the overlap check when `dst == src`).

### Disputed upstream labels

Two ESBMC labels contradict the C++ standard: `cpp/github_6199_fail`
(`std::string(nullptr, 0)`: an empty range is valid) and
`cpp/github_6588_multidim_fail` (`new int[2][3]()` value-initialises to
zero). PRISM proves both. They are skipped at conversion with the reason
(`ESBMC_DISPUTED` in `src/tools/qa/support/fetch.cpp`), not relabelled.
ESBMC tasks run with `--error-label` are skipped too: a reachable `ERROR:`
label is ESBMC's violation there, and PRISM checks no labels.
