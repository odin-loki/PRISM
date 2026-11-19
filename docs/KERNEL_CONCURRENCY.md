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
| `__prism_sleep_point()` | Emitted by the spec of every sleeping primitive. Checks ctx == PROC and atomic-depth == 0 (SLEEP-IN-ATOMIC). |

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
- **SLEEP-IN-ATOMIC**: `might_sleep` / sleeping primitive while
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
| SLEEP-IN-ATOMIC | `sleep_in_spin_true.c` | `sleep_after_unlock_false.c` |
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

## 11. Implementation finding (phase A attempt, 2026-11-16)

The first attempt put the ghost lock set in C globals of the Linux lock spec
(`static const void *__prism_held[8]`, `static int __prism_atomic`) and raised
the checks with `__prism_check`. The gate (`*kernel*`, branch pir-w7-concurrency)
failed:

- `__prism_check` needs literal class and message strings. A check inside a
  helper with a `const char *` parameter is UNENCODED (NEEDS-HARNESS).
- The translator does not carry writes to a model's mutable globals from one
  library-model call to the next. Reads of the held set then need values other
  than the initialisers, so `lock_ok`, `trylock_ok` and `spin_irqsave_ok` came
  back NEEDS-HARNESS (not PROVED-ASSUMING), with the note "the global state the
  function is called in is an unstated precondition".

Consequence: phase A cannot be done in the spec layer alone. The ghost state
has to be frame-local state of the analysed function in `pir::translate` (a
fixed slot array allocated with the frame, zero at entry, read and written by
the `__prism_lock_*` intrinsics of section 2.1). The spec change and the test
case were reverted so the existing kernel tests stay green. The toy file
testdata/pir/kernel_lock_ctx.c is kept for that step.

The pilot (e1000e, em) was not run: the checks are not live, so it would report
nothing new.

## 12. Implementation (phase A, frame-local ghost state)

The ghost state is frame-local state of the analysed function in pir::translate
(src/prism/pir/translate_lock.inc, hooks in translate.cpp): one hidden static object
(8 lock slots, atomic depth, overflow flag), zero at entry, allocated in the prologue when
the call graph reaches a ghost intrinsic. The lock specs call __prism_lock_acquire(p, spin),
__prism_lock_release(p, spin) and __prism_sleep_point(); the checks are ordinary PRISM
failure checks: LOCK-DOUBLE, LOCK-UNHELD-UNLOCK, LOCK-HELD-ON-RETURN (at each return of the
analysed function; entry set empty), SLEEP-IN-ATOMIC (mutex_lock*, __might_sleep, msleep,
usleep_range while a spinlock is held). Overflow past 8 locks is the named assumption
linux-lock-overflow (the release check is then not reported). FreeBSD mtx and exclusive sx
locks are tracked as sleep locks (no atomic depth). Not done: entry contracts,
hardirq/softirq context, RCU, preempt_disable depth, lock order, LOCK-CTX-MISMATCH.

## 13. Implementation (W7r: contracts, preempt/bh, RCU)

Same ghost object as section 12, now with: the exit contract (words K..2K-1),
the preempt/bh count (word 2K+1), the RCU nesting (word 2K+2). Intrinsics:

| intrinsic | where it is called | checks |
|---|---|---|
| `__prism_lock_entry(p, spin)` | analysed code (contract marker), root frame only | p held on entry (caller's lock); spin: atomic depth +1 |
| `__prism_lock_exit(p)` | analysed code (contract marker), root frame only | p must be held at every return: LOCK-EXIT-MISSING if not |
| `__prism_atomic_add(delta)` | `preempt_count_add/sub`, `__local_bh_disable_ip/enable_ip` | PREEMPT-UNBALANCED below zero; not zero at a return |
| `__prism_rcu_add(delta)` | `__rcu_read_lock/unlock` | RCU-UNBALANCED below zero; RCU-HELD-ON-RETURN |
| `__prism_sleep_if(may)` | `lx_alloc` (kmalloc with __GFP_DIRECT_RECLAIM) | SLEEP-IN-ATOMIC; RCU-SLEEP (`synchronize_rcu` is a sleep point too) |

LOCK-HELD-ON-RETURN now means: a held lock that the function's exit contract does not
declare. An inlined callee's markers are no-ops (its lock operations act on the caller's
state); the contract is checked only where the function is the analysed root. Assumption
`linux-lock-contract` names the contract as trusted by the callers.

Not done (left): local_irq_save / arch_local_irq_* (inline asm in kernel headers: not
seen by the depth), hardirq and softirq contexts and LOCK-CTX-MISMATCH, lock order
(LOCK-ORDER, phase B), RCU grace periods, a contract table for unmodified driver code
(the markers must be in the analysed function, so helpers in a real driver stay
LOCK-HELD-ON-RETURN until a contract source exists).

## 14. Implementation (W7b: lock-contract table, lock order)

**Lock-contract table (`--lock-contracts FILE`).** A TSV for unmodified code: `function TAB lock TAB action [TAB kind]`,
with `lock` = `global:NAME` (a module global's address) or `arg:N` (0-based parameter), `action` = `acquires` (the
function returns with the lock held), `releases` (the caller holds it on entry) or `requires-held` (both), and the
optional `kind` = `mutex` (default) or `spin`. The table is applied to the analysed (root) function only, at its entry
block (translate_lock.inc `lock_table`), so the same declarations the markers make (`__prism_lock_entry`,
`__prism_lock_exit`) are made without editing the source. Assumption `lock-contract-table` names the trust. Needs
`--kernel-api`. Toy: `testdata/pir/kernel_lock_table.c` with `.tsv`. The e1000e table is `tools/pir/lock_contracts/e1000e.tsv`
(the NVM mutex acquire and release halves of ich8lan.c).

**Lock order (LOCK-ORDER-INVERSION).** A module pass over the unit's IR (`src/prism/pir/lock_order.cpp`, run by the pir
stage with `--kernel-api`). Lock classes are module global names (the built-in lock calls and the table's acquires and
releases give the events; table `arg:N` takes the caller's operand class). Each defined function is explored from its
entry with an ordered held set, calls to defined functions are inlined (depth 3, memoised), and each acquire of C with H
held records the pairs (h, C). A pair recorded in both orders is one LOCK-ORDER-INVERSION finding on the function that
takes the second order, with the other function in `extra.other_function`. Bounds: 200000 (block, state) steps per
unit, then exploration stops silently; cycles of length three or more are not reported; no interleaving (across
contexts) is modelled. A clean run is therefore not a proof of lock order. Toy: `kernel_lock_order_true.c` (AB and BA,
one through a callee) and `kernel_lock_order_false.c` (consistent order).

Left: hardirq and softirq contexts (LOCK-CTX-MISMATCH), cycles longer than two, cross-unit order, the FreeBSD `em` pilot.

## 15. Implementation (W7c: interrupt context, lock-order cycles)

Same pass as section 14 (`lock_order_report`, `src/prism/pir/lock_order.cpp`). Two additions.

**Lock-order cycles.** The recorded (held, acquired) pairs form a class graph. Every simple cycle of three to six
classes is one LOCK-ORDER-INVERSION on the function that closes it (`extra.cycle`, `extra.cycle_functions`). Length-two
cycles remain the pair findings. Bounds: 100 cycles, 100000 expansions, then the search stops silently.

**Interrupt context.** The handler of `request_irq`, `request_threaded_irq`, `devm_request_irq`,
`devm_request_threaded_irq` (operand 1 or 2) and the filter of `bus_setup_intr` (operand 3) runs in hardirq. The
handler and its defined callees (depth 3) are walked:
- a sleeping primitive reached there is SLEEP-IN-ATOMIC with `extra.context = hardirq` (kmalloc-family calls only when
  the GFP operand is a constant with `__GFP_DIRECT_RECLAIM`);
- a spinlock class taken there (spin acquires, contract table entries of kind spin) that a function outside every
  hardirq context takes without interrupt disabling (`spin_lock`, `_raw_spin_lock`, `spin_lock_bh`) is
  LOCK-CTX-MISMATCH: lockdep's inconsistent {IN-HARDIRQ-W} -> {HARDIRQ-ON-W}.

Not done (left): softirq context (tasklets, bh handlers), the threaded handler (runs in process context, so it is not
hardirq), local_irq_disable regions (inline asm, not seen), classes passed as parameters on the process side (only
module globals are matched there), a function reached from both contexts (treated as hardirq only), cross-unit order.
Toy: `testdata/pir/kernel_lock_irq_true.c` and `kernel_lock_irq_false.c`; cycles: `kernel_lock_cycle_true.c` and
`kernel_lock_cycle_false.c`.

## 16. Triage item 6: lock scopes, guard(), caller-held locks (`--pir-lock-scopes`)

The r1 FALSE triage (OS-Proving `data/coverage/prism_false_triage/SUMMARY.md`, pattern 5) found lock findings that
are modelling gaps, not defects. `--pir-lock-wrappers` (#70) already covers a whole function that only acquires or only
releases. `--pir-lock-scopes` (with `--kernel-api`) covers the rest. Each part is exact or a named assumption, and
imbalances whose two ends are both visible are still reported.

**Lock classes through scopes (lock-order pass, exact).** A value's lock class is followed through:
- a `getelementptr` with all-zero indices (`&lock->rlock` is the lock);
- a call of a unit function that returns one of its parameters (`spinlock_check`, `class_mutex_constructor`);
- an `alloca` to which only one lock class is stored (the `guard()` / `scoped_guard` variable). Its address is passed
  to the cleanup (`class_mutex_destructor`), and the load through it there has the class again.

So the scope-exit release of a `guard(mutex)` ends the hold. Without it, the lock looked held across the rest of the
function and produced false ABBA pairs "in class_mutex_constructor". The same rules give `spin_lock_irqsave`'s
`spinlock_check(lock)` its class. A slot with two classes gets none.

**Constant branches (lock-order and hardirq walk, exact).** A `br` on a constant, or on an `icmp` of two integer
constants, takes only its live edge. After mem2reg, `readl_poll_timeout(..., sleep_us = 0, ...)` has
`if (__sleep_us) usleep_range(...)` as `icmp ne i64 0, 0`, so the sleep is dead code and is no longer reported as a
hardirq sleep. With a non-zero `sleep_us` it still is.

**Per-lock caller-held / returned-held (ghost lock state).** `lock_sites` (`lock_order.cpp`) walks the analysed
function's call tree. It records every acquire and release made through PRISM's lock models by lock class: a global's
name, followed as above. `lock_scopes_entry` (`translate_lock.inc`) then handles each global lock:
- *Released somewhere, acquired nowhere in the tree* (`probe_irq_off` releasing `probing_active`): the caller holds
  it on entry. It is put in the held set at entry, with atomic depth when the release is a spin release. Its release
  is then not LOCK-UNHELD-UNLOCK, but a second release is, and sleeping before the release is SLEEP-IN-ATOMIC. Named
  `lock-scope: @f releases @L and acquires it nowhere in its call tree: its caller holds @L on entry ...`.
- *Acquired somewhere, released nowhere*: it is returned held by design, so it is not LOCK-HELD-ON-RETURN.
  LOCK-DOUBLE still applies. Named `lock-scope: @f acquires @L and releases it nowhere ...`.
- *Acquired and released somewhere* (both ends visible): checked on every path as before. A path that leaks it is
  LOCK-HELD-ON-RETURN, and a path that drops it unheld is LOCK-UNHELD-UNLOCK.

Nothing is called release-only while any acquire in the tree has an unclassified lock (a parameter-derived or
array-indexed lock, an indirect call, an intrinsic in analysed code), and the reverse also holds. So an unseen end
never hides an imbalance. RCU and preempt/bh have no lock address, so they are per function:
- a tree that only leaves RCU sections starts at nesting 1 (a second `rcu_read_unlock` is still RCU-UNBALANCED);
- one that only enters them is not RCU-HELD-ON-RETURN;
- one that only disables preemption / bh is not PREEMPT-UNBALANCED at the return.

**Ghost state across memory havocs (always on, exact for asm).** An asm "memory" clobber or an `--pir-icall-havoc`
call makes every object's contents arbitrary. PRISM's ghost objects (the lock / context state, the allocation-failure
flag) are not program memory, so they keep their values across the havoc. Before, a `writel` between `spin_lock_irqsave`
and `spin_unlock_irqrestore` (`sc520_wdt` `wdt_config`) erased the held set, and the release looked unheld. For
`--pir-icall-havoc` this is named: the arbitrary callee leaves the lock and context state as it found it.

**Condition-code outputs (always on, exact).** An inline asm output `"={@cc<cond>}"` is 0 or 1. Clang states this
with an `llvm.assume(icmp ult %r, 2)` right after the asm (`btq` in `test_bit`, FreeBSD's `atomic_fcmpset` in
`mtx_lock` / `sx_xlock`). The asm-havoc model made it an arbitrary byte, so all 102 `assume` FALSE rows (ASSUME/*)
were that assume failing. The havoc, and the free flag of `asm-exact`, are now limited to {0, 1}.

**sx locks / OpenZFS mutexes (models, exact).** `_sx_xlock` returns 0 (taken) unless `opts` has `SX_INTERRUPTIBLE`
(0x40). Only then can `_sx_xlock_hard` return an error (`kern_sx.c`). OpenZFS `mutex_enter` (`sx_xlock`, opts 0) used
to look like it might fail, so the paired `mutex_exit` was LOCK-UNHELD-UNLOCK. `sx_try_xlock_` and `sx_try_xlock_int`
(`mutex_tryenter`) are now modelled: taken (tracked) or 0.

Not done (left):
- path-sensitive lock order (a lock taken and released under two separate tests of the same condition: rose route
  locks, `binder_dec_node_tmpref`);
- conditional-acquire wrappers that return with the lock held only on success (`qcuefi_acquire`). These have no
  return-value contract and stay FAILED, as the real-imbalance test requires;
- locks in arrays or reached through parameters for the per-lock rule.

Toys: `testdata/pir/kernel_item6_linux.c` (scopes), `kernel_item6_freebsd.c` (sx), `kernel_item6_scopes.c` (guard,
constant branches); tests `tests/cpp/test_pir_triage_item6.cpp`.
