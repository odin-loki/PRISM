# Kernel concurrency and interrupt contexts (W7)

Status: design, first draft (2026-11-16). Base: origin/integration-wave1.
Scope from docs/FUTURE_FULL_PROOF_SCOPE.md section 3 and the W7 brief in
the wave plan: process, softirq, hardirq and NMI contexts; lock discipline;
RCU read-side sections; preemption and migrate-disable; per-CPU data.

## 0. What exists

| piece | where | what it does for kernel code |
|---|---|---|
| kernel lock specs | `src/prism/pir/models/kernel/linux_lock.cpp`, `freebsd_lock.cpp` | Take or release a lock by a no-op. Every spec states `ASSUME("linux-lock: lock state is not tracked ...")`. No balance, double-lock, order or context check. |
| RCU and might_sleep specs | `linux_lock.cpp` (`__rcu_read_lock`, `synchronize_rcu`, `__might_sleep`) | Empty. Named assumptions `linux-rcu`, `linux-might-sleep`. |
| conc stage | `src/prism/conc/` (`docs/CONCURRENCY.md`) | Bounded context switches between the threads a harness creates. Reports `CONC-DATA-RACE`, `CONC-DEADLOCK`, `LOCK-DOUBLE-UNLOCK` for pthread/C11 mutexes on global state. It knows nothing about kernel primitives, contexts or per-function contracts. |
| module contracts | `docs/DRIVER_CONTRACTS.md` | Names the lock/context rules a module must satisfy (which context may call what, what the interrupt handler may assume during teardown). Not yet machine-readable. |

So a kernel function today is analysed as if it ran alone, with its locks
invisible. Lock bugs in drivers are exactly the bugs that this hides.

## 1. Contexts

A kernel function runs in one of four contexts. The context is a property of
the call path, not of the function, so it is a per-call-site requirement and
is checked against the caller's context.

| context | may sleep | may take a sleeping lock (mutex, rwsem, sx) | may take a spinlock | may be preempted | may take a process-context lock it can also contain |
|---|---|---|---|---|---|
| process | yes | yes | yes | yes (unless preempt-disabled) | yes |
| softirq / bh | no | no | yes | by hardirq only | no, only with `_bh` locks |
| hardirq | no | no | yes, with `_irqsave` | by NMI only | no |
| NMI | no | no | only NMI-safe locks (none in the model) | no | no |

Ghost state: one variable `__prism_ctx` in the PIR of the analysed unit,
with values `PROC`, `SOFTIRQ`, `HARDIRQ`, `NMI`, plus an atomic-depth counter
(`preempt_count`-style: spin locks, `preempt_disable`, `rcu_read_lock` in
non-preemptible kernels, `local_irq_disable`, the irq-saving forms).
`might_sleep()` is a check, not an annotation: it fails when atomic-depth > 0
or ctx != PROC.

Entry contracts. A function's context at entry is given by a spec annotation
(`requires-ctx: process` in the comment, read by the spec tool, or by a
module contract row). Default when not given: `unknown`. Unknown entry
context is not checked for sleep, it is reported as an assumption
(`ctx-entry-unknown`) so the result cannot be read as "no sleep in atomic".

## 2. Lock discipline

### 2.1 Ghost lock set

A bounded ghost array `__prism_held[K]` of (address, class-id, kind, ctx)
entries, `K = 8` per function (overflow is UNKNOWN, not a pass). The
lock arguments are symbolic addresses; equality is SMT equality on the
pointer value (already in the PIR memory model).

New spec intrinsics (no-ops in real code, handled by `pir::translate`):

| intrinsic | effect |
|---|---|
| `__prism_lock_acquire(p, cls, kind)` | Checks `p` not in held set (LOCK-DOUBLE). Adds `(p, cls, kind)`. Sets atomic-depth += 1 for spinlock kinds. |
| `__prism_lock_release(p, kind)` | Checks `p` in held set (LOCK-UNHELD-UNLOCK). Removes it. atomic-depth -= 1 for spinlock kinds. |
| `__prism_lock_entry(p, cls, kind, state)` | Declares a lock held (`state=HELD`) or free (`FREE`) on entry, from the contract. |
| `__prism_ctx_enter(c)` / `__prism_ctx_leave()` | Context change for a call (e.g. an interrupt handler body). |
| `__prism_sleep_point()` | Emitted by the spec of every sleeping primitive. Checks ctx == PROC and atomic-depth == 0 (CTX-SLEEP-ATOMIC). |

The kernel lock specs then call these intrinsics instead of
`ASSUME("lock state is not tracked")`. The named assumption stays for the
parts still not modelled (data effects of the lock word, lock ordering
across functions until phase B).

### 2.2 Per-function properties

Each function is checked for:

- **LOCK-DOUBLE**: acquire of a lock already in the held set (self-deadlock
  for mutexes and spinlocks, since neither is recursive).
- **LOCK-UNHELD-UNLOCK**: release of a lock not in the held set, after the
  entry declarations.
- **LOCK-HELD-ON-RETURN**: at each `ret`, the held set must equal the entry
  set. A lock taken and not released, or a lock released that the contract
  says the caller holds (`HELD` on entry, no `__prism_lock_entry`
  declaration), is a violation of the function's own contract.
- **CTX-SLEEP-ATOMIC**: `might_sleep` / sleeping primitive while
  atomic-depth > 0 or ctx != PROC.
- **LOCK-CTX-MISMATCH**: a lock whose class was declared process-only
  (a mutex) acquired in ctx != PROC, or a spinlock whose class is also taken
  in hardirq/softirq and acquired there without the `_irqsave`/`_bh` form.
  The second needs the set of contexts a class is used in, so it is a
  module-level check (section 4), not per function.

Each violation is a property (same mechanism as `FUNC-CONTRACT`), so the
verdict is FAILED with the counterexample path, never PROVED. A clean run is
PROVED-ASSUMING with the named lock assumptions.

### 2.3 Lock order (ABBA)

Lock classes come from the lockdep annotations already in the spec layer:
the `lock_class_key *key` argument of `__mutex_init`, `__raw_spin_lock_init`,
and the FreeBSD `MTX_*` name. A class is a stable id per key address.

Per function, the checker records the order pairs (class held, class
acquired). The module-level pass (section 4) builds the union graph and
reports cycles as LOCK-ORDER with the two call paths. Ordering needs the
calls to be resolved, so it depends on W5 (ops tables) for callbacks and on
the device-model entry table.

Ordering across threads (two contexts racing on two locks) is an
interleaving question. It is phase B and needs W3 (memory model) for the
lock-word ordering and the acquire/release semantics of `spin_lock` (the
acquire barrier is what makes the critical sections ordered).

## 3. RCU

- Read side: `rcu_read_lock` / `rcu_read_unlock` increment and decrement a
  per-context nesting counter `__prism_rcu`. Counter goes negative: RCU-UNBALANCED.
  Counter > 0 at return: RCU-HELD-ON-RETURN (same rule as locks).
- Sleeping inside a read-side section: `might_sleep` while `__prism_rcu > 0`
  is RCU-SLEEP (the classic bug; a preemptible-RCU kernel still forbids
  sleeping in a read section).
- Grace periods: `synchronize_rcu` (and `call_rcu` callbacks) cannot be called
  from a read-side section (RCU-SYNC-IN-READ), and cannot be called from
  atomic context (it sleeps).
- Use-after-free across a grace period: a pointer loaded under
  `rcu_dereference` is valid until the read section ends. Modelled as a named
  assumption `linux-rcu-gp` (object freed only after a grace period), not
  checked. This is a model assumption, not a result, and the verdict must
  list it.
- Publish/subscribe ordering (`rcu_assign_pointer` release, `rcu_dereference`
  consume): the ordering half is W3 work and is phase B.

## 4. Module pass (no model involved)

A module skeleton (device-model work, `pir-device-model`) gives the registered
entry points and their roles (probe, remove, ISR, workqueue, timer, file ops).
The module pass:

1. Assigns each entry point its context from the registration role:
   probe/remove/open/read: PROC; ISR (`request_irq` handler): HARDIRQ; tasklet
   and `timer_list` callbacks: SOFTIRQ; workqueue work: PROC.
2. Walks the call graph from each entry (bounded depth) and, per call edge,
   checks the callee's context requirement against the caller's (CTX-*).
3. Builds the lock-class usage table: for each class, the set of contexts it
   is taken in. A class taken in HARDIRQ or SOFTIRQ and in PROC without
   irq-saving is LOCK-CTX-MISMATCH (the classic IRQ-deadlock).
4. Builds the lock-order graph (section 2.3) and reports cycles.

The module pass reads only the per-function records (held set at each call,
entry declarations) and the call graph. It runs after the per-function pass,
so the verdict of a module is the conjunction of the per-function verdicts
and the module findings.

## 5. Preempt, migrate and per-CPU data

- `preempt_disable/enable`, `migrate_disable/enable`: counters in the same
  atomic-depth ghost. Unbalanced exit: PREEMPT-UNBALANCED.
- Per-CPU data: `this_cpu_*`, `per_cpu_ptr`, `get_cpu_ptr`: must run with
  preemption or migration disabled, or in a context where the CPU cannot
  change. Check: PERCPU-MIGRATABLE when a per-CPU access happens in PROC with
  atomic-depth == 0 and no migrate-disable. `smp_processor_id` in PROC is the
  same check (a warning under CONFIG_DEBUG_PREEMPT, modelled as a finding).
- Per-CPU data shared between contexts on the same CPU (softirq and process
  on one CPU) is a per-CPU race covered by the interleaving pass (phase B).

## 6. Mapping to the existing `conc` stage

The `conc` stage stays the interleaving checker. Kernel contexts are added
as threads in the same bounded schedule, with an extra rule:

- A hardirq or softirq "thread" is created for each registered handler. It
  may run at any visible operation of a PROC thread while that thread's
  irq-enabled bit (ghost, set by `local_irq_enable`, cleared by the irq-saving
  forms) is set, and it may not run while the handler's atomic depth
  conflicts (softirq disabled).
- Only lock and unlock on a *global* lock are visible today. Kernel locks
  are mostly embedded in structs (`struct mutex` in `dev`), so the conc
  stage needs the address-based lock identity from section 2.1. This is the
  cheapest route: the conc stage already keeps a mutex owner per global;
  the kernel version keys owner by pointer value.
- Heap and struct data stays NEEDS-HARNESS until the PIR memory model hook
  (roadmap 2.5) lands, so a driver's per-device struct accesses cannot be
  raced in phase A. This is a stated limit, not a pass.

## 7. Phases and dependencies

Phase A (does not need W3): sections 2.1, 2.2, 3 (sleep-in-read, counters),
4 steps 1, 2, 3 (context table and LOCK-CTX-MISMATCH), 5. Lock classes from
annotations.

Phase B (needs the W3 PR, and W5/device-model tables): 2.3 lock order across
functions, section 3 publish/subscribe ordering, interleavings of irq
handlers with process code in the conc stage (section 6), per-CPU races.

## 8. Toy tests (per bug class, true and false variant)

Under `tests/conc/kernel/`, one C file per variant, each with an expected
verdict line. Names:

| class | true (must report) | false (must not report) |
|---|---|---|
| LOCK-DOUBLE | `double_lock_true.c` | `double_lock_false.c` (unlock then relock) |
| LOCK-UNHELD-UNLOCK | `unheld_unlock_true.c` | `unheld_unlock_false.c` (unlock of an entry-HELD lock with declaration) |
| LOCK-HELD-ON-RETURN | `held_on_return_true.c` (error path skips unlock) | `held_on_return_false.c` (goto-unwind releases) |
| CTX-SLEEP-ATOMIC | `sleep_in_spin_true.c` | `sleep_after_unlock_false.c` |
| LOCK-CTX-MISMATCH | `irq_deadlock_true.c` (module of two entries) | `irq_deadlock_false.c` (irqsave on the PROC side) |
| RCU-SLEEP | `rcu_sleep_true.c` | `rcu_sleep_false.c` |
| RCU-SYNC-IN-READ | `rcu_sync_true.c` | `rcu_sync_false.c` |
| PREEMPT-UNBALANCED | `preempt_true.c` | `preempt_false.c` |
| PERCPU-MIGRATABLE | `percpu_true.c` | `percpu_false.c` (inside preempt_disable) |

Phase B adds `lock_order_abba_true.c` / `_false.c` and an interleaving case
for the conc stage.

Gate for phase A: each true file reports the named class and no other class;
each false file reports nothing new; the existing `tests/cpp/test_conc.cpp`
and `testdata/conformance/concurrency/` results are unchanged.

## 9. Pilot drivers and triage

Pilot inputs: Linux e1000e (`D:\ParanoidBSD\clang-work\subsets\compile_commands-linux-e1000e.json`)
and FreeBSD `em` (`compile_commands-freebsd-e1000.json`). Each finding is
triaged as one of:

- **real bug**: the code violates the lock/context rule and the path is
  feasible (confirmed by reading the path, not only the model);
- **model artefact**: the spec or the entry context is wrong (e.g. a callee
  releases a lock its caller holds, which is a contract the spec lacks);
- **missing spec**: a primitive is not in the model, so its lock/context
  effect is unknown.

Expected initial results: many LOCK-HELD-ON-RETURN artefacts until module
entry contracts are written, because helper functions take a lock the caller
holds. That is a missing contract, not a bug, and the triage must say so.

## 10. Risks and limits

- A per-function check cannot know the caller's held set. Default for an
  undeclared lock argument is FREE on entry, which over-reports helpers. The
  default is reported as assumption `lock-entry-unknown` and never as a clean
  pass.
- The held-set bound (K = 8) makes deep nesting UNKNOWN.
- Lock identity by pointer value is exact in-function but not across
  functions without the module pass.
- No claim about RCU grace periods or memory ordering before W3.
