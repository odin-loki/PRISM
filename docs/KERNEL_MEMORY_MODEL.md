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

   Soundness consequences and how the verdict now carries them. A BOUNDED that would rest on one of
   the named assumptions below is withheld: the stage reports NEEDS-HARNESS with an
   `UNENCODED: assumption <names>` reason and `extra.assumptions` (stage.cpp `refuse_assumptions`,
   Program::assumptions set in extract.cpp `build_impl`). FAILED is never changed: a run the model finds
   is a run of the model, and the model's extra or missing orderings only change what it can find.
   - Unrecognised external calls. The earlier note that these "pass through with no effect" is out of
     date for calls inside a thread: the PIR translation refuses an undefined callee there
     (`UNENCODED: call @f (in thread p)`, checked with ext_sync, strlen, memcmp, puts, usleep), so the
     verdict is already NEEDS-HARNESS. The guard `conc-extern-noeffect` (extract.cpp, end of
     rewrite_call; `extern_call_benign` lists the calls that have no effect: verifier and debug
     intrinsics, `__prism.uninit`, assert, abort, exit, stdio output) withholds a BOUNDED if a call
     the translation does accept reaches the conc stage without a visible operation. No such call is
     known today, and no litmus test reaches that path, so it is a guard, not a tested fix. Havoc of
     the shared state was not done: it would add nondeterministic writes to every shared global, which
     creates spurious data races (FAILED the LKMM does not have).
   - Load buffering (`lkmm-no-lb`): kernel mode only (below). The LB shape is a relaxed load of one
     location followed, with no full ordering in between, by a relaxed store to another location, in
     at least two threads. Full orderings are seq_cst fences, smp_mb__before/after_atomic, and
     seq_cst or acq_rel accesses. Dependencies do not count as ordering. Acquire loads and release
     stores do not reset the pattern, which is conservative (more withheld, never fewer).
     LB+poonceonces (lb_kernel_once) is withheld; LB+fencembonceonce+ctrlonceonce (lb_kernel_mb_ctrl)
     keeps its BOUNDED, because its second thread has a full barrier between its load and store.
     The C11 test lb_relaxed keeps its RC11 BOUNDED: it is not kernel code, and the RC11 choice is
     documented in section 3.
   - `smp_rmb()` as an acquire fence (`lkmm-rmb-strong`) and `smp_wmb()` as a release fence
     (`lkmm-wmb-strong`) are stronger than the LKMM's read-read and write-write fences. Kernel mode
     only. Every BOUNDED with such a fence present is withheld, since a verdict cannot tell whether it
     depends on the stronger order.
   - Dependency ordering is missing: the model allows more than the LKMM there. A FAILED may be
     spurious for dependency-protected code; a BOUNDED is not weakened by it (the model has more
     behaviours, so no violation in it means none in the LKMM either). Not withheld.
   - NOT covered by this change (open): `rcu_dereference` as an acquire load is STRONGER than the
     LKMM's address dependency (table in section 2). A BOUNDED that depends on that ordering is not
     sound for the kernel. It needs its own named assumption (for example `lkmm-rcu-deref-acquire`)
     or a dependency model; neither exists yet.
   Kernel mode means: a volatile (READ_ONCE / WRITE_ONCE) access, a spin_lock / spin_unlock (raw too),
   or smp_mb__before/after_atomic in scope (KernelScan::kernel in extract.cpp).
   So a BOUNDED on kernel code is claimed only for the modelled orderings, and only when none of the
   named assumptions applies to it; any other claim about kernel code must carry these limits.
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
| mp_kernel_wmb_rmb | NEEDS-HARNESS (lkmm-rmb-strong, lkmm-wmb-strong) | .../MP+fencewmbonceonce+fencermbonceonce.litmus (Never); withheld because the fences are stronger than smp_rmb / smp_wmb |
| lb_kernel_once | NEEDS-HARNESS (lkmm-no-lb) | .../LB+poonceonces.litmus (Sometimes): the LKMM allows the outcome, the model forbids it, so no BOUNDED claim |
| mp_kernel_extern | NEEDS-HARNESS (UNENCODED: call @ext_sync) | MP with an undefined external call between the producer's stores: refused by the PIR translation; shape of MP+poonceonces.litmus |
| mp_kernel_once | FAILED | .../MP+poonceonces.litmus (Sometimes) |
| sb_kernel_mb | BOUNDED | .../SB+fencembonceonces.litmus (Never) |
| sb_kernel_once | FAILED | .../SB+poonceonces.litmus (Sometimes) |
| lb_kernel_mb_ctrl | BOUNDED | .../LB+fencembonceonce+ctrlonceonce.litmus (Never) |
| mp_kernel_spinlock | BOUNDED | .../MP+polocks.litmus (Never): spin_lock / spin_unlock as acquire / release |
| sb_kernel_atomic_mb | BOUNDED | linux-kernel.def (smp_mb__before_atomic, smp_mb__after_atomic as fences); Documentation/explanation.txt ("oddball fences"); Documentation/atomic_t.txt. Expected = SB with a full barrier on each side |
| sb_kernel_atomic_nofence | FAILED | .../SB+poonceonces.litmus (Sometimes), with the stores and loads as relaxed and an unordered atomic_inc between them (Documentation/atomic_t.txt: no implied barrier) |

The downgrade rule is tested on both sides: mp_kernel_relacq (release/acquire MP, no LB shape, no
smp_rmb) keeps its plain BOUNDED, and lb_relaxed (C11, not kernel mode) keeps its RC11 BOUNDED.
lb_kernel_mb_ctrl keeps its BOUNDED because a full barrier breaks its LB shape in one thread.

Not in the table, by design: LB with dependencies (dependencies are not ordering here, section 4).

Existing tests that already cover parts of this: c11_sb_ra, c11_mp_ra, c11_sc_fence_sb,
tso_sb (tests/conc).

## 6. Results

Gate `-tc='*litmus*'` on pir-w3-memory-model (tag W3r): `[doctest] test cases: 2 | 2 passed | 0 failed | 1398 skipped`,
`[doctest] assertions: 81 | 81 passed | 0 failed |`, `[doctest] Status: SUCCESS!`. The 20 files in tests/conc/litmus
(the 17 earlier ones plus mp_kernel_spinlock, sb_kernel_atomic_mb, sb_kernel_atomic_nofence) each match their EXPECT line.
An earlier note here claimed "all 17 files match" before any gate ran on this branch; that claim is replaced by this run.

A BOUNDED result is never PROVED: bounded rounds/unwinding. Harness note: the EXPECT value is read up to the first non-capital character.
