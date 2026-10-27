# vector_insert_true

`vector_insert_true` does not finish because the insert index is symbolic, so the
Bv memory encoding never switches to word-level loads. A timeout or an abort
stays a no-answer. Nothing here marks that task PROVED.

## Cause

`SymMem::word_at` (`src/prism/pir/memory.cpp`, the `Entry::Byte` case around
line 856) returns nullopt as soon as one earlier byte store has no value set.
The load then stays a per-byte read-over-write ite: the symbolic address is
compared with every store. That is the formula that runs for tens of minutes
or is killed.

The index in `testdata/conformance/libc-models/vector_insert.cpp` (line 9,
`PRISM_INDEX_LE3`) is a nested ternary. Clang is invoked at `-O0`, so the join
is a phi of constants 0, 1, 2, and 3, not an `Op::Select`. The phi ite is built
in `encode.cpp` (around line 952). The multiply that scales the index is not
marked `ptr_arith`, so the old seeding path never saw it.

The previous `materialize_vs` made two separate failures:

- It called full `vs_raw` on the select. That walk retains every subexpression
  it touches (`vs_keep_` / `vs_memo_`) and is the memory blow-up on a
  read-over-write term.
- It kept a pointer into `vs_memo_` and then called `seed_vs`, which erases
  that slot (`memory.cpp` `seed_vs`, the `vs_memo_.erase` after the keep
  list). The following read of the pointer is a use-after-free. An abort
  writes no report. It also copied the ite's guards onto the index parameter.
  The else guard of `PRISM_INDEX_LE3` is "not 0, 1, or 2", which is not
  "index == 3". That transfer is not done now. Only the ite/phi/arithmetic
  term is seeded.

An empty value set stored as a present `optional` was treated as success.
`vs_of`, `read_cell_log`, and `per_obj` then call `front()` / `back()` or skip
the byte model. Empty is a miss (`vs_raw` resets it, around line 519;
`seed_vs` refuses it).

## Law 7: no findings, no output

`prism-qa conformance` defaults `--mem-limit-mb` to 4096
(`src/tools/qa/conformance.cpp`, the `--mem-limit-mb` help text). When the
child leaves no `report.json`, `src/tools/qa/support/engine.cpp` (around line
124) records `no report.json (memory limit)` if stderr mentions `bad_alloc` /
`out of memory`, or if the exit code is -6, -9, 134, or 137. Otherwise it
records the exit code. `conformance.cpp` (around line 458) then scores an
empty finding list. `src/tools/qa/support/report.cpp` (around line 97) returns
`missing` when that list has no statuses. That is the ~96s Law 7 row.

Three different events land there:

- Abort (the old use-after-free, or a heap checker). No C++ catch runs, so
  `src/prism/pir/stage.cpp` (around line 1194) never writes its ERROR finding.
- `std::bad_alloc` thrown from the encoder. The stage catch would record
  ERROR if it propagated. `check_function_at` now catches it first and returns
  UNKNOWN with `encoder ran out of memory; no proof` (`encode.cpp` around
  line 2216). UNKNOWN is a finding, not a proof.
- The process is killed by the job object or SIGKILL while encoding or
  solving. That cannot be caught. Block-buffered stdout explains a direct
  `pir` run that sits for ~61 minutes and then prints nothing.

`PRISM_CHECKER_MEM` (`src/prism/solver/portfolio.cpp`, `checker_mem_mb` around
line 386) caps cake_lpr, the Lean LRAT checker, and the Lean bit-blast. It
does not cap the Bv encoder. `PRISM_FUNCTION_BUDGET` is checked when a VC is
solved, not while the formula is built. Checking it during `build` would fail
the existing 1e-9 budget test, which expects the budget to be spent before a
VC, after encoding. The solver cache (`portfolio.cpp` around line 1303)
stores Sat and Unsat only. A timeout is not replayed as Unsat.

## What changed

Sound, local. The byte model is still used whenever a seed is missing. No
check is skipped. `kVsMax` is unchanged.

- `shallow_vs` (`memory.cpp`) seeds a nested ite of constants, already-seeded
  arithmetic, or `x & mask` when `x` is a leaf parameter and the mask has at
  most 5 bits (`i & 1` in `vector_insert_uaf_false`). The mask guards are
  equalities on that and-term, not a value set for `x`. A leaf that already
  has a value set keeps the smaller product. A miss is not written into
  `vs_memo_`. One walk remembers expr ids so a shared DAG is not expanded.
- `materialize_vs` copies the shallow set, then seeds. It does not call
  `vs_raw` and does not seed the index parameter.
- `note_prov` (`encode.cpp` around line 436) materializes select, add/sub/mul,
  shifts, div, and, sext/zext/trunc. Phis (around line 952) and LCSSA (around
  line 593) are materialized after the ite is built. `seed_vs_bin` still
  handles `ptr_arith`, and its operand walk is shallow rather than `vs_raw`.
- `load` / `store` seed `ptr+k` from a seeded base with no walk
  (`seed_plus_const`).
- Group queries: an unknown group keeps UNSAT marks, so those properties are
  not solved again. `answered` is cleared only when a group SAT was not pinned
  to one property (the existing MSVC caveat). A singleton UNSAT is recorded
  the same way. A timed-out property is still asked on its own.
- `std::bad_alloc` in `check_function_at` is UNKNOWN, not PROVED.

The harness is unchanged. The index clamp is not a harness bug.

## Residual risk

- One store whose address still has no value set disables `word_at` for every
  later load that sees that log prefix. A pointer loaded as an unseeded concat
  of bytes, or a GEP that uses the raw parameter `i` instead of the clamped
  ite/phi, stays on the per-byte formula.
- Shallow depth is 64. A deeper ite is not seeded here. A later `vs_of` may
  still call `vs_raw` (depth 400) on that address and retain the term.
- A mask wider than 5 bits is not enumerated. `x & mask` is not applied to a
  load, only to a leaf parameter.
- Subgroup UNSAT is trusted the same way an all-UNSAT group already was. The
  disagreement path (SAT group, both halves UNSAT) still erases that range
  and asks those properties again.
- A kill or an abort still produces no `report.json` and still scores
  `missing`. This change does not add an encoder RSS cap.
- `pir` was not run on `vector_insert`. There is no evidence the task now
  finishes inside the 900s task timeout or the 4096MB conformance cap.
