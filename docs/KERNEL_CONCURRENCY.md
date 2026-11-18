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

## 16. Implementation (W7d: struct-field lock classes)

Lock classes are no longer module globals only (`src/prism/pir/lock_order.cpp`, `field_class`). A lock whose
address is a struct field (a getelementptr with constant field indices, e.g. `&adapter->stats64_lock`) gets the
lockdep-style class `STRUCT.#INDEX`: the struct type that directly contains the member and the member's index
(the last struct descended into, so a nested member is keyed by its innermost struct). Every instance of the struct
shares the class. The class is the same for lock order (pairs and cycles), LOCK-CTX-MISMATCH and the contract
table. A parameter keeps the class its caller passed (`arg:N` as before); the pointee type of a parameter is not
inferred (opaque pointers), so a lock reached only through a parameter has no class of its own.

**Contract rows.** `field:TYPE.INDEX` in the lock column names a fixed field class (`field:dev.0` is `dev.#0`).
It feeds the lock-order pass only: the translator's ghost lock state (translate_lock.inc) skips these rows.

**Same-class nesting** (two adapters, both `adapter.#0`) is not an inversion (lockdep nesting rule). `acquire()`
ignores a class that is already held, so no pair is recorded and an opposite nesting across functions is not
reported. There is no explicit nested annotation yet (left).

**Limits (left).** No field names: the IR of this pipeline carries no debug information for struct members, so
`field:` takes the index, and a name in the table is a parse error. Byte-offset getelementptrs (container_of's
`i8` arithmetic) give no class; the class comes from the later struct getelementptr that selects the member.
Anonymous (literal) structs give no class.

Toy: `testdata/pir/kernel_lock_field_order_{true,false}.c` (AB/BA through two fields, and consistent),
`kernel_lock_field_irq_{true,false}.c` (hardirq handler and a plain spin_lock in process context, and irqsave),
`kernel_lock_field_nest.c` (same-class nesting), `kernel_lock_field_table.c` with `.tsv` (field contract rows).

### 16.1 Coverage of the lock-order pass (what "0 findings" covers)

`PRISM_LOCK_COVERAGE=1` makes the pir stage print one line per unit on stderr (`LOCK-COVERAGE unit=... acquire_sites=... `, and
`LOCK-COVERAGE-UNMODELLED` lines); it is a diagnostic only and adds no finding. `LockOrderReport::coverage` (`LockCoverage`,
include/prism/pir.hpp) carries the same numbers. Terms: an *acquire site* is a call to a lock primitive of the pass
(`mutex_lock`, `spin_lock`, `mtx_lock`, ... or a `--lock-contracts` row) in a defined function; *resolved* means the
exploration gave it a class (a global, a struct-field class, or the class a caller passed); *field_geps* are getelementptrs
that `field_class` could name; *unmodelled* are calls to undefined functions whose name contains lock, mutex, sem_, down or
up_ and which the pass does not treat as acquire or release.

Measured on the 11 e1000e units and the 20 em units that finished (19 Nov, 2 s query timeout, `--kernel-api`, the e1000e unit
set with `tools/pir/lock_contracts/e1000e.tsv`; one em unit, `em_txrx.c`, hit the 900 s unit timeout in the stage and has no line):

| | e1000e (Linux, 11 units) | em (FreeBSD, 20 of 21 units) |
|---|---|---|
| defined functions analysed | 828 | 971 |
| acquire sites / resolved | 13 / 13 (2 global, 11 struct-field) | 0 / 0 |
| release sites / resolved | 14 / 14 | 0 / 0 |
| distinct struct-field classes | 3 (`e1000_adapter.#67`, `e1000_adapter.#98`, `net_device.#195`) | 0 |
| distinct global classes | 2 | 0 |
| class pairs recorded (lock held while another is taken) | 0 | 0 |
| contract-table call sites (`e1000e.tsv`) | 0 | n/a |
| getelementptrs / named as a struct field | 7,356 / 6,652 | 12,838 / 12,407 |
| exploration steps (limit 200,000 per unit) | 20,517, limit not hit | 28,959, limit not hit |

Reading: both results are zero findings because the pass found no lock taken while another is held (0 class pairs), not
because it checked many orders. On e1000e the 13 lock sites are all resolved (netdev.c 6, ptp.c 5, one global mutex each in
82571.c and ich8lan.c); no site is unresolved. The two table rows never fire: the functions they name are called only through
the `.acquire` and `.release` members of the NVM ops tables (ich8lan.c lines 5984-5996), which the pass does not follow (indirect calls). Lock-like calls the pass does not model:
`rtnl_lock` 3 and `rtnl_unlock` 4 (a global mutex, not modelled), the `*_locked` helpers (callee holds or needs a lock, defined in another unit),
`lockdep_init_map_type` 2 and `__raw_spin_lock_init` 2 (init, not an acquire). On em the pass sees no site at all:
the one lock use in the unit set is `sx_xlock(iflib_ctx_lock)` in `em_print_nvm_info`, which FreeBSD expands to an inline atomic
fast path plus the call `_sx_xlock_hard` (unmodelled, with `_sx_xunlock_hard`); the `mtx_lock` family expands to
`__mtx_lock_flags` or an inline fast path with `__mtx_lock_sleep(&m->mtx_lock, v)`, whose operand is the lock word of the
struct, not the lock object, so naming them would put every mutex in one class (`mtx.#1`) and invent inversions. Not done (FreeBSD mtx_lock macro read in sys/sys/mutex.h line 176 and 181).
em's lock order is therefore not checked by this pass at all.
