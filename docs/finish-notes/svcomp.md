# SV-COMP ILP32 slice

Landed from `caed2f7a1` (`origin/cursor/svcomp-ilp32-witness-11d1`) onto
`finish/svcomp-ilp32`, with the witness gate kept strict and a few ILP32
width bugs fixed. The pinned subset was not re-scored.

## Behavior

- Default data model stays **LP64**. `Config::data_model` defaults to
  `"LP64"`. Only the string `ILP32` (any case) selects 32-bit `long`,
  `size_t` and pointers. Any other string, including empty, stays LP64.
- `prism svcomp --data-model ILP32` is opt-in. The score harness and
  `tools/svcomp/prism.py` already pass the task YAML `data_model` through
  that flag. BenchExec tool-info (`prism.py`) did not need a change.
- The hand BMC encoder (`src/prism/bmc_encoder.inc`) takes those widths for
  `long` / `long long`, `size_t`, `ptrdiff_t`, `intptr_t`, `sizeof` (including
  pointers), `__VERIFIER_nondet_long` / `size_t`, `<limits.h>` / `<stdint.h>`
  macros, C11 6.4.4.1 literal ranks, `labs` / `llabs`, and `__builtin_expect`.
  `printf` length modifiers use the matching width (`l` long, `ll`/`j` long
  long, `z` size_t, `t` ptrdiff_t). On LP64 those widths are still all 64, so
  the previous LP64 checks are unchanged.
- Replay adds `-m32` only when the data model is ILP32. A host without a
  32-bit toolchain does not confirm the counterexample, so the answer stays
  `unknown` rather than `false`. On Windows, ILP32 replay is `NOTRUN`.
- `pir` is `NOTRUN` on ILP32 (see stage.cpp below). It does not prove an
  ILP32 task with its LP64 pointer model.
- Correctness witnesses export a loop invariant only when the finding is
  `PROVED`, `PROVED-UNBOUNDED` or `PROVED-CERTIFIED` **and**
  `extra.k_induction` is `closed-invariants`, and the payload is
  `invariants` or `invariant_conjuncts`. A `PROVED` row that merely carries
  `invariant_loops` plus invariant text is not exported. The branch's wider
  gate was not kept.

## Tests

- `tests/cpp/test_bmc_svcomp.cpp`: ILP32 `long` overflow vs LP64 proof;
  `2147483648L` encodes as long long on ILP32; `labs(LONG_MIN)` is
  `INT-SIGNED-OVF` on ILP32 and safe on LP64; `%lld` with `int` is not a
  proof on ILP32 and `%lld` with `long long` is; `abi_from_data_model`
  keeps LP64 unless the string is ILP32.
- `tests/cpp/test_svcomp.cpp`: an ILP32 program with no overflow answers
  `true` from `bmc` (pir is out of the verdict); an ILP32 `long` overflow
  with replay off answers `unknown`, not `true`. Witness export stays empty
  for havoc k-induction and for a `BOUNDED` row even when invariant text
  and loop locations are present.

## Re-score later

The tables in `docs/SVCOMP.md` (no-overflow **88**/192, unreach-call
**117**/225, 0 incorrect, including 64 ILP32 `unknown`s) are the run from
before this change. Re-run `prism svcomp score` on
`testdata/conformance/sv-comp` after merge. That run was not done here.
Do not read those tables as a score for this slice.

What the re-score has to watch:

- ILP32 tasks that use `long` / `size_t` / `sizeof` can now be `true` or a
  replayed `false` from `bmc`, where they used to be `unknown` before
  analysis.
- `false` still needs `--allow-exec` and a compiler that accepts `-m32`.
  Without that, refutations stay `unknown`.
- `pir` contributes no ILP32 proof. Tasks that only `pir` could prove, and
  that are width-dependent, stay on `bmc` alone.

## `src/prism/pir/stage.cpp`

Localized. The rest of `run_pir` is unchanged.

- Include `prism/abi_model.hpp` (line 8).
- `run_pir` lines 1109-1117: comment `SVCOMP-ILP32`, then an early
  `NOTRUN` return when `is_ilp32(cfg.data_model)` is true, before units
  are built.

`CMakeLists.txt` adds `src/prism/abi_model.cpp` with the comment
`svcomp-ilp32`.

## Risks

- The hand BMC encoder still has no 32-bit address space. Pointer
  parameters stay `NEEDS-HARNESS` (Law 6). `sizeof` of a pointer and
  `intptr_t` are 32 bits on ILP32; `pir` does not fill that gap.
- `run_harness_bmc` still calls `run_bmc` at the LP64 default. SV-COMP's
  stage list is `inventory,classify,bmc,pir`, so harness is outside this
  verdict. A full `prism` run uses ILP32 only if something sets
  `Config::data_model`; today that is `prism svcomp`.
- `g_abi` is process-local encoder state, restored when `run_bmc` returns.
  It is not a licence to run two BMC encodes on different models at once.
- `labs`, literal rank, and `%lld` were wrong on the branch (ILP32 `labs`
  stayed 64-bit, an `L` suffix that did not fit in 32 bits was left
  unencoded, and `%lld` was checked against `size_t`). Those are fixed
  here and covered by the new BMC tests. They were not run in this
  worktree (no OneDrive build, no SV-COMP score).
