# PRISM conformance: metrics, current numbers, known issues

Roadmap Part 2.7 (conformance suite), 5.5 / 6.2 (random-program testing) and
6.1 (release gate). The suite and its sources are described in
[`tests/conformance/SOURCES.md`](../tests/conformance/SOURCES.md); verdicts
are defined in [VERDICTS.md](VERDICTS.md).

## Metrics

Every task names functions and, for each, whether the property holds
(`true`: no undefined behaviour for any input) or not (`false`: a violation
exists). The scorer (`tools/conformance.py`) runs PRISM with
`--stage inventory,classify,bmc,harness` (plus `pir` whenever `prism
--list-stages` lists it) and scores each verdict stage separately.

| metric | definition | target |
|---|---|---|
| **Soundness** | wrong proofs: `PROVED`, `PROVED-UNBOUNDED`, `PROVED-ASSUMING` or `PROVED-CERTIFIED` on a `false` function | **0** — any wrong proof fails the release gate (exit 1) |
| **Completeness** | `true` functions proved / `true` functions | grows |
| **Bug detection** | `false` functions refuted with `FAILED` **and a counterexample that replays** / `false` functions. Replay: the task is compiled with `-fsanitize=undefined,address -fno-sanitize-recover=all`, the counterexample's arguments are passed to the function (inside `bwrap` when it works) and the sanitizer must fire | grows |
| False alarms | `FAILED` on a `true` function | 0 |
| Law 6 | pointer-parameter tasks (`expect_status: NEEDS-HARNESS`) must get `NEEDS-HARNESS`, never a proof or a refutation | all |

Rules that keep the numbers honest:

- `BOUNDED` is neither a proof nor a refutation (Law 2): it counts toward
  neither completeness nor detection, and never toward soundness.
- A `FAILED` whose counterexample does not replay is reported ("refuted, not
  replayed") but is not counted as detection: the verdict may be right for a
  wrong reason (see F4 below).
- A crashed stage and a function the stage never mentions are listed
  separately (Law 7), not folded into "no answer".
- SV-COMP labels speak for one property (`no-overflow`); a `FAILED` of another
  class on an SV-COMP task is listed as "other property", not as a false
  alarm or a detection. SV-COMP and Juliet counterexamples are not replayed
  (their inputs come from `__VERIFIER_nondet_*` / `rand()`/`stdin`).
- The suite's own labels are validated without PRISM:
  `python tools/conformance.py --self-check` requires every `false` witness to
  trip a sanitizer and every `true` function to survive an edge-value grid
  plus 2000 random inputs (it caught one mislabelled task while the suite was
  written).
- The release gate counts wrong proofs on every stage and every origin.
  `harness` only speaks about pointer functions under `// requires:`; a scalar
  function it does not mention is out of its scope, not missing.

## How to run

```
python tools/conformance.py --self-check                    # labels (needs clang or gcc with sanitizer runtimes)
PRISM_BIN=build/prism python tools/conformance.py           # C++ engine; exit 1 on any wrong proof
python tools/conformance.py                                 # Python engine (frozen oracle)
python tools/conformance.py --fetch-juliet /tmp/juliet      # NIST Juliet 1.3, sha256-checked
PRISM_BIN=build/prism python tools/conformance.py --suite /tmp/juliet/juliet
PRISM_BIN=build/prism python tools/csmith_soundness.py -n 300             # random programs
PRISM_BIN=build/prism python tools/csmith_soundness.py --generator csmith -n 100
```

Outputs: `conformance-out/metrics.json`, `metrics.md`, `results.json` (every
function, stage, verdict, counterexample and replay). `.github/workflows/conformance.yml`
runs the self-check and the suite on every push (gating), and the random
programs and Juliet nightly.

## Current numbers (2026-09-23)

Engine: C++ engine built from `claude/prism-code-checker-x7538r` at
`24a72d40b` (stages `inventory,classify,bmc,harness`; no `pir` stage yet).
The frozen Python engine gives **identical** numbers on the same suite
(same 11 wrong proofs, same 9 false alarms): the bugs below are in the shared
encoding design, so the differential oracle cannot catch them.

Label self-check: 212 functions ok, 6 skipped (pointer parameters), 0 failed.

### In-house suite + SV-COMP (`tests/conformance`, 263 tasks)

| stage | origin | true | false | **wrong proofs** | completeness | detection (replayed cex) | refuted, not replayed | false alarms | BOUNDED | no answer | Law 6 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bmc | in-house | 106 | 106 | **11** | 51/106 (48.1%) | 55/106 (51.9%) | 4 | 8 | 14 | 69 | 6/6 |
| bmc | SV-COMP | 25 | 20 | **0** | 4/25 (16.0%) | n/a (not replayable) | 11/20 | 1 | 0 | 27 | – |
| bmc | all | 131 | 126 | **11** | 55/131 (42.0%) | 55/126 (43.7%) | 15 | 9 | 14 | 96 | 6/6 |
| harness | all | – | – | **0** | – | – | – | 0 | – | – | 6/6 |

`pir`: not present in `prism --list-stages` yet; the scorer picks it up
automatically and reports it as its own row when it lands.

### Concurrency (`tests/conformance/concurrency`, 22 labels, roadmap 2.6)

This suite is scored by the `conc` stage only (docs/CONCURRENCY.md), and
`conc` is scored on it only. The labels are `norace`, `noassert` and
`nodeadlock`, each property-scoped, and the verdict applies to `main`.
`conc` never proves: completeness is 0 by definition, and every true label
is `BOUNDED`. Its counterexamples are schedules, so detection is "refuted,
not replayed".

| stage | true | false | **wrong proofs** | BOUNDED (true) | refuted, not replayed | false alarms | no answer |
|---|---|---|---|---|---|---|---|
| conc | 13 | 9 | **0** | 13/13 | 9/9 | 0 | 0 |

(C++ engine at the conc branch, `K = 2` rounds, default unwind.)

Per category (bmc, in-house): overflow 26/41 proved, 30/41 refuted;
div 8/10, 10/10; shift 8/11, 6/11; unsigned 4/5, 3/5; c23 2/11, 3/11;
C++ 2/14, 6/14; macro 1/3, 1/3; array 0/9, 0/9 (every array task is
`ERROR`, see G1); local pointers 0/2, 0/2 (`NEEDS-HARNESS`); SV-COMP
signedintegeroverflow 1/5 proved, 10/10 refuted; bitvector 0/13, 1/10
(front-end `ERROR`s, see G2); loop-simple 3/3.

### NIST Juliet 1.3 (CWE190/191/369/476/680, flow `_01`, 104 files, 410 functions)

| stage | true | false | **wrong proofs** | completeness | refuted (not replayed) | false alarms | no answer |
|---|---|---|---|---|---|---|---|
| bmc | 306 | 104 | **6** | 190/306 (62.1%) | 17/104 | 1 | 196 |

By CWE (false functions): CWE190 10 refuted / 35 no answer; CWE191 6 refuted,
**2 wrong proofs**; CWE369 **4 wrong proofs**, 8 no answer; CWE476 1 refuted;
CWE680 0 refuted.

### Random programs (`tools/csmith_soundness.py`)

In-house generator, 300 programs, 900 functions, seeds 1–300:

| verdict | functions |
|---|---|
| `PROVED-UNBOUNDED` | 304 |
| `FAILED` | 153 |
| `ERROR` | 136 |
| `UNKNOWN` | 1 |
| stage crashed (whole file lost) | 306 (102 programs) |

- **57 of the 304 proofs are wrong** (18.8%): the function, executed under
  UBSan on the edge grid plus 3000 random inputs, hits undefined behaviour.
  45 of the 57 involve an `unsigned char`/`unsigned short` parameter (S4), 9
  are left shifts (S3), the rest S1/S2.
- 129 of 153 `FAILED` counterexamples replay; 17 `FAILED` functions neither
  replay nor show UB on the grid (suspected false alarms, F1/F2/F4).
- Csmith 2.3 (scalar-only configuration, `--no-safe-math`), 100 programs,
  224 functions: 175 `NEEDS-HARNESS`, 48 `ERROR`, 1 proof (checked, correct).
  Csmith output is outside the subset the current front end models, so the
  in-house generator is the useful one until the Clang front end (Part 2)
  lands.

## Known issues

Every wrong proof found, reduced to a minimal reproducer. Run any of them
with `./build/prism FILE --no-llm --stage inventory,classify,bmc`. None is
fixed here (this change only measures); each needs the same fix in
`src/prism/bmc.cpp` and `prism/bmc.py`.

### Soundness (wrong proofs)

**S1. Signed subtraction overflow is only checked downward.** `bmc.cpp`
`apply_binop` uses `bvsub_no_underflow` alone; `a - b` overflowing upward
(`b` negative) is never checked.
```c
int sub_false(int a, int b) { if (a < 0) return 0; return a - b; }
/* PROVED-UNBOUNDED; a=5, b=-2147483647 overflows */
```
Tasks: `overflow/sub_false`.

**S2. `INT_MIN % -1` is not a property.** `%` adds `mod0` but not the
`divovf` check that `/` has (C11 6.5.5p6 makes it undefined).
```c
int r(int a, int b) { if (b == 0) return 0; return a % b; }   /* PROVED */
```
Tasks: `overflow/mod_intmin_false`.

**S3. Signed left shift: only `1 << 31` is checked.** Left shift of a
negative value and left shift whose result does not fit are not properties
(`shift31` only fires when the left operand is the constant 1).
```c
int a(int x) { if (x < -1000 || x > 1000) return 0; return x << 2; }   /* PROVED, x=-1 is UB */
int b(int x) { if (x < 0 || x > 4) return 0; return x << 30; }         /* PROVED, x=2 is UB */
int c(int x) { if (x < 0 || x > 7) return 0; return 2147483647 << x; } /* PROVED, x=1 is UB */
```
Tasks: `shift/shl_negative_false`, `shift/shl_overflow_false`; 9 random programs.

**S4. Integer promotions of `unsigned char` / `unsigned short` are wrong.**
They are modelled as 32-bit *unsigned* values ("unsigned params use unsigned
compares and wrap", PLAN.md) instead of values in 0..255 / 0..65535 promoted
to *signed* `int`. Three consequences:
```c
unsigned m(unsigned short a, unsigned short b) { return a * b; }   /* PROVED; 65535*65535 overflows int */
int s(unsigned char c) { return c << 24; }                         /* PROVED; 255<<24 overflows int */
int add(int a, unsigned char b) { return a + b; }                  /* PROVED; INT_MAX+1 */
int v(unsigned char c, int d) {                                    /* PROVED (vacuous): c < -10 is */
    if (c < -10 || c > 100) return 0;                              /* taken as an unsigned compare, */
    return 20 / d;                                                 /* so every path "returns 0";    */
}                                                                  /* d=0 divides by zero           */
```
Tasks: `overflow/ushort_mul_false`, `overflow/mixed_sign_false`,
`shift/char_shift_false`; 45 of 57 random-program wrong proofs. The same
root cause gives false alarms on `signed char`/`short` (F2).

**S5. `int >> unsigned` is a logical shift.** The signedness of a shift is
taken from either operand (`u = ua || ub`); C takes it from the promoted left
operand, so `-8 >> 1u` is modelled as `0x7FFFFFFC`.
```c
int f(int a, unsigned s) { if (s < 1 || s > 7) return 0; return (a >> s) - 2147483647; }
/* PROVED; a=-8, s=1 gives -4 - INT_MAX */
```
Tasks: `shift/shift_by_unsigned_false`.

**S6. Unmodelled calls and unexpanded macros are treated as UB-free, and
unknown identifiers as a constant.** There is no preprocessor: `abs`, a
template call, a function-like macro, or a call whose arguments contain the
defect are proved; an unexpanded `INT_MIN` is a constant that makes guards
vacuous.
```c
#include <stdlib.h>
int a(int x) { return abs(x); }                            /* PROVED; abs(INT_MIN) is UB */
#define SQ(x) ((x) * (x))
int b(int x) { return SQ(x); }                             /* PROVED; x=46341 overflows */
void sink(int);
int c(int d) { sink(100 / d); return 0; }                  /* PROVED; d=0 (call arguments are not checked) */
#include <limits.h>
int e(void) { int v = INT_MIN; if (v < 0) return v * 2; return 0; }  /* PROVED; INT_MIN*2 */
```
```c++
template <typename T> T twice(T x) { return x + x; }
int f(int a) { return twice<int>(a); }                     /* PROVED; a=1500000000 */
```
Tasks: `overflow/abs_libc_false`, `macro/fn_macro_false`,
`cxx/cxx_template_false`; Juliet CWE369 `int_zero_divide`/`modulo`,
`int_rand_divide`/`modulo` (`printIntLine(100 / data)`), CWE191
`int_min_multiply`, `int_rand_multiply`.
Fix direction: an unmodelled call or unknown identifier must make the
function `NEEDS-HARNESS`/`ERROR` (as unencoded libc calls already do), and
call arguments must be evaluated for UB.

### Robustness (no verdict where one was possible)

**R1. One Z3 sort error kills the bmc stage for the whole file** (Law 7 is
kept: the stage is `failed` with the message, but every function in the file
loses its verdict). 102 of 300 random programs.
```c
int w(long long p0) { unsigned v0 = 12; v0 &= (p0 + (~p0)); return (int)v0; }
int innocent(int a) { if (a < 0 || a > 10) return 0; return a * 2; }  /* also lost */
/* bmc failed: Argument ((_ sign_extend 32) (bvnot p0)) ... has sort (_ BitVec 96) */
```

**R2. Functions never reported** (not in `classify`/`bmc` at all):
`[[nodiscard]] int f(...)` (C23 attribute before the declaration) and
`auto f(int a) -> int` (trailing return type). Tasks: `c23/c23_attr_*`,
`cxx/cxx_auto_*`. Law 7 wants these listed as not checked.

### False alarms and right-for-the-wrong-reason refutations

**F1. Both arms of `?:` are checked unconditionally.**
```c
unsigned f(unsigned a, unsigned b) { return b ? a / b : 0; }   /* FAILED div0, b=0 */
```
Tasks: `div/udiv0_true`, `cxx/cxx_div_true`; `c23/c23_bool_false` and
`cxx/cxx_div_false` are refuted with counterexamples that do not replay.

**F2. `short`/`signed char` parameters range over all 32-bit values**, so
`short + short` "overflows" (`overflow/short_promote_true`,
`char_promote_true`; `short_promote_false` is refuted with a non-replaying
counterexample).

**F3. A static helper's properties are reported on its caller without the
caller's guards** (`overflow/helper_call_true`: `a` in [-1000,1000],
`twice(a)`, FAILED with `a=-4`).

**F4. `long` is 32 bits** in the encoder (LP64 Linux is the primary platform,
D7): `overflow/long_mul_false` is refuted with `a=0x56127fff`, which does not
overflow a 64-bit `long`.

**F5. Integer literal types.** `-2147483648 - 1` in a `long` context is
flagged (SV-COMP `NoNegativeIntegerConstant`, true).

**F6. Macros/enum constants as free values**: `a == INT_MAX` guards
(`macro/limits_true`) and `enum : unsigned char` constants
(`c23/c23_enum_fixed_true`).

**F7. C++20 shift semantics.** `1 << 31` is well defined in C++20 but flagged
(`cxx/cxx_shift_cpp20_true`); `shift31` should be C-only.

### Coverage gaps (honest `ERROR` / `NEEDS-HARNESS`, costing completeness)

- G1. Local arrays with initialisers (`int a[4] = {0};`), 2-D arrays and
  globals indexed with `u`-suffixed constants: front-end `ERROR` on all 18
  array tasks.
- G2. Declarations of several variables in one statement (`int x, y;`),
  `L`/`LL`/`U` literal suffixes, binary literals, digit separators,
  `unsigned x = __VERIFIER_nondet_uint()`: front-end `ERROR` (most SV-COMP
  bitvector/loop-invgen tasks).
- G3. Loops whose bound is a parameter end `BOUNDED` (k-induction step open):
  14 in-house tasks.
- G4. `NEEDS-HARNESS` by design today: recursion, lambdas, range-for,
  `std::array`, structured bindings, `typeof`, `nullptr`, `_BitInt`, `ckd_add`
  (address-of), local pointers.

## Release gate

`tools/conformance.py` exits 1 while any wrong proof exists, and
`.github/workflows/conformance.yml` runs it on every push, so **the gate is
currently red: 11 wrong proofs in the suite, 6 in Juliet, 57 in the random
campaign**. That is the intended state until S1–S6 are fixed; the gate must
not be relaxed to make it green.
