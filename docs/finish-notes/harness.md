# Harness exec

Slice on `finish/harness-exec`, started at `a6fe572d2`. Law 9 for
`run_harness_bmc` in `src/prism/stages/harness_bmc.cpp`.

The stage model-checks a materialized function. It does not compile a
harness and it does not run one. No `--allow-exec` gate was added: that
would record an execution this function never performs. Behavior is
unchanged. Proof statuses are unchanged: a closed BMC result stays
`PROVED-ASSUMING`, never `PROVED` or `PROVED-UNBOUNDED`.

## What `run_harness_bmc` does

POINTER functions only. SCALAR and VOID functions produce no row.

`materialize` rewrites the function in memory. A one-level integer pointer
parameter becomes a local array and a pointer into that array. The
`// requires:` atoms become an `if` guard that returns 0 when they fail.
The result is a SCALAR or VOID `FunctionInfo`. Nothing is written to disk,
no compiler is invoked, and no process is spawned.

That function is passed to `run_bmc` with `allow_local_pointers` true, or
to `bmc_one` when BMC returns an empty list. `bmc_one` is `run_bmc` of one
function. `allow_local_pointers` is the encoder flag that lets the local
buffer be model-checked (`k_induction_strengthened` in `src/prism/bmc.cpp`).
It is not `Config.allow_exec`.

`run_bmc` is in-process Z3 k-induction. A status of `PROVED` or
`PROVED-UNBOUNDED` is rewritten to `PROVED-ASSUMING`, the stage becomes
`harness`, and the requires text is stored on the finding. Every other
status is left as BMC wrote it, including `BOUNDED`, `FAILED`,
`NEEDS-HARNESS`, and the build-without-Z3 row.

When materialize cannot build an honest buffer, `ai::drafted_harness_bmc`
may draft assumptions. Its checker is also `run_bmc`
(`evaluate` in `src/prism/ai/harness.cpp`). A missing model is a string
note (`NOTRUN:` inside `extra["llm_harness"]` or `extra["harness_draft"]`),
not a finding status and not `sandbox::exec_notrun`. With no usable draft
the stage writes `NEEDS-HARNESS`: "POINTER: no honest requires; unguarded
BMC would invent a buffer or dress a NULL crash as a finding".

## Law 9

`exec_stages()` in `src/prism/pipeline.cpp` does not list `harness`.
Stages absent from that table execute nothing from the scanned tree. The
pipeline calls `run_harness_bmc` directly. It does not wrap the stage in
`exec_gate_note` (fuzz, rapid, and muttest do, because those stages have a
compiled-harness half).

`sandbox::exec_notrun` is not used in this file. There is no path that
compiles the materialized body or runs a binary built from it.

## Tests already present

No new doctest. The row this function writes when it cannot materialize a
harness is `NEEDS-HARNESS`, and that case is already locked.

`tests/cpp/test_wp_harness.cpp`, "harness: POINTER without requires is
NEEDS-HARNESS, not ERROR": `run_harness_bmc` on `null_branch` is
`NEEDS-HARNESS`, and the status is not `NOTRUN`, `ERROR`, `CRASH`,
`FAILED`, or `PROVED`.

`tests/cpp/test_main.cpp` locks the draft path the same way: a template
draft, a refused pointer, and a drafted counterexample stay
`NEEDS-HARNESS` (the counterexample is not reported as `FAILED`).

The only `NOTRUN` status this function can return is the one `run_bmc`
already returns when the binary is built without Z3 (`"z3 not built"`,
`extra["install"]` naming `-DPRISM_Z3=ON`). The harness stage forwards
that status. It is not an allow-exec hold, and `bmc.cpp` is outside this
slice. The no-requires doctest already asserts that path is not `NOTRUN`.
