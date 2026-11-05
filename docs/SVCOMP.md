# PRISM as an SV-COMP verifier

PRISM has **not** entered SV-COMP. This file describes the pieces that would
make an entry, as the code is: `prism svcomp` (one task), `prism svcomp
score` (the pinned subset), `prism svcomp pack` (the tool archive), the
BenchExec tool-info module, how an answer is decided, the witnesses, the
data models, and what is still missing.

## Pieces

| piece | what it is |
|---|---|
| `prism svcomp` (`src/prism/cli_svcomp.cpp`, `src/prism/svcomp/`, `include/prism/svcomp.hpp`) | runs PRISM on one task in-process, decides the answer, replays the counterexample, writes the witness |
| `prism svcomp score` | runs `prism svcomp` on every task of the pinned subset and scores it with SV-COMP points |
| `prism svcomp pack` | builds the tool archive |
| `tools/svcomp/prism.py` | BenchExec tool-info module (`BaseTool2`): builds the command line and reads the result line |
| `tools/svcomp/prism-subset.xml` | BenchExec benchmark definition for the subset (15 min CPU, 15 GB, 4 cores; passes `--allow-exec`) |
| `tools/svcomp/fm-tools.yml` | fm-tools registration draft (`id: prism-svcomp`) |
| `testdata/conformance/sv-comp/` | the pinned subset: upstream task files and the property files `no-overflow.prp`, `unreach-call.prp`, `valid-memsafety.prp` (pin and licences: `testdata/conformance/SOURCES.md`) |
| `tests/cpp/test_svcomp*.cpp` | doctests: properties, decisions, replay, fuzzing, waypoints, witnesses, invariants, data models, scoring, archive, tool-info module |

## `prism svcomp`

```
prism svcomp [--allow-exec] [--data-model ILP32|LP64] [--out DIR] [--witness FILE]
             --prop PROPERTY.prp TASK.c
```

| option | meaning |
|---|---|
| `--prop FILE` | the SV-COMP property file (required) |
| `--allow-exec` | allow counterexample replay and fuzzing, which compile and run the task (Law 9); without it every refutation is `unknown` |
| `--data-model ILP32\|LP64` | the task's data model (default LP64; any other value is exit 2) |
| `--out DIR` | work directory (default `prism-svcomp-out`) |
| `--witness FILE` | witness path (default `witness.yml`) |
| `--version` | prints the PRISM version |

It prints exactly these lines:

```
PRISM-SVCOMP-RESULT: true | false(no-overflow) | false(unreach-call) | false(valid-deref) | false(valid-free) | unknown | error
PRISM-SVCOMP-REASON: <why>
PRISM-SVCOMP-WITNESS: <file>          (only when a witness was written)
```

What it does:

1. Reads the property file. An unsupported property is `unknown`.
2. Copies the task to `DIR/<stem>.c` (a preprocessed `.i` task is C).
3. For `no-overflow` only, rewrites the copy so the error calls cannot block
   a proof (`neutralize_error_calls`, `src/prism/svcomp/property.cpp`):
   every call statement `reach_error()`, `__VERIFIER_error()` or `abort()`
   becomes `exit(0)`, line numbers kept. All three end the execution, so the
   set of reachable overflows is the same. A `__VERIFIER_assert` whose body
   is then only "exit on a false condition" is renamed to
   `__VERIFIER_assume` at every call. Nothing is rewritten when the task
   defines its own `exit()` or already names `__VERIFIER_assume`. Replay,
   fuzzing and the witness use the original task.
4. Runs the pipeline in its own process with the stages
   `inventory,classify,pir`, no LLM, and `allow_exec` off: the pipeline
   never runs task code. The task is analysed as a Linux program
   (`x86_64-unknown-linux-gnu`, or i386 with `--data-model ILP32`),
   whatever the host or `PRISM_TARGET`.
5. Decides the answer from `report.json` (next section).
6. If the answer is `unknown`, `--allow-exec` is given and the property is
   `no-overflow` or `unreach-call`, tries the fuzz fallback.
7. Writes the witness.

## Properties

`parse_property` reads the LTL formula of the property file. A file that
names exactly one of these is supported:

| property | formula |
|---|---|
| `no-overflow` | `LTL(G ! overflow)` |
| `unreach-call` | `LTL(G ! call(reach_error()))` |
| `valid-memsafety` | `LTL(G valid-free)`, `LTL(G valid-deref)`, `LTL(G valid-memtrack)` |

Every other property (`termination`, `no-data-race`, `valid-memcleanup`,
coverage) is `unknown`.

## Mapping report.json to an SV-COMP answer

`decide` (`src/prism/svcomp/decide.cpp`) reads the findings about `main` of
the `pir` stage; a crashed `pir` stage is a `STAGE-FAILED` row. It follows
the verdict laws in [VERDICTS.md](VERDICTS.md): a weaker verdict never
becomes a stronger answer.

| property | a `true` needs | a refutation is a `FAILED` of |
|---|---|---|
| `no-overflow` | a `pir` proof of `main` | `INT-SIGNED-OVF`, or `INT-SHIFT-UB` whose check is `shift-base` |
| `unreach-call` | a `pir` proof of `main` | `FUNC-CONTRACT` whose check is `reach_error` or `assert` |
| `valid-memsafety` | never `true` | `MEM-OOB-READ`, `MEM-OOB-WRITE`, `PTR-NULL-DEREF` |

The check name is `extra["prop"]` of the finding, else the message prefix
before `:`. In order:

1. No finding about `main` → `unknown`.
2. Every refutation is replayed (next section), those that report nondet
   call locations first. The first that replays gives `false(<property>)`;
   for `valid-memsafety` the replay names the subproperty (`valid-deref` or
   `valid-free`). If `pir` also proved `main`, the reason says
   `DISAGREEMENT`.
3. A refutation that does not replay → `unknown`. It also blocks a proof.
4. `PROVED`, `PROVED-UNBOUNDED` or `PROVED-CERTIFIED` of `main` by a stage
   that covers the property → `true`. `PROVED-ASSUMING` and `BOUNDED` are
   never `true` (Law 2).
5. Anything else (`NEEDS-HARNESS`, `BOUNDED`, `UNKNOWN`, `TIMEOUT`,
   `ERROR`, `NOTRUN`, a crashed stage) → `unknown`.

No stage encodes memory leaks (`valid-memtrack`), so a proof never covers
`valid-memsafety`.

### unreach-call

`pir` (`src/prism/pir/translate.cpp`) checks every call of `reach_error()` or
`__VERIFIER_error()` as a `FUNC-CONTRACT` property named `reach_error`: a
reachable call is the violation, whatever the unit's definition of the sink
does (an empty `static void reach_error(void) {}` included), and the path
ends there. A `__VERIFIER_assert` declared without a body is checked at the
call site as the property `assert`; a defined one is analysed like any
function, so the `reach_error()` call in its body is checked.
`__VERIFIER_assume` and `assume_abort_if_not` (declared without a body)
constrain the path; `abort()` inside `assume_abort_if_not` only ends it.

A failing C `assert()` (`__assert_fail` and its variants) is also a
`FUNC-CONTRACT` check named `assert`, so it is a candidate refutation of
`unreach-call`. It answers `false` only when the replay shows
`reach_error`; otherwise the answer is `unknown`. A reachable `abort()`
elsewhere is a `FUNC-CONTRACT` check named `abort`: it is not an
`unreach-call` refutation, but it prevents a proof of `main`, so the answer
is `unknown`. `exit()` ends the path.

### Shifts and no-overflow

The SV-COMP rules define `no-overflow` as an operation whose result type is
a signed integer type but whose value is out of its range (C11 undefined
behaviour; conversions to signed types do not count). A signed `E1 << E2`
with `E1 >= 0` whose value `E1 × 2^E2` does not fit is such an operation
(C11 6.5.7p4). A negative or too large shift count, and a negative left
operand, are undefined but do not produce an out-of-range value, so they
are not `no-overflow` violations.

PRISM reports all of these as `INT-SHIFT-UB`. The mapping accepts only the
check `shift-base`, which also fires for a negative left operand. The replay
then decides: only UBSan's report "left shift of N by M places cannot be
represented" counts.

## Replay

`replay` (`src/prism/svcomp/replay.cpp`) proves a refutation by running it:

- The refutation must carry the values of the `__VERIFIER_nondet_*` calls on
  its path (`extra["nondet"]`, `fn=value, ...` in call order), unless the
  task has no nondet call. `pir` reports them from one Z3 model of the
  violation, only calls that run before the violated check
  (`src/prism/pir/encode.cpp` `nondet_trace`). Calls inside PRISM's own
  library models (a failing `malloc`) are not in the list.
- Without `--allow-exec` the replay is `notrun` (Law 9), and the answer is
  `unknown`.
- Generated stubs define each nondet function the task does not define
  (returning the values in order), `__VERIFIER_assume` (exit 0 when false)
  and `reach_error` (abort). An unsupported nondet type is `unsupported`.
- The task and stubs are compiled with `clang`, `gcc` or `cc` from `PATH`:
  `-g -O0 -w -std=gnu11`, plus `-fsanitize=signed-integer-overflow,shift-base`
  for `no-overflow` and `-fsanitize=address` for `valid-memsafety`, with
  `-fno-sanitize-recover=all`, plus `-m32` for ILP32.
- The binary runs with a 10 s limit in the sandbox (`sandbox::wrap_argv`:
  bwrap when it works here, resource limits always; the reason records
  which).
- It replays when the run shows the violation: a UBSan report of signed
  overflow, negation, division or a non-representable left shift
  (`no-overflow`); an abort naming `reach_error` (`unreach-call`); an ASan
  invalid access (`valid-deref`) or invalid free (`valid-free`).

### Fuzz fallback

When the answer is still `unknown`, `--allow-exec` is given and the property
is `no-overflow` or `unreach-call`, `fuzz_replay` compiles the task once
(`-no-pie`, the `no-overflow` sanitizer) and runs it on up to 400 seeds
within 60 s, 2 s per run. Odd seeds draw small values; even seeds mix in
type extremes. If every early run times out, seed 1 gets one longer run
(up to 30 s). The answer becomes `false` only when a run shows the violation
under the same check as replay. Fuzzing never answers `true` (Law 3). The
run's nondet values become the witness's waypoints; their call sites come
from `llvm-symbolizer` and are dropped unless every one resolves in the task
file.

## Witnesses

Witnesses are format 2.0 YAML (`src/prism/svcomp/witness.cpp`,
`include/prism/svcomp_witness.hpp`). The metadata names the producer
(`PRISM` and the binary's version), a UUID, the UTC creation time, and the
task: input file, its SHA-256, the specification, the data model and the
language.

**Violation witness** (`false`): one `violation_sequence` entry.

- One `function_return` waypoint per nondet value, constraint
  `\result == <value>` in format `acsl_expression`, located at the call's
  closing parenthesis. The call is found at the location the finding reports
  (`extra["nondet_loc"]`; it must start exactly there), else at the
  function's only call site in the task. A call that is neither ends the
  waypoint list: a waypoint at the wrong call would make the witness wrong,
  a missing one only weaker. Locations not marked
  `extra["nondet_loc_kind"] = "physical"` are ignored in a task with line
  markers (`# 12 "file.c"`); `pir` and the fuzz fallback mark theirs
  physical.
- One `target` waypoint. For `unreach-call`: the `reach_error()` call, the
  only call site or the one on the finding's line; if that is ambiguous, no
  witness is written. For the other properties: the statement or full
  expression holding the operation the sanitizer reported. A statement that
  is the first on its line gets no column (format 2.0 then means the first
  statement or full expression of the line); a later one gets its start
  column; a `for` header, several declarators or a statement continued from
  an earlier line keep the sanitizer's column. The target has no `function`
  field (the violation need not be in `main`).

**Correctness witness** (`true`): one `invariant_set` entry. It carries only
invariants `pir` proved: when the deciding finding is a proof (not
`PROVED-ASSUMING`) with `k_induction = closed-invariants`, its Houdini loop
invariants (`extra["invariant_conjuncts"]`, over IR value names, and
`extra["invariant_loops"]`, the loop positions). `src/prism/svcomp/debug_ir.cpp`
compiles the task once more with `-g` and maps a value to a C variable only
when that is certain at the loop head (one variable, a unique name, in scope,
not reassigned on the way to the header). A conjunct is rendered only where
C means the proved bit-vector relation (signedness, promotion,
wrap-around). Left out: `do` loops, loops whose keyword is not at the
reported position, conjuncts over a name declared in a `for` init, and every
invariant of a task with line markers. Every other proof exports the empty
invariant set, which is trivially valid and leaves the proof to the
validator. No invariant is ever guessed, and dropping a conjunct keeps the
witness valid.

## Witness validation

Nothing in this repository validates a witness. To check one, give it to a
format-2.0 validator with the task and its data model, for example:

```
cpachecker --witnessValidation --witness witness.yml --spec PROP.prp --64 --timelimit 900s TASK.i
Ultimate.py --spec PROP.prp --architecture 64bit --file TASK.i --validate witness.yml --witness-type violation_witness
```

A validator confirms a violation witness with `FALSE` and a correctness
witness with `TRUE`. UAutomizer 0.3.1 did not match `function_return`
waypoints on plain assignments `x = __VERIFIER_nondet_int();` (it rejects the
sv-witnesses format-2.0 example witness the same way); PRISM keeps
`function_return`, which format 2.0 defines for a nondet return value.

## Data models

The default data model is LP64. `--data-model ILP32` (the score harness and
`tools/svcomp/prism.py` pass the task's `options.data_model`) makes `pir`
lower the task and its library models for `i386-unknown-linux-gnu`: 32-bit
`long`, `size_t` and pointers (`include/prism/pir/frontend.hpp`). That needs
the 32-bit glibc headers (Debian: `libc6-dev-i386`). Replay and fuzzing add
`-m32`, so they need a 32-bit C toolchain with the i386 sanitizer runtimes;
without one the build fails and a refutation stays `unknown`. On Windows an
ILP32 replay is `notrun`. SV-COMP tasks are always analysed as Linux
programs.

## `prism svcomp score`

```
prism svcomp score [--property no-overflow|unreach-call] [--jobs N] [--out DIR]
                   [--timeout S] [--only SUBSTRING] [--suite DIR] [--prism BIN]
```

It runs `prism svcomp --allow-exec` on every task of the subset with a
verdict for the property (default `no-overflow`), one process session per
task (a timeout kills the whole tree; default 900 s, `--jobs` default 2).
The pinned subset in this repository is trusted, so its tasks run with
`--allow-exec`. Points as in SV-COMP: correct `true` +2, correct `false` +1,
incorrect `true` −32, incorrect `false` −16, `unknown`, `error` and timeout
0. No validator runs, so the score is an unvalidated upper bound. It writes
`DIR/score.md` and `DIR/results.json` and exits 1 on any incorrect answer.
The suite defaults to `testdata/conformance/sv-comp` above the working
directory or the binary.

### Last recorded scores

These are the last rescores of the pinned subset with a recorded commit.
That binary still had the `bmc` stage (deleted since, commit c66284cde), so
they do not describe the current engine; the subset has not been rescored
since.

| property | date, commit | score / max | correct true | correct false | incorrect | unknown |
|---|---|---|---|---|---|---|
| no-overflow | 2026-10-30, 237a3b309 | 156 / 192 | 57 | 42 | 0 | 18 |
| unreach-call | 2026-10-30, e4deccc86 | 152 / 225 | 64 | 24 | 0 | 39 |

## `prism svcomp pack`

```
prism svcomp pack [--out prism-svcomp.tar.gz] [--prism BIN] [--repo DIR]
```

It builds a tarball (with `cmake -E tar`, else `tar`) that unpacks to
`prism/`: the `prism` binary (BIN, default the running executable), the
tool-info module `prism.py`, `README.md` and `fm-tools.yml` from
`tools/svcomp/`, the repository `LICENSE`, `MANIFEST.json` (format version,
creation time, PRISM version, git commit, file list) and
`dependencies.json` (the `clang` and `opt` versions found on `PATH`, whether
`bwrap` is there, and the optional `PRISM_FUNCTION_BUDGET`,
`PRISM_HOUDINI_BUDGET` and `PRISM_CERTIFY_BUDGET` knobs). clang and opt are
recorded, not bundled. CI builds the archive and lists it
(`.github/workflows/ci.yml`).

## BenchExec

`tools/svcomp/prism.py` is the one Python file of the entry: BenchExec
loads tool-info modules as Python classes. It runs
`prism svcomp OPTIONS --prop PROPERTY.prp [--data-model DM] TASK` and maps
the `PRISM-SVCOMP-RESULT:` line to BenchExec's results; no result line is
`TIMEOUT` when BenchExec stopped the run for time, else `ERROR`. CI runs
BenchExec's own check of the module:

```
PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism --tool-directory build --no-container
```

Run it outside the repository root, so that the module name `prism` means
this file. The subset runs in BenchExec with:

```
PYTHONPATH=tools/svcomp benchexec tools/svcomp/prism-subset.xml --tool-directory DIR
```

where DIR holds the `prism` binary. The benchmark definition passes
`--allow-exec` (a `false` answer needs a replay) and collects
`witness.yml`. BenchExec needs every property file a task definition names;
the subset carries only `no-overflow.prp`, `unreach-call.prp` and
`valid-memsafety.prp`. Where BenchExec's overlay mount of `/` fails, add
`--read-only-dir / --overlay-dir /tmp --overlay-dir /home`; where the cgroup
allows less than 15 GB, lower the limit with `-M`.

## What is missing for an entry

1. **Bundled toolchain.** The archive records `clang`/`opt`; it does not
   carry them. The `pir` stage needs the pinned LLVM (`PRISM_LLVM_BIN`, or
   the pinned build baked in at build time); replay needs a C compiler with
   sanitizer runtimes.
2. **Registration.** `prism.py` is not merged into BenchExec, and
   `fm-tools.yml` is a draft without versions, an archived release URL or
   maintainers.
3. **Properties.** `valid-memsafety` can only answer `false(valid-deref)` or
   `false(valid-free)`; `termination`, `no-data-race`, `valid-memcleanup` and
   the coverage properties are `unknown`.
4. **Correctness witnesses** are empty unless `pir` closed the loops with
   Houdini invariants; a validator must then re-prove the program.
5. **Nondet values from library models** (a failing `malloc`) are not in
   the trace, so such a refutation replays down another path and stays
   `unknown`.
6. **Scale and validation.** Only the pinned subset has been scored, and no
   witness is validated in the repository.
