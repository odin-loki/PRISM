# SV-COMP readiness

Roadmap 6.3: "Enter SV-COMP once the Clang pipeline is stable." PRISM has
**not** entered SV-COMP. This file describes the pieces built so far, the
local score they give on the pinned SV-COMP subset in this repository, and
what is still missing for a real entry. The score below is computed by this
repository's own scripts on 45 (no-overflow) and 20 (unreach-call) tasks; it
is not an SV-COMP result. PRISM's violation witnesses for that subset have
been run through two format-2.0 validators, CPAchecker 4.2.2 and UAutomizer
0.3.1 (see "Witness validation" below).

## Pieces

| file | what it is |
|---|---|
| `tools/svcomp/prism.py` | BenchExec tool-info module (`BaseTool2`). Loads as `benchexec.tools.prism` once copied into BenchExec, or from this directory (`PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism --tool-directory tools/svcomp --no-container` passes BenchExec's own tool-info check). |
| `tools/svcomp/prism_svcomp.py` | the executable the module runs: runs PRISM on one task, maps `report.json` to an answer, replays the counterexample, writes the witness |
| `tools/svcomp/witness.py` | violation witnesses in the SV-COMP witness format 2.0 (YAML) |
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
  Covering stages: `bmc` and `pir` for `no-overflow`; only `pir` for
  `unreach-call` (the `pir` stage checks reachability of `reach_error` /
  `__assert_fail` as a property; `bmc` treats those calls as the end of a
  path, not as a property); none for `valid-memsafety`
  (the `pir` memory model checks dereferences and frees but no stage
  encodes `valid-memtrack`, memory leaks), so `valid-memsafety` is never
  `true`.
  `PROVED-ASSUMING` and `BOUNDED` are never `true` (Law 2).
- **`false(...)`** only when a verdict stage reports `FAILED` of the
  property's class (`INT-SIGNED-OVF` for no-overflow; `FUNC-CONTRACT` with
  `prop` `reach_error`/`assert` for unreach-call; `MEM-OOB-*` /
  `PTR-NULL-DEREF` for valid-memsafety) **and the counterexample replays**:
  the task is compiled with clang or gcc (`-fsanitize=signed-integer-overflow`
  for no-overflow, `-fsanitize=address` for memory safety), given the
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
  at the debug location `pir` reports for it (`extra["nondet_loc"]`, checked
  against the task text: the call must start exactly there), else at the
  function's only call site in the task. A call that is neither ends the
  waypoint list (a waypoint at the wrong call would make the witness wrong, a
  missing one only weaker). Debug locations are ignored for a task with line
  markers (`# 12 "file.c"`), whose debug lines name another file's lines.
  When both `bmc` and `pir` refute, the refutation with call locations is
  replayed first.
- `target`: for unreach-call, the `reach_error()` call; for no-overflow, the
  statement or full expression holding the operator UBSan reported. A
  statement that is the first on its line gets no column (format 2.0 then
  means the first statement or full expression in that line); a later one
  gets its start column; a `for` header or a statement continued from an
  earlier line keeps UBSan's column. The target location has no `function`
  field (the violation need not be in `main`).

## Results (local)

Run on 2026-09-23 with the C++ engine built from `claude/svcomp-pir-nondet`
(binary SHA-256 prefix `0f71cc72fbe4`), clang 18.1.3, bubblewrap available,
BenchExec installed (so the answers went through the tool-info module's
`determine_result`), on a shared machine at load ~15:

```
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --jobs 2
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --property unreach-call --jobs 2
```

Scoring as in SV-COMP: correct `true` +2, correct `false` +1, incorrect
`true` −32, incorrect `false` −16, `unknown` 0. SV-COMP only awards the
points of a `false` answer whose witness a validator confirms; every
witness of the `false` answers below was confirmed by CPAchecker 4.2.2
(see "Witness validation"). `true` answers carry no correctness witness
(item 2 below), so their points are still an upper bound.

| property | tasks (true / false) | score | max | correct true | correct false | incorrect | unknown |
|---|---|---|---|---|---|---|---|
| no-overflow | 45 (25 / 20) | **37** | 70 | 11 | 15 | **0** | 19 |
| unreach-call | 20 (16 / 4) | **13** | 36 | 5 | 3 | **0** | 12 |

The previous run (before `pir` reported nondet values) scored 35 and 9.
The same numbers came out of three reruns of this branch (37 / 13 each).

Where the points moved:

- unreach-call: `byte_add-1` and `id_trans` (`false`) were refuted by `pir`
  but could not be replayed for lack of nondet values. `pir` now reports
  them (`extra["nondet"]`, with call sites in `extra["nondet_loc"]`); both
  replay (`reach_error()` aborts the program) and answer
  `false(unreach-call)`.
- `gcd_1` (`true`, both properties) is `pir: PROVED` in these runs; it was
  `UNKNOWN`/`BOUNDED` (solver timeout under load) in the previous one. Not
  a change of this branch.

Where the points are still lost (no-overflow):

- `modulus-1`, `byte_add-2`, `byte_add_1-2`, `byte_add_2-1` (`false`): the
  refutation is `INT-SHIFT-UB` (a left shift into or past the sign bit), not
  `INT-SIGNED-OVF`; the mapping only accepts the overflow class for `false`
  and replay only runs `-fsanitize=signed-integer-overflow`.
- `jain_5-1` (`false`) and the `true` tasks `gcd_2`, `jain_1-1`, `jain_2-1`,
  `jain_5-2`, `num_conversion_1`, `parity`, `sum02-1`, `half_2`, `nested6`
  are `BOUNDED` (loops not closed within unwind 8); `large_const` is
  `NEEDS-HARNESS`; `interleave_bits` is an ILP32 task using `sizeof`.
- `byte_add-1`, `modulus-2`, `id_trans` (`true`): the `pir` stage stops at a
  reachable `assert`/`abort` failure; a `FAILED` of another property says
  nothing about overflow on the other paths.

unreach-call: `sum02-1` (`false`) is `BOUNDED`; the other 3 `false` tasks
answer `false(unreach-call)`.

## Witness validation

Every `witness.yml` of the runs above (15 no-overflow, 3 unreach-call) was
given to two SV-COMP validators that read format 2.0. Both were downloaded
through this environment's proxy and run on the task file the witness names,
with the task's data model:

- **CPAchecker 4.2.2** (`CPAchecker-4.2.2-unix.zip` from
  cpachecker.sosy-lab.org, OpenJDK 21):
  `bin/cpachecker --witnessValidation --witness witness.yml --spec PROP.prp --32|--64 --timelimit 300s TASK.i`.
  Confirmed = `Verification result: FALSE`.
- **UAutomizer 0.3.1** (`UltimateAutomizer-linux.zip` of the
  `ultimate-pa/ultimate` GitHub release `v0.3.1`; `--ultversion`:
  `0.3.1-dev-35a8436538`):
  `python3 Ultimate.py --spec PROP.prp --architecture 32bit|64bit --file TASK.i --validate witness.yml --witness-type violation_witness`.
  Confirmed = result `FALSE(...)`.

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

CPAchecker confirmed 18 of 18, UAutomizer 16 of 18. In SV-COMP a witness
counts when a validator confirms it, so all 18 `false` answers would score.

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
   order; `pir` also reports each call's debug location. Calls inside
   PRISM's own library models (for example `malloc` failing) are not in the
   list, so a replay can take another path than the model did (the replay
   then fails and the answer stays `unknown`). `bmc` reports no locations,
   so for its refutations the waypoint list stops at the first function
   called from more than one site. Floating-point nondet values are
   reported by `pir` but not yet exercised by any task in the subset.
   Witness validation ran locally with default validator settings, not in
   BenchExec with the competition's validator configuration and limits.
2. **Correctness witnesses.** PRISM writes no witness for `true` answers.
   Format 2.0 correctness witnesses are sets of loop invariants
   (`invariant_set`); PRISM's k-induction and Houdini invariants would be the
   source, but they are not exported.
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
   tool-info module merged into BenchExec, a benchmark definition, and the
   registration the rules ask for (the fm-tools metadata and an archived
   release). None of these exist yet.
6. **A version string.** `prism` has no `--version`; the wrapper reports
   `0.1.0+sha256.<first 12 hex digits of the binary>` instead.
7. **Scale.** Only the pinned subset (45 no-overflow and 20 unreach-call
   tasks) has been run. The competition categories are hundreds to
   thousands of tasks, with
   CPU-time and memory limits per task that PRISM has not been measured
   against.

## Findings for the engines (not fixed here)

- The `pir` stage takes only `.c`/`.cpp` units (`is_unit` in
  `src/prism/pir/stage.cpp`). On a `.i` file it reports status `ok` with no
  finding at all, which reads as "nothing to say" rather than "not run"
  (Law 7). The wrapper works around it by copying `.i` to `.c`.
- A `FAILED` counterexample for a function without parameters is just
  `<prop>=sat` (for example `ovf+=sat`); the nondet inputs of such a
  refutation are in `extra["nondet"]` instead (item 1).
- A left shift into or past the sign bit is `INT-SHIFT-UB`, which the
  mapping does not count as `no-overflow` (4 `false` tasks of the subset are
  lost to it; see "Results").
