# SV-COMP readiness

Roadmap 6.3: "Enter SV-COMP once the Clang pipeline is stable." PRISM has
**not** entered SV-COMP. This file describes the pieces built so far, the
local score they give on the pinned SV-COMP subset in this repository, and
what is still missing for a real entry. The score below is computed by this
repository's own scripts on 45 (no-overflow) and 20 (unreach-call) tasks; it
is not an SV-COMP result, and none of PRISM's witnesses has been validated.

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
  (no stage encodes `valid-free` or `valid-memtrack`, and the `pir` stage has
  no memory model yet), so `valid-memsafety` is never `true`.
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
and a `target` waypoint at the violation location UBSan reported during
replay.

## Results (local, unvalidated)

Run on 2026-09-23 with the C++ engine built from this branch (engine
sources identical to base commit `9135d97`; binary SHA-256 prefix
`cb4e4d21a621`), clang 18, bubblewrap
available, BenchExec 3.35 installed (so the answers went through the
tool-info module's `determine_result`):

```
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --jobs 3
PRISM_BIN=build/prism python tools/svcomp/run_subset.py --property unreach-call --jobs 3
```

Scoring as in SV-COMP: correct `true` +2, correct `false` +1, incorrect
`true` −32, incorrect `false` −16, `unknown` 0. **Not validated**: SV-COMP
only awards points for answers whose witnesses a validator confirms, and no
validator was run, so these are upper bounds under the official rules.

| property | tasks (true / false) | score | max | correct true | correct false | incorrect | unknown |
|---|---|---|---|---|---|---|---|
| no-overflow | 45 (25 / 20) | **35** | 70 | 10 | 15 | **0** | 20 |
| unreach-call | 20 (16 / 4) | **9** | 36 | 4 | 1 | **0** | 15 |

The rerun above is from 2026-09-23, after `bmc` began reporting nondet values
(item 1 below). It used the binary built from `claude/prism-code-checker-x7538r`
at that point, on a machine at load ~18. The previous run scored 22 and 11.

Where the points moved:

- no-overflow: the 5 `jain_*` `false` tasks that were refuted but could not
  be replayed (`jain_1-2`, `jain_2-2`, `jain_4-1`, `jain_6-2`, `jain_7-1`)
  now replay on `bmc`'s nondet values and answer `false(no-overflow)`.
- unreach-call: `gcd_1` and `gcd_2` were `pir: UNKNOWN` or `BOUNDED` across
  two reruns. That is a solver timeout under load, not a wrong answer, and it
  accounts for the lower score.

Where the points are still lost (no-overflow):

- `modulus-1` (`false`): `bmc` reports `INT-SHIFT-UB` (on `1 << s`), not
  `INT-SIGNED-OVF`; the mapping only accepts the overflow class for `false`.
- The rest are `BOUNDED` (loops not closed within unwind 8), `UNKNOWN`
  (solver gave up), `NEEDS-HARNESS` (`large_const`), one `bmc` front-end
  `ERROR` (`nested6`: `goto`), one ILP32 task using `sizeof`
  (`interleave_bits`), and `true` tasks where the
  `pir` stage stops at a reachable `assert`/`abort` failure (`byte_add-1`,
  `modulus-2`, `id_trans`): a `FAILED` of another property says nothing
  about overflow on the other paths.

unreach-call: 2 of the 4 `false` tasks were refuted by `pir` but read nondet
input (not replayable); `sum02-1` is `BOUNDED`. The one `false` answer
(`nested_1b`) replayed: the program was run and `reach_error()` aborted it.

Witnesses: all 11 `false` answers came with a `witness.yml` that parses
as YAML with `format_version: "2.0"` and one `target` waypoint (at the
UBSan location for no-overflow, at the `reach_error()` call for
unreach-call). That is all that was checked about them.

## What is missing for an actual entry

1. **Nondet values in counterexamples (`pir`).** `bmc` now records every
   `__VERIFIER_nondet_*` call with the path guard at the call. A refutation
   of `main` then reports, in `extra["nondet"]`, the values of the calls its
   path executes, in call order. The wrapper replays those values through
   the stubs and writes a `function_return` waypoint for each call, up to
   the first call whose site is ambiguous. The engine reports no source
   positions, so the waypoint prefix stops at a function called from more
   than one site. The `pir` stage does not report nondet values yet, so its
   refutations of programs that read nondet input still answer `unknown`
   (`byte_add-1`, `id_trans` on unreach-call).
2. **Witness validation.** SV-COMP only counts answers whose witnesses a
   validator confirms. No validator (CPAchecker, UAutomizer, or the other
   format-2.0 validators) has been run on PRISM's witnesses. Until one has,
   treat the witness format as untested against real validators.
3. **Correctness witnesses.** PRISM writes no witness for `true` answers.
   Format 2.0 correctness witnesses are sets of loop invariants
   (`invariant_set`); PRISM's k-induction and Houdini invariants would be the
   source, but they are not exported.
4. **Other properties.** `unreach-call` is mapped and scored below;
   `valid-memsafety` can only ever answer `false(valid-deref)` /
   `false(valid-free)` from a replayed refutation; `termination`,
   `no-data-race`, `valid-memcleanup` and the coverage properties are
   `unknown`.
5. **ILP32.** The encoders are LP64. ILP32 tasks that use width-dependent
   types are `unknown` instead of being analysed with 32-bit `long` and
   pointers.
6. **Archive and registration.** A competition entry needs, per the current
   rules: a self-contained archive of the tool runnable on the competition
   machines (PRISM binary, the clang/opt it calls for the `pir` stage, the
   wrapper and witness writer), a licence and README in the archive, the
   tool-info module merged into BenchExec, a benchmark definition, and the
   registration the rules ask for (the fm-tools metadata and an archived
   release). None of these exist yet.
7. **A version string.** `prism` has no `--version`; the wrapper reports
   `0.1.0+sha256.<first 12 hex digits of the binary>` instead.
8. **Scale.** Only the pinned subset (45 no-overflow and 20 unreach-call
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
  `<prop>=sat` (for example `ovf+=sat`): see item 1.
