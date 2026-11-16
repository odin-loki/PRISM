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
| `cmpxchg`, `xchg`, `atomic_*_return` | RMW with the success/failure orders | the kernel default for value-returning RMWs is fully ordered (seq_cst) |
| `atomic_read` / `atomic_set` | relaxed | |
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

1. READ_ONCE / WRITE_ONCE (volatile accesses) and the smp_* barriers (`__atomic_thread_fence`,
   `__atomic_load_n` / `__atomic_store_n`) ARE now recognised by the conc extractor, provided the harness
   defines the macros that way (tests/conc/litmus/*kernel*.c). Not modelled: kernel macros that
   expand to inline asm, dependencies (address/data/control dependency ordering is not tracked:
   `rcu_dereference` as acquire is the stand-in), `smp_mb__before_atomic` and friends, locks as
   LKMM primitives, and load buffering (LB+poonceonces is Sometimes in the LKMM, the
   promise-free model forbids it: it cannot report that outcome). A plain (non-volatile)
   shared access is still race-checked, as before.
   Pointers: address values are not modelled (ptrtoint is UNENCODED), so the RCU litmus tests
   publish an index instead of a pointer.
2. LL/SC pairs: no model. Reported UNENCODED.
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

Existing tests that already cover parts of this: c11_sb_ra, c11_mp_ra, c11_sc_fence_sb,
tso_sb (tests/conc).

## 6. Results

All 17 litmus files match their EXPECT line (gate `-tc='*litmus*'`: 2 test cases, 69 assertions, all passed). A BOUNDED result is never PROVED: bounded rounds/unwinding. Harness note: the EXPECT value is read up to the first non-capital character.
