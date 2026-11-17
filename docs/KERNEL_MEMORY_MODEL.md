# Kernel memory model and atomics (W3)

Status: design and first encoding check. Scope: what the conc stage (src/prism/conc)
can say about C11/LKMM atomics and barriers, which litmus families it must match, and
what stays a named assumption. Litmus results are in section 5 and are filled from the
gate run (tests/cpp/test_conc_litmus.cpp).

## 1. Where the sequential lowering sits

Two places give atomics a meaning today:

- Per-function checks (src/prism/pir): `atomicrmw`, `cmpxchg`, `load atomic`, `store atomic`
  are lowered by the IR parser (ir_parser.cpp, around line 1126) to the plain operation they
  are in a sequential execution; `fence` has no effect. Each use records the assumption
  `atomics-sequential: <instruction> (<orders>)` (translate.cpp line 1065). This is the
  assumption every per-function verdict carries (docs/KERNEL_MODELS.md, R3). It is not changed
  by W3: a function-level proof is still sequential, and stays labelled that way.
- Concurrent checks (src/prism/conc): the only place with a weak memory model. Three modes:
  - default (sequential consistency for plain globals, plus the C11 view model when a scalar
    global or a bounded-memory location has a weaker order): lazy.cpp, section "C11 view model"
    (promise-free RC11-style view machine, docs/CONCURRENCY.md section 6);
  - `--memory-model tso` (per-thread FIFO store buffers, Options::tso, tso_buffer = k);
  - seq_cst accesses with no weaker access to the same location stay SC.

So the question for W3 is not "add a memory model" but "make the existing conc model cover the
kernel's spellings and prove it matches the published litmus outcomes", and say exactly where it
is weaker.

## 2. Mapping of kernel primitives to C11 orders

The mapping is applied where the kernel primitive becomes an IR atomic or fence. Entries marked
(approx) add behaviours (safe for FAILED, may over-report) or drop them only where stated.

| Kernel primitive | C11 image | Note |
|---|---|---|
| `READ_ONCE(x)` / `WRITE_ONCE(x,v)` | relaxed atomic load / store | recognised in the conc stage: a volatile scalar load/store (what the kernel macros expand to) is encoded as a relaxed atomic access (extract.cpp has_volatile); per-function pir keeps the `volatile-access` assumption |
| `smp_load_acquire` / `smp_store_release` | acquire load / release store | recognised when spelled with `__atomic_load_n` / `__atomic_store_n` (or C11 explicit orders) |
| `rcu_dereference` | acquire load (approx) | LKMM orders the dependent accesses by address dependency; acquire is stronger, so sound for BOUNDED-type claims only if the address dependency is also respected: approx |
| `rcu_assign_pointer` | release store | |
| `smp_mb()`, `mb()` | seq_cst fence | recognised via `__atomic_thread_fence`; the IR parser lowers `fence` to a `llvm.donothing` call, so the conc extractor now recognises that form (its fence branch was unreachable before) |
| `smp_rmb()` / `smp_wmb()` | acquire / release fence | a fence strength the kernel does not promise for all pairs: approx (over-strong for smp_rmb, so may drop behaviours; flagged) |
| `smp_mb__before_atomic()`, `smp_mb__after_atomic()` | seq_cst fence at the call (extract.cpp, recognised by callee name) | LKMM linux-kernel.def: `fence{before-atomic}` / `fence{after-atomic}`. Bracketing a relaxed RMW (atomic_inc and friends) gives the full ordering the LKMM gives (explanation.txt, "oddball fences"); a standalone seq_cst fence orders at least what they order, so this is exact for the adjacent-RMW idiom and an over-approximation of their ordering elsewhere |
| `spin_lock` / `spin_unlock` (and `raw_spin_*`) | mutex Lock / Unlock in the view model: acquire / release on the lock | recognised by callee name, lock operand must be a global (else UNENCODED). Same synchronisation as `pthread_mutex_*`. Mutual exclusion and deadlock are modelled by the mutex semantics |
| `cmpxchg`, `xchg`, `atomic_*_return` | RMW with the success/failure orders | the kernel default for value-returning RMWs is fully ordered (seq_cst) |
| `atomic_read` / `atomic_set` | relaxed | |
| `smp_mb__after_spinlock()`, `spin_lock_irqsave`, rwlocks, RCU grace periods | not modelled: the callee is not recognised and is passed through with no effect | see section 4 (unrecognised calls) |
| LL/SC pairs (`ldxr`/`stxr`, arm64) | not an IR atomic: outside the model | UNENCODED until an LL/SC model exists |

## 3. The encoding that exists

- Views (lazy.cpp): each weak location keeps messages (timestamp, value, release view); a thread
  view per location; acquire joins, release attaches, relaxed stores carry the last release fence;
  RMWs read ts and write ts+1; seq_cst fences share one SC view. Race check by happens-before.
- Approximation: seq_cst accesses to a location that also has weaker accesses are treated as
  acq_rel. This only adds behaviours. Locations with only seq_cst accesses stay SC, so the
  classical SB and IRIW tests with seq_cst accesses are forbidden.
- Load buffering is forbidden (RC11, no po;rf cycles). This is a deliberate restriction of
  ISO C11, not a bug; it is a point where the model is stronger than the standard, and it is
  listed in the litmus expectations.
- TSO is a separate hardware mode; mixing it with weaker C11 orders is refused as NEEDS-HARNESS.

## 4. Gaps (what W3 does not yet close)

1. READ_ONCE / WRITE_ONCE (volatile accesses), the smp_* barriers (`__atomic_thread_fence`,
   `__atomic_load_n` / `__atomic_store_n`), `smp_mb__before_atomic` / `smp_mb__after_atomic` and
   `spin_lock` / `spin_unlock` ARE recognised by the conc extractor (section 2). The kernel macros
   must reach the IR as those spellings; kernel macros that expand to inline asm are not recognised.
   Not modelled:
   - Dependency ordering. Address, data and control dependencies are not tracked. `rcu_dereference`
     as an acquire load is the stand-in for address dependencies, and it is stronger than the LKMM.
   - `smp_mb__after_spinlock()`, `spin_lock_irqsave`, rwlocks, RCU grace periods, `smp_mb()` /
     `smp_rmb()` spelled as calls rather than fences (the litmus harness spells them as fences).
   - Load buffering: LB+poonceonces is Sometimes in the LKMM; the promise-free model forbids it.
   A plain (non-volatile) shared access is still race-checked, as before.
   Pointers: address values are not modelled (ptrtoint is UNENCODED), so the RCU litmus tests
   publish an index instead of a pointer.

   Soundness consequences (what a verdict on kernel code may claim):
   - An unrecognised external call is passed through unchanged (extract.cpp, end of rewrite_call):
     any barrier or lock it stands for has NO effect in the conc model. That adds behaviours, so a
     FAILED may be spurious for code relying on it; a BOUNDED is not weakened by it.
   - Dependency ordering is missing: the model allows more than the LKMM there. Same direction: a
     FAILED may be spurious for dependency-protected code; a BOUNDED is not weakened by it.
   - Load buffering is forbidden in the model and allowed by the LKMM. This is the one place the
     model is STRONGER than the LKMM. A BOUNDED verdict on an LB-shaped outcome is therefore NOT
     sound for kernel code, and must not be reported as a kernel result.
   - `smp_rmb()` as an acquire fence is stronger than the LKMM (section 2). A BOUNDED verdict that
     depends on an smp_rmb is not sound for the kernel.
   So BOUNDED verdicts on kernel code are sound only for the modelled orderings and no LB-shaped or
   smp_rmb-dependent outcome; any other claim about kernel code must carry these limits.
2. LL/SC pairs: no model. Reported UNENCODED, so a verdict on code using them is never produced
   silently; no soundness claim is made for it.
3. Per-function pir atomics stay sequential (assumption `atomics-sequential`). A proof of a
   single function does not see other threads' weak behaviour; only the conc stage does.
4. An atomic location in the bounded memory is identified by its start address; overlapping
   accesses of different widths are distinct locations (docs/CONCURRENCY.md).
5. Bounded interleavings: the encoding is bounded by rounds K and per-thread unwinding. A
   BOUNDED verdict is never PROVED (laws.hpp).

## 5. Litmus suite

Files: tests/conc/litmus/*.c. The first line of each file gives the expected verdict
(`// EXPECT: FAILED` = forbidden outcome reachable, `// EXPECT: BOUNDED` = forbidden outcome
unreachable). Each header cites the source of the expected outcome.

| Test | Expected | Source of the expected outcome |
|---|---|---|
| sb_relaxed | FAILED (0,0 allowed) | ISO C11 7.17.3; Batty et al., POPL 2011, sec. 2; Lahav et al., PLDI 2017, fig. 1 |
| sb_seqcst | BOUNDED | ISO C11 7.17.3 (total order S); herd7 SB+scs |
| mp_relaxed | FAILED | ISO C11 5.1.2.4; Lahav et al., PLDI 2017, sec. 2 |
| mp_relacq | BOUNDED | ISO C11 5.1.2.4 (synchronizes-with); herd7 MP+rel+acq |
| lb_relaxed | BOUNDED (RC11) | Lahav et al., PLDI 2017, sec. 3 (RC11 forbids LB; ISO C11 allows it: the model is RC11) |
| iriw_acq | FAILED | Batty et al., POPL 2011, sec. 4; Lahav et al., PLDI 2017, sec. 2; herd7 IRIW+acqs |
| iriw_seqcst | BOUNDED | ISO C11 7.17.3; herd7 IRIW+scs |
| corr_relaxed | BOUNDED | ISO C11 5.1.2.4, 7.17.3; Batty et al., POPL 2011, sec. 3 |
| cow_coww | BOUNDED | ISO C11 5.1.2.4, 7.17.3; herd7 CoWW |
| rcu_publish | BOUNDED | Linux memory model (tools/memory-model, smp_store_release / smp_load_acquire); McKenney, "Is Parallel Programming Hard" sec. 9.5 |
| rcu_publish_relaxed | FAILED | ISO C11 5.1.2.4; Linux memory model (rcu_assign_pointer is a release store) |
| mp_kernel_relacq | BOUNDED | tools/memory-model/litmus-tests/MP+pooncerelease+poacquireonce.litmus (Never) |
| mp_kernel_wmb_rmb | BOUNDED | .../MP+fencewmbonceonce+fencermbonceonce.litmus (Never) |
| mp_kernel_once | FAILED | .../MP+poonceonces.litmus (Sometimes) |
| sb_kernel_mb | BOUNDED | .../SB+fencembonceonces.litmus (Never) |
| sb_kernel_once | FAILED | .../SB+poonceonces.litmus (Sometimes) |
| lb_kernel_mb_ctrl | BOUNDED | .../LB+fencembonceonce+ctrlonceonce.litmus (Never) |
| mp_kernel_spinlock | BOUNDED | .../MP+polocks.litmus (Never): spin_lock / spin_unlock as acquire / release |
| sb_kernel_atomic_mb | BOUNDED | linux-kernel.def (smp_mb__before_atomic, smp_mb__after_atomic as fences); Documentation/explanation.txt ("oddball fences"); Documentation/atomic_t.txt. Expected = SB with a full barrier on each side |
| sb_kernel_atomic_nofence | FAILED | .../SB+poonceonces.litmus (Sometimes), with the stores and loads as relaxed and an unordered atomic_inc between them (Documentation/atomic_t.txt: no implied barrier) |

Not in the table, by design: LB with dependencies and smp_rmb (the model's load-buffering and
smp_rmb strength are outside the LKMM, section 4).

Existing tests that already cover parts of this: c11_sb_ra, c11_mp_ra, c11_sc_fence_sb,
tso_sb (tests/conc).

## 6. Results

PENDING: the gate result for this section is filled in after the W3r run. An earlier note here
claimed "all 17 files match"; no gate run backed it, so it has been removed.

A BOUNDED result is never PROVED: bounded rounds/unwinding. Harness note: the EXPECT value is read up to the first non-capital character.
