# SV-COMP readiness

Roadmap 6.3: "Enter SV-COMP once the Clang pipeline is stable." PRISM has
**not** entered SV-COMP. This file describes the pieces built so far, the
local score they give on the pinned SV-COMP subset in this repository, and
what is still missing for a real entry. The scores below are computed by this
repository's own scripts, first on 45 (no-overflow) and 20 (unreach-call)
tasks, since 2026-09-25 on 117 and 127 tasks (174 task files, see
"2026-09-25" below); they are not SV-COMP results. PRISM's violation and correctness witnesses for
that subset have been run through two format-2.0 validators, CPAchecker
4.2.2 and UAutomizer 0.3.1 (see "Witness validation" below), and the subset
has been run in BenchExec with the competition's resource limits (see
"BenchExec").

## Pieces

| file | what it is |
|---|---|
| `tools/svcomp/prism.py` | BenchExec tool-info module (`BaseTool2`). Loads as `benchexec.tools.prism` once copied into BenchExec, or from this directory (`PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism --tool-directory tools/svcomp --no-container` passes BenchExec's own tool-info check). |
| `tools/svcomp/prism_svcomp.py` | the executable the module runs: runs PRISM on one task, maps `report.json` to an answer, replays the counterexample, writes the witness |
| `tools/svcomp/witness.py` | violation and correctness witnesses in the SV-COMP witness format 2.0 (YAML) |
| `tools/svcomp/prism-subset.xml` | BenchExec benchmark definition for the subset (SV-COMP limits: 15 min CPU, 15 GB, 4 cores) |
| `tools/svcomp/run_subset.py` | runs the pinned subset through the module's `cmdline` / `determine_result` and scores it |
| `tests/conformance/sv-comp/properties/` | the upstream property files (`no-overflow.prp`, `unreach-call.prp`, `valid-memsafety.prp`) at the pinned sv-benchmarks commit |
| `tests/test_svcomp.py` | mapping, scoring, witness shape, replay, tool-info and end-to-end tests |

## What PRISM runs

```
prism TASK.c --no-llm --stage inventory,classify,bmc,pir --out DIR
```

A `.i` task is copied to `.c` first, because the `pir` stage only takes
`.c`/`.cpp` units (see "Findings" below). The data model comes from the task
(`--data-model`); PRISM's encoders are LP64.

## Mapping report.json to an SV-COMP answer

The mapping may never turn a weaker verdict into a stronger answer; it
follows the verdict laws in [VERDICTS.md](VERDICTS.md).

- **`true`** only when a verdict stage that covers the property reports
  `PROVED`, `PROVED-UNBOUNDED` or `PROVED-CERTIFIED` for `main`, and no
  verdict stage reports a `FAILED` of the property's class for `main`.
  Covering stages: `bmc` and `pir` for `no-overflow` and for
  `unreach-call` (both stages check a reachable `reach_error()` /
  `__VERIFIER_error()` call as a `FUNC-CONTRACT` property named
  `reach_error`, and a canonical `__VERIFIER_assert` as `assert`; see
  "bmc and unreach-call"); none for `valid-memsafety`
  (the `pir` memory model checks dereferences and frees but no stage
  encodes `valid-memtrack`, memory leaks), so `valid-memsafety` is never
  `true`.
  `PROVED-ASSUMING` and `BOUNDED` are never `true` (Law 2).
- **`false(...)`** only when a verdict stage reports `FAILED` of the
  property's class (`INT-SIGNED-OVF`, or an `INT-SHIFT-UB` whose check is a
  signed left shift overflow — `pir` `shift-base`, `bmc` `shift31` — for
  no-overflow, see "Shifts and no-overflow"; `FUNC-CONTRACT` whose check
  is `reach_error`/`assert` for unreach-call (`pir`: `extra["prop"]`,
  `bmc`: the message prefix `reach_error:`); `MEM-OOB-*` /
  `PTR-NULL-DEREF` for valid-memsafety) **and the counterexample replays**:
  the task is compiled with clang or gcc
  (`-fsanitize=signed-integer-overflow,shift-base` for no-overflow, where
  a shift must report "left shift of N by M places cannot be represented";
  a negative left operand does not count), `-fsanitize=address` for memory safety), given the
  counterexample's `__VERIFIER_nondet_*` values through generated stubs, run
  in bubblewrap with rlimits, and the sanitizer (or `reach_error`) must fire.
  Replay executes task code, so it needs `--allow-exec` (Law 9); the
  benchmark definition must pass it.
- **`unknown`** otherwise: `NEEDS-HARNESS`, `BOUNDED`, `UNKNOWN`,
  `TIMEOUT`, `ERROR`, a stage that crashed, a refutation that does not
  replay, an ILP32 task whose function bodies use width-dependent types
  (`long`, `size_t`, `sizeof`, or a typedef or struct built from them), and
  every other property.

A `false` answer comes with `witness.yml`: one `violation_sequence` entry,
a `function_return` waypoint per nondet value (when the engine reports them)
and a `target` waypoint.

- `function_return`: constraint `\result == <value>` with format
  `acsl_expression` (format 2.0 allows only ACSL `\result <op> <constant>`
  there), located at the closing parenthesis of the call. The call is found
  at the location the engine reports for it (`extra["nondet_loc"]`, checked
  against the task text: the call must start exactly there), else at the
  function's only call site in the task. A call that is neither ends the
  waypoint list (a waypoint at the wrong call would make the witness wrong, a
  missing one only weaker). `pir` reports debug locations, which are ignored
  for a task with line markers (`# 12 "file.c"`: debug lines then name
  another file's lines); `bmc` reports physical positions in the analysed
  text (`extra["nondet_loc_kind"] = "physical"`), which are kept. When both
  stages refute, a refutation with call locations is replayed first, `pir`'s
  before `bmc`'s.
- `target`: for unreach-call, the `reach_error()` call; for no-overflow, the
  statement or full expression holding the operator UBSan reported. A
  statement that is the first on its line gets no column (format 2.0 then
  means the first statement or full expression in that line); a later one
  gets its start column; a `for` header or a statement continued from an
  earlier line keeps UBSan's column. The target location has no `function`
  field (the violation need not be in `main`).

A `true` answer comes with a correctness witness: one `invariant_set`
entry (format 2.0). It carries only invariants the engine proved:

- `bmc` `PROVED-UNBOUNDED` with `k_induction = closed-invariants`: the
  Houdini-filtered loop invariants (`extra["invariants"]`, one list per cut
  loop) at each loop's keyword (`extra["invariant_loops"]`: kind, line,
  column of `for`/`while`/`do`, computed from the function body's source
  position, `FunctionInfo::body_line`/`body_col`; 0 when the body was
  rewritten by inlining). They are `loop_invariant`s: Houdini checks each
  one after the loop's init and after every body+increment, i.e. wherever
  the loop condition is about to be evaluated, which is what format 2.0
  asks. Left out: `do` loops (proved at the top of the body instead), loops
  whose keyword is not at the reported position in the task, conjuncts over
  a name declared in a `for` init (not in scope at the keyword), and
  conjuncts with `+ - *` unless constant bounds among the exported
  comparisons keep every subterm within `int` (the engine proves them over
  wrapping bit-vectors; in C an overflow there would be UB). Dropping a
  conjunct keeps the witness valid, it only helps the validator less.
- `pir` `PROVED-UNBOUNDED` with `k_induction = closed-invariants`: the
  surviving Houdini invariants of the function's own loops
  (`extra["invariant_conjuncts"]`, structured, over IR value names, and
  `extra["invariant_loops"]`, the loop start from clang's `llvm.loop`
  metadata). The wrapper compiles the task once more with `-g` and maps a
  value to a C variable only when certain at the loop head (one variable,
  unique name, in scope, no other assignment on the way from its definition
  to the header), and renders a conjunct only where C means the proved
  bit-vector relation (signedness, promotion, wrap-around). Tasks with line
  markers get none.
- every other proof (`pir` PROVED with loops closed within the unwind, loop
  free, plain k-induction of `pir` or `bmc`): neither stage exports a loop
  invariant for these (k-induction proves the step without one), so the
  witness is the empty invariant set, which is trivially valid and leaves
  the proof to the validator. No invariant is ever guessed.

## Shifts and no-overflow

The SV-COMP rules define `no-overflow` as: "It can never happen that the
resulting type of an operation is a signed-integer type but the resulting
value is not in the range of values that are representable by that type. A
violation of this property matches what C11 defines as undefined behavior.
(Hence, conversions to signed-integer types do not violate this property.)"
(sv-comp.sosy-lab.org, rules page, read 2026-09-23.) A signed `E1 << E2`
with `E1 >= 0` whose value `E1 × 2^E2` does not fit is such an operation
(C11 6.5.7p4 makes it undefined). The pinned tasks agree: `byte_add-1`
(`true`) and `byte_add-2` (`false`) differ only in
`(unsigned int)r3 << 24U` versus `r3 << 24U`. A negative or too large
shift count and a negative left operand are undefined too, but their
result is not an out-of-range value, so the wrapper does not answer
`false(no-overflow)` for them (they stay `unknown`). PRISM reports all of
these as `INT-SHIFT-UB`; the mapping accepts the checks that can be a shift
overflow (`pir` `shift-base`, which also fires for a negative base; `bmc`
`shift31`), and the replay decides: only UBSan's `shift-base` report "left
shift of N by M places cannot be represented" counts.

## goto

`loop-invgen/nested6` has a `goto END` out of an `if`, which the `bmc`
front end used to reject (`ERROR`, "goto unencoded"). Both engines' `bmc`
now encode structured gotos (`src/prism/bmc_encoder.inc` `goto_stmt` /
`label_stmt`, `prism/bmc.py` `_goto` / `_label`):

- **Forward, out of blocks:** `goto L` keeps its state until `L:`, which
  must label a later statement of the goto's own statement list or of an
  enclosing one (a jump out of nested blocks, loops and switches, as in
  nested6, or ahead in the same list). The state is merged at the label
  like a `break` at the end of a loop. A jump past the declaration of a
  name the label can see is refused (the name is indeterminate there).
- **Backward, forming a loop:** when `goto L` follows `L:` in the same list
  (at any depth inside the later statements), the statements from `L:` to
  the last one containing `goto L` are the body of a loop whose `goto L` is
  a `continue`: unwound like every other loop (a pass still jumping back
  after the unwind bound makes the result `BOUNDED`), and havocked in the
  k-induction step like `Parser::loop_havoc`.
- **Anything else** (a jump into a block, into an `else` branch, into a
  later switch arm past a `break`, a label never reached) is `NEEDS-HARNESS`
  with "unstructured goto unencoded", never `ERROR` and never a guess. A
  static callee that contains `goto` is not inlined (its labels would be
  copied per call site).

Tests: `testdata/goto_structured.c` (both engines: forward out of nested
loops, backward loops that close, an unbounded backward loop proved by
k-induction, a failure beyond the unwind bound that stays `BOUNDED`, an
uninitialised read on the goto path, the two unstructured shapes),
`tests/test_bmc_goto_shift.py`, doctest "bmc goto: ...".

nested6 itself does not change its answer: `bmc` is now `NEEDS-HARNESS`
(its `main` calls the task's own `__VERIFIER_assert`, which is not
modelled) instead of `ERROR`, and `pir` is still `BOUNDED`, so both
properties stay `unknown` (re-run 2026-09-24 with `--only nested6`; the
subset score is unchanged).

## bmc and unreach-call

Until 2026-09-25 `bmc` treated `reach_error()` and `__VERIFIER_error()` like
`abort()`: the path ended there and nothing was reported, so a `bmc` proof
said nothing about unreach-call. `loop-simple/deep-nested` (expected
`false`) shows why that mattered: the old `bmc` reported
`PROVED-UNBOUNDED` for it, which the mapping correctly ignored for
unreach-call.

Now both engines' `bmc` encode such a call as a property (`bmc_encoder.inc`
`model_call`, `prism/bmc.py` `_model_call`): reaching it is a violation, a
`FAILED` `FUNC-CONTRACT` with the check name `reach_error` (message
`reach_error: FUNC-CONTRACT`), as in `pir`; the path still ends there.
A static definition of the error function in the unit is never inlined over
the call (`inline.cpp` / `prism/inline.py` `inlineable_callee`), so an empty
`static void reach_error(void) {}` cannot make the violation disappear.
`abort()`, `exit()` and `__assert_fail()` still only end the path (SV-COMP's
unreach-call is about `reach_error` alone). The Python engine also gained
the canonical `__VERIFIER_assert` rewrite the C++ `bmc` got in the
invariants round (`_canonical_verifier_assert`, `_rewrite_verifier_assert`),
so both engines give the same verdicts (`tests/test_bmc_reach_error.py`,
doctest "bmc: a reachable reach_error() call ...").

A `bmc` proof of `main` now covers unreach-call: every call of the error
function reachable from `main` is either encoded (inlined static callees,
the canonical `__VERIFIER_assert`) or sits in an unmodelled callee, which
already makes the verdict "not a proof". A `bmc` refutation counts when it
replays like any other (`reach_error()` must be called in the replay). On
`deep-nested` the new `bmc` reports `BOUNDED` (the call is reachable only
after about 2^32 iterations, far beyond the unwind bound, and the
k-induction step, which closed before, is now open on the `reach_error`
property), so the answer stays `unknown`.

The cost is on no-overflow: a task whose `reach_error()` is reachable now
gets `FAILED` (of `FUNC-CONTRACT`) from `bmc` instead of a proof of the
other properties, exactly as `pir` already did. In the subset that is
`loop-simple/nested_1b` (expected `true` for no-overflow, `false` for
unreach-call): no-overflow `true` → `unknown` (−2), unreach-call unchanged.

## Results (local)

Run on 2026-09-24 with the C++ engine built from `claude/svcomp-2`
(binary SHA-256 prefix `4cab37bffd21`), clang 18.1.3, bubblewrap available,
BenchExec 3.35 installed (so the answers went through the tool-info
module's `determine_result`), on a shared machine:

```
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --jobs 2
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --property unreach-call --jobs 2
```

Scoring as in SV-COMP: correct `true` +2, correct `false` +1, incorrect
`true` −32, incorrect `false` −16, `unknown` 0. SV-COMP awards the points
of an answer only when a validator confirms its witness; every witness of
the answers below (38: 16 correctness, 22 violation) was confirmed by
CPAchecker 4.2.2, and 30 of them also by UAutomizer 0.3.1 (see "Witness
validation"), so these are the points the rules would give.

| property | tasks (true / false) | score | max | correct true | correct false | incorrect | unknown |
|---|---|---|---|---|---|---|---|
| no-overflow | 45 (25 / 20) | **41** | 70 | 11 | 19 | **0** | 15 |
| unreach-call | 20 (16 / 4) | **13** | 36 | 5 | 3 | **0** | 12 |

The previous branch scored 37 and 13, with the same `true` answers but no
correctness witnesses (so those points were an upper bound). Two runs of
this branch (before and after merging the latest main line) gave the same
numbers.

Where the points moved (no-overflow, +4):

- `byte_add-2`, `byte_add_1-2`, `byte_add_2-1` (`pir` `shift-base`) and
  `modulus-1` (`bmc` `shift31`, `1 << 31`) answer `false(no-overflow)`:
  a signed left shift overflow is an overflow under the rules (see "Shifts
  and no-overflow"), and each replays under `-fsanitize=shift-base`
  ("left shift of 1 by 31 places cannot be represented in type 'int'" for
  `modulus-1`). `modulus-1` is a `bmc`-only refutation with two calls of
  `__VERIFIER_nondet_uint()`; its witness has both `function_return`
  waypoints at the exact calls (bmc call sites, see "Mapping"); without
  them the waypoint list would stop before the first call (two call sites,
  no location).

Where the points are still lost (no-overflow):

- `jain_5-1` (`false`) and the `true` tasks `gcd_2`, `jain_1-1`, `jain_2-1`,
  `jain_5-2`, `num_conversion_1`, `parity`, `sum02-1`, `half_2`, `nested6`
  are `BOUNDED` (loops not closed within unwind 8, `pir` k-induction step
  open at k = 1, 2); `bmc` is `NEEDS-HARNESS` on all of them (the task's own
  `__VERIFIER_assert` is not modelled; nested6 was a front-end `ERROR` on
  its `goto` before the goto model, see "goto"), so the Houdini invariants
  that could close them are never tried; `large_const` is `NEEDS-HARNESS`;
  `interleave_bits` is an ILP32 task using `sizeof`.
- `byte_add-1`, `modulus-2`, `id_trans` (`true`): the `pir` stage stops at a
  reachable `assert`/`abort` failure; a `FAILED` of another property says
  nothing about overflow on the other paths.

unreach-call: `sum02-1` (`false`) is `BOUNDED`; the other 3 `false` tasks
answer `false(unreach-call)`.

### 2026-09-24: loop invariants (branch `claude/invariants`)

`pir` loop invariants (docs/PIR.md "Loop invariants") and `bmc` checking a
canonical `__VERIFIER_assert` call as `assert((int)(E))`: no-overflow
**51**/70 (16 correct true, 19 correct false, 0 incorrect), unreach-call
**21**/36 (9 / 3 / 0). Newly `true`: `jain_1-1`, `jain_2-1`, `jain_5-2`,
`nested6` (pir, both properties; their witnesses carry 5, 10, 10 and 15
invariant conjuncts, e.g. `(y & 1) == 1` and `k == n`) and `num_conversion_1`
(bmc, no-overflow). Validation of the no-overflow correctness witnesses:
31 of 32 runs confirmed (CPAchecker 4.2.2 and UAutomizer 0.3.1 confirm the
four invariant-carrying witnesses except CPAchecker on `nested6`, a
timeout); the unreach-call validation did not finish before this report.
`bmc` now reaches `BOUNDED` with Houdini on `jain_5-2`, `half_2` & co.
(its invariants do not close them). A `bmc` proof is still not used for
unreach-call (a direct `reach_error()` outside `__VERIFIER_assert` ends the
path in `bmc` instead of being reported).

## BenchExec

The same subset was run in BenchExec 3.35 (`benchexec` with the tool-info
module `tools/svcomp/prism.py` and the benchmark definition
`tools/svcomp/prism-subset.xml`), inside BenchExec's container. cgroups (v1)
are available in this environment; the container needs
`--read-only-dir / --overlay-dir /tmp --overlay-dir /home` because the
overlay mount of `/` fails here:

```
PYTHONPATH=tools/svcomp benchexec tools/svcomp/prism-subset.xml --tool-directory DIR \
    --read-only-dir / --overlay-dir /tmp --overlay-dir /home -M 12GB
```

(DIR holds `prism_svcomp.py`, `witness.py` and the `prism` binary.) Limits:
15 min CPU time and 4 cores as in SV-COMP; memory 12 GB instead of 15 GB,
because this container's cgroup allows only 14.3 GB and BenchExec refuses
a larger limit (the benchmark definition asks for 15 GB). BenchExec needs
every property file a task definition names, so the run used a copy of the
suite with the five property files the repository does not carry
(`coverage-*.prp`, `termination.prp`) added from the pinned sv-benchmarks
commit.

Result: the same answers as above, task for task: no-overflow 11 correct
`true`, 19 correct `false`, 15 `unknown`; unreach-call 5 / 3 / 12; 0
incorrect; BenchExec score 54 of 106 (= 41 + 13). No run came near a
limit: the most CPU time was 52.6 s (`gcd_1`, unreach-call), the most
memory 661 MB; the 65 runs took 151 s of CPU time together. The validators
were not run under BenchExec (they ran as below, with their own time
limits).

**2026-09-25, enlarged subset (244 runs: 117 no-overflow, 127
unreach-call), same limits and command line**, binary of this branch
(SHA-256 prefix `c59b5e977165`), on a machine shared with other jobs (load
average 13–20 on 4 cores): BenchExec score **203** of 417, 88 correct
`true`, 27 correct `false`, 0 incorrect, 129 `unknown`. That is the
`run_subset.py` result (88 + 117 = 205) task for task except
`loop-invgen/apache-escape-absolute.i.v+cfa-reducer` (no-overflow), which
BenchExec ended as `OUT OF MEMORY` at a measured peak of 280 MB, far below
the 12 GB limit (host memory pressure from the other jobs, not the run's
own use); re-run alone in BenchExec it answers `true` for both properties
(35 s and 26 s CPU). The only other limit reached was
`loop-simple/nested_6` (unreach-call, expected `true`): BenchExec killed it
at its wall-time limit (931 s wall, 281 s CPU on the loaded machine); its
six nested constant loops make `bmc` unroll 6^6 iterations with a solver
call each (`pir` stops at its 6000-block budget with `UNKNOWN`), so it is a
timeout at any load. Apart from `nested_6` the most CPU time was 181.6 s
(`loop-invgen/SpamAssassin-loop.i.v+cfa-reducer`, no-overflow) and the
most memory 682 MB (`bitvector/gcd_2`); the 244 runs took 1528 s of CPU
time together.

## Witness validation

Every `witness.yml` of the runs above (30 no-overflow, 8 unreach-call) was
given to two SV-COMP validators that read format 2.0. Both were downloaded
through this environment's proxy and run on the task file the witness names,
with the task's data model and the validators' default configuration:

- **CPAchecker 4.2.2** (`CPAchecker-4.2.2-unix.zip` from
  cpachecker.sosy-lab.org, OpenJDK 21):
  `bin/cpachecker --witnessValidation --witness witness.yml --spec PROP.prp --32|--64 --timelimit 900s TASK.i`.
  Confirmed = `Verification result: FALSE` for a violation witness, `TRUE`
  for a correctness witness.
- **UAutomizer 0.3.1** (`UltimateAutomizer-linux.zip` of the
  `ultimate-pa/ultimate` GitHub release `v0.3.1`; `--ultversion`:
  `0.3.1-dev-35a8436538`):
  `python3 Ultimate.py --spec PROP.prp --architecture 32bit|64bit --file TASK.i --validate witness.yml --witness-type violation_witness|correctness_witness`.
  Confirmed = result `FALSE(...)` for a violation witness, `TRUE` for a
  correctness witness. Killed after 1020 s wall time (the competition
  gives correctness validation 900 s).

Correctness witnesses (`true` answers). All are the empty invariant set:
no proof in the subset exports a loop invariant (see "Mapping").

| task | property | proof | CPAchecker 4.2.2 | UAutomizer 0.3.1 |
|---|---|---|---|---|
| bitvector/byte_add_1-1 | no-overflow | pir PROVED (unwind) | confirmed | confirmed |
| bitvector/byte_add_2-2 | no-overflow | pir PROVED (unwind) | confirmed | confirmed |
| bitvector/gcd_1 | no-overflow | pir PROVED (unwind) | confirmed | confirmed |
| loop-simple/nested_1 | no-overflow | bmc PROVED-UNBOUNDED | confirmed | confirmed |
| loop-simple/nested_1b | no-overflow | bmc PROVED-UNBOUNDED | confirmed | confirmed |
| loop-simple/nested_2 | no-overflow | bmc PROVED-UNBOUNDED | confirmed | confirmed |
| signedintegeroverflow-regression/ConversionToSignedInt | no-overflow | pir PROVED (loop-free) | confirmed | confirmed |
| signedintegeroverflow-regression/IntegerPromotion-1 | no-overflow | pir PROVED (loop-free) | confirmed | confirmed |
| signedintegeroverflow-regression/Multiplication-1 | no-overflow | pir PROVED (loop-free) | confirmed | confirmed |
| signedintegeroverflow-regression/NoNegativeIntegerConstant | no-overflow | pir PROVED (loop-free) | confirmed | confirmed |
| signedintegeroverflow-regression/UsualArithmeticConversions | no-overflow | pir PROVED (loop-free) | confirmed | confirmed |
| bitvector/byte_add_1-1 | unreach-call | pir PROVED (unwind) | confirmed | timeout |
| bitvector/byte_add_2-2 | unreach-call | pir PROVED (unwind) | confirmed | timeout |
| bitvector/gcd_1 | unreach-call | pir PROVED (unwind) | confirmed | timeout |
| loop-simple/nested_1 | unreach-call | pir PROVED (unwind) | confirmed | confirmed |
| loop-simple/nested_2 | unreach-call | pir PROVED (unwind) | confirmed | confirmed |

CPAchecker confirmed 16 of 16, UAutomizer 13 (3 timeouts, no rejection).
An empty invariant set gives the validator nothing to check and nothing to
use: it re-proves the program itself. The validators do check the
invariants they get: for `gcd_1`, a witness with the `loop_invariant` `0`
or `b < 0` at the `while` of `gcd_test` is rejected by CPAchecker ("invalid
invariant") and UAutomizer, one with `1` is confirmed by both.

Houdini invariants: no `true` answer of the subset comes from a
`closed-invariants` proof (see "Results"), so that path was checked on a
small program outside the subset (`n` nondet in `[0, 1000]`,
`while (i < n) { s = s + 2; i = i + 1; }`, no-overflow): `bmc` proves it
`PROVED-UNBOUNDED` with 25 template invariants; the witness carries 13 of
them at the `while` (for example `i <= 1000` and `s == 2 * i`; those with
arithmetic over the unbounded `s` or `n`, such as `s <= 2 * n`, are left
out), and both validators confirm it (CPAchecker in 21 s, UAutomizer in
107 s).

Violation witnesses (`false` answers):

| task | property | waypoints | CPAchecker 4.2.2 | UAutomizer 0.3.1 |
|---|---|---|---|---|
| bitvector/byte_add-1 | unreach-call | 2 function_return + target | confirmed | rejected (function_return not matched) |
| loop-invgen/id_trans | unreach-call | 3 function_return + target | confirmed | rejected (function_return not matched) |
| loop-simple/nested_1b | unreach-call | target | confirmed | confirmed |
| bitvector/jain_1-2 | no-overflow | 1 function_return + target | confirmed | confirmed |
| bitvector/jain_2-2 | no-overflow | 1 function_return + target | confirmed | confirmed |
| bitvector/jain_4-1 | no-overflow | 1 function_return + target | confirmed | confirmed |
| bitvector/jain_6-2 | no-overflow | 1 function_return + target | confirmed | confirmed |
| bitvector/jain_7-1 | no-overflow | 1 function_return + target | confirmed | confirmed |
| bitvector/byte_add-2 | no-overflow | 2 function_return + target | confirmed | rejected (function_return not matched) |
| bitvector/byte_add_1-2 | no-overflow | 1 function_return + target | confirmed | rejected (function_return not matched) |
| bitvector/byte_add_2-1 | no-overflow | 1 function_return + target | confirmed | rejected (function_return not matched) |
| bitvector/modulus-1 | no-overflow | 2 function_return (bmc call sites) + target | confirmed | rejected (function_return not matched) |
| signedintegeroverflow-regression/AdditionIntMax | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/AdditionIntMin | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/Division-1 | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/Multiplication-2 | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/NoConversion | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/PostfixDecrement | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/PostfixIncrement | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/PrefixDecrement | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/PrefixIncrement | no-overflow | target | confirmed | confirmed |
| signedintegeroverflow-regression/UnaryMinus | no-overflow | target | confirmed | confirmed |

CPAchecker confirmed 22 of 22, UAutomizer 17 of 22. In SV-COMP a witness
counts when a validator confirms it, so all 22 `false` answers would score.
The five UAutomizer rejections are all plain `x = __VERIFIER_nondet_*();`
calls (see below).

An intermediate run of this branch showed why the `bmc` trace needs a cut:
`bmc` listed every nondet call whose path guard the model makes true,
including calls encoded after the violated check (`jain_4-1`: three values,
the overflow is in the first loop iteration), and once both stages reported
call locations the `bmc` trace was used; CPAchecker rejected the four
`jain_*` witnesses whose waypoints went past the target. `bmc` now lists
only the calls encoded before the violated check, `pir`'s trace (which
stops at the check) is preferred, and all 22 are confirmed.

Checks that the validators really read the waypoints:

- CPAchecker's log shows the `WitnessAutomaton` in the analysis, and its
  counterexample for `id_trans` has exactly the witness's values
  (`nlen = 0`, `idBitLength = 28`, `material_length = 7`). With one value
  changed (`\result == 3` instead of 28, a run that never reaches
  `reach_error`) CPAchecker answers `TRUE`, i.e. rejects the witness.
- With the `AdditionIntMax` target moved one line down (to the `printf`),
  both validators reject the witness.

The two UAutomizer rejections: its log lists every `function_return`
waypoint of `byte_add-1` and `id_trans` as "unmatched", so it explores no
path at all. Those calls are simple assignments (`a = __VERIFIER_nondet_uint();`);
the `jain_*` calls that it does match sit inside an expression
(`y = y + 2*__VERIFIER_nondet_int();`). UAutomizer 0.3.1 matched none of
the placements tried for the `id_trans` calls (the closing parenthesis as
format 2.0 specifies, the call's start, the statement's start, the `;`, no
column), and it also rejects the format-2.0 example witness of the
sv-witnesses repository (`examples/unsafe-program-example.symbiotic.witness-2.0.yml`,
same kind of calls) with the same "unmatched" message. A witness from
CPAchecker for `id_trans`, which uses `assumption` waypoints on the
assigned variables instead of `function_return`, is confirmed by
UAutomizer. PRISM keeps `function_return` (what format 2.0 defines for a
nondet return value, and what CPAchecker confirms).

Witness format problems the validators showed, fixed on this branch:

- `function_return` constraints were written with format `c_expression`;
  format 2.0 requires `acsl_expression` for `\result <op> <constant>` (the
  schema text; CPAchecker accepted either).
- The no-overflow `target` pointed at the operator UBSan reports
  (`int x = (2147483647 ` **`+`** ` 1) - 23;`). Format 2.0 wants the first
  character of the statement or full expression; UAutomizer rejected 12 of
  15 no-overflow witnesses for it. Now a statement first on its line gets
  no column (see "Mapping" above), which both validators accept. UAutomizer
  does not match the target at a leading `(` of an initializer, only at the
  first operand or without a column.
- The target carried `function: main` although the `reach_error()` call or
  the overflow can be in another function; the field is now left out.

The format linter of the sv-witnesses repository (`witnesslint` 2.2.1-dev,
`--svcomp --expectViolationWitness --expectedWitnessVersion 2.0`) accepts
the witnesses. Its `--strictChecking` mode stops at every `target`
waypoint with "Unknown waypoint type target" (a linter limitation: that
check only knows assumption, branching and function waypoints).

The validators, their downloads and the scripts that ran them are not part
of this repository (the six linked libraries in `third_party/` are the only
vendored code). Re-running needs Java 21 and the two archives above.

## What is missing for an actual entry

1. **Nondet values, remaining gaps.** `bmc` and `pir` report the values of
   the `__VERIFIER_nondet_*` calls a refutation of `main` executes, in call
   order, and each call's location (`pir`: debug location; `bmc`: physical
   position, tagged into the call's name before inlining and unrolling).
   Calls inside PRISM's own library models (for example `malloc` failing)
   are not in the list, so a replay can take another path than the model
   did (the replay then fails and the answer stays `unknown`). A `bmc` call
   site's column is off when a block comment precedes the call on the same
   line (the front end drops the comment delimiters); the wrapper then
   finds no call at that position and falls back to the unique call site.
   Floating-point nondet values are reported by `pir` but not yet exercised
   by any task in the subset. Witness validation ran with default
   validator settings, outside BenchExec.
2. **Correctness witnesses, remaining gaps.** Every `true` answer has one
   (all 16 confirmed by CPAchecker), but in the subset they are all empty:
   `pir`'s k-induction and bounded proofs have no invariant to export, and
   `bmc`'s Houdini invariants (exported at the loop keyword) only reach
   tasks `bmc` can encode. A validator must re-prove the program, which
   UAutomizer did not do within 900 s for three unreach-call tasks.
3. **Other properties.** `unreach-call` is mapped and scored above;
   `valid-memsafety` can only ever answer `false(valid-deref)` /
   `false(valid-free)` from a replayed refutation; `termination`,
   `no-data-race`, `valid-memcleanup` and the coverage properties are
   `unknown`.
4. **ILP32.** The encoders are LP64. ILP32 tasks that use width-dependent
   types are `unknown` instead of being analysed with 32-bit `long` and
   pointers.
5. **Archive and registration.** A competition entry needs, per the current
   rules: a self-contained archive of the tool runnable on the competition
   machines (PRISM binary, the clang/opt it calls for the `pir` stage, the
   wrapper and witness writer), a licence and README in the archive, the
   tool-info module merged into BenchExec, a benchmark definition (a local
   one for the subset is `tools/svcomp/prism-subset.xml`), and the
   registration the rules ask for (the fm-tools metadata and an archived
   release). None of these exist yet.
6. **A version string.** `prism` has no `--version`; the wrapper reports
   `0.1.0+sha256.<first 12 hex digits of the binary>` instead.
7. **Scale.** Only the pinned subset (45 no-overflow and 20 unreach-call
   tasks) has been run, in BenchExec with the competition's CPU-time and
   core limits and 12 GB of memory (see "BenchExec"); the largest run used
   53 s of CPU time and 661 MB. The competition categories are hundreds to
   thousands of tasks.

## Findings for the engines (not fixed here)

- The `pir` stage takes only `.c`/`.cpp` units (`is_unit` in
  `src/prism/pir/stage.cpp`). On a `.i` file it reports status `ok` with no
  finding at all, which reads as "nothing to say" rather than "not run"
  (Law 7). The wrapper works around it by copying `.i` to `.c`.
- A `FAILED` counterexample for a function without parameters is just
  `<prop>=sat` (for example `ovf+=sat`); the nondet inputs of such a
  refutation are in `extra["nondet"]` instead (item 1).
- `INT-SHIFT-UB` covers both a shift overflow (an SV-COMP `no-overflow`
  violation) and a bad shift count or negative base (not one); the wrapper
  tells them apart by the check name (`shift-base` / `shift31`) and the
  UBSan replay. `pir`'s `shift-base` check also fires for a negative base,
  which the replay then rejects.
- `bmc` is `NEEDS-HARNESS` on most SV-COMP `main`s because the task's own
  `__VERIFIER_assert` definition is not inlined (it is not `static`) and
  not modelled, so its Houdini invariants never reach those tasks.
