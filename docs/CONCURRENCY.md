# Concurrency: the `conc` stage

Roadmap 2.6, "Threads and atomics". This is a separate stage that checks
programs that create threads. It uses lazy sequentialisation, the Lazy-CSeq
method, which bounds the number of context switches. Atomics are sequentially
consistent (SC) for now; relaxed, acquire and release orders come later. A
data race counts as a property violation.

| | |
|---|---|
| Stage | `conc`, placed after `pir` in `STAGE_ORDER` (both engines) |
| Engine | C++ only: `src/prism/conc/`, `include/prism/conc.hpp`. The Python engine is frozen (roadmap D8) and records one `NOTRUN` row: "C++ engine only (Python engine frozen as oracle, roadmap D8)" |
| Verdict audit | origin `solver` in `proofs/Prism/Verdict.lean`, `src/prism/verdict/verdict.cpp` and `prism/laws.py`. The stage **never** emits `PROVED*` |
| Executes scanned code | no. It runs clang/opt to compile, then Z3 in-process (Law 9: not in `EXEC_STAGES`) |
| Tests | `tests/conc/*.c`, `tests/cpp/test_conc.cpp` (doctest), `tests/test_conc.py` (end to end through `PRISM_BIN`), `tests/conformance/concurrency/` (scored by `tools/conformance.py`) |

## What it reports

A **harness** is `main` when `main` creates threads, directly or through
callees. Otherwise it is every function that creates threads and that no
other function calls. For each harness the stage reports:

| status | when |
|---|---|
| `FAILED` | A violation is reachable within the bound. There is one row per distinct violation, up to 8. `counterexample` holds the schedule and `evidence` holds the interleaving, one visible operation per line with the values read and written |
| `BOUNDED` | No violation within `K` rounds: "no data race, deadlock, assertion failure or UB within K round(s) of N thread(s) (loop unwind U); a context-switch bound is not a proof" |
| `NEEDS-HARNESS` | The program uses something the stage does not model. The reason is an `UNENCODED: ...` string, listed below |
| `UNKNOWN` | Z3 returned unknown or timed out |
| `NOTRUN` | Z3 was not built, or clang/opt were not found (Law 1, Law 7) |

A function that creates no threads gets no finding. A unit whose source does
not mention `pthread_create`, `thrd_create`, `std::thread` or `std::jthread`
is not even lowered.

The following are checked in every thread:

- **Data race** (`CONC-DATA-RACE`, CWE-362). Thread *t* accesses shared
  variable *v*, and the visible operation just before it, in the global
  order, was an access to *v* by another thread. At least one of the two
  accesses must be a write, and they must not both be atomic. Locks,
  unlocks, creates and joins are visible operations themselves. Two accesses
  ordered by one of these operations are therefore never adjacent, and are
  not a race. Two conflicting accesses that nothing orders can always be
  scheduled back to back, so this "adjacent across a context switch" check
  finds them, given enough rounds.
- **Every PIR property**. This covers assertions, `reach_error`, `abort`,
  and the signed-overflow, division, shift and poison checks. They come from
  `pir::translate` unchanged (`FUNC-CONTRACT`, `INT-SIGNED-OVF`, ...).
- **Deadlock** (`CONC-DEADLOCK`, CWE-833). At the end of a round, every live
  thread is blocked. A thread is blocked when it waits at a
  `pthread_mutex_lock` whose mutex is held (by anyone, itself included), or
  at a join of an unfinished thread. The program must not have exited.
- **Unlock of a mutex the thread does not hold** (`LOCK-DOUBLE-UNLOCK`).

Options (`conc::Options`):

| option | default | meaning |
|---|---|---|
| `rounds` | 2 | `K` round-robin rounds |
| `unwind` | `--unwind` | loop bound inside threads |
| `max_threads` | 4 | the most thread creation sites allowed |
| `timeout_s` | `--timeout` | timeout per solver query |
| `max_findings` | 8 | the most distinct violations reported per harness |

With `N` threads (the harness is `T0`), a run has `K·N` slots plus a final
slot for `T0`. That allows up to `K·N` context switches.

## Modelled API

| construct | model |
|---|---|
| `pthread_create(&h, NULL, f, arg)`, `thrd_create(&h, f, arg)` | Marks thread `h` active. `f` must be a defined function, and `arg` must be unused by `f`. Each creation site has a static thread index, and handles in local or global `pthread_t` variables are resolved to that index |
| `pthread_join(h, NULL)`, `thrd_join(h, NULL)` | Blocks until thread `h` has finished (`assume`). A non-null result pointer is `NEEDS-HARNESS` |
| `pthread_mutex_lock/unlock`, `mtx_lock/unlock` on a global mutex | Lock is `assume(owner == 0); owner = self`. Unlock checks `owner == self`, then sets `owner = 0` |
| `pthread_mutex_trylock`, `mtx_trylock` | Returns 0 and takes the mutex if it is free. Otherwise returns `EBUSY` (16) or `thrd_busy` (1) |
| `pthread_mutex_init(&m, NULL)`, `mtx_init` | `owner = 0` |
| `pthread_cond_wait(&c, &m)` | Modelled as unlock `m`, then a context-switch point, then lock `m`. POSIX allows spurious wake-ups, so this allows exactly the behaviours POSIX allows (`condvar_false.c` fails because of it). `signal`, `broadcast`, `init` and `destroy` have no effect |
| `pthread_exit`, `thrd_exit` in a thread entry | The thread returns |
| `exit`, `abort`, a failed assertion, `main` returning | The whole program stops, and no thread runs after it |
| scalar integer globals (`i1`..`i64`) | Shared state. Plain loads and stores are visible operations |
| `atomic_*` / `__atomic_*` / `__sync_*` with `seq_cst`: `load atomic`, `store atomic`, `atomicrmw` (`xchg add sub and or xor nand max min umax umin`), `cmpxchg` (strong; weak may also fail spuriously), `fence seq_cst` | Each is one atomic visible step. A `cmpxchg` or `atomicrmw` reads and writes in the same step |
| `sched_yield`, `thrd_yield` | No effect. Every schedule is explored anyway |
| `__VERIFIER_nondet_*`, `__VERIFIER_assume` | Symbolic value, and assumption |
| `__VERIFIER_atomic_begin/end`, calls to defined `__VERIFIER_atomic_*` functions (SV-COMP) | An atomic section. It is a pseudo-mutex, and while one thread holds it no other thread is runnable |

Not modelled. Each of these gives `NEEDS-HARNESS` with an `UNENCODED:`
reason:

- `memory_order_relaxed`, `acquire`, `release` and `acq_rel`, and
  `syncscope`. The message names the order. For example, `relaxed.c` gives
  "UNENCODED: memory_order_relaxed (SC only: relaxed/acquire/release atomics
  are not encoded yet)".
- Shared data that is not a scalar integer global: heap, arrays, structs,
  pointers, and mutexes that are not globals. These wait for the PIR memory
  model (roadmap 2.5). See "Memory model hook" below.
- A thread argument that the thread entry uses, and a join result pointer.
- A creation site inside a loop, or in a function called more than once
  (dynamic thread sets), and threads created by threads.
- `std::thread` and `std::jthread`, which lower through a virtual `_State`
  object, and OpenMP.
- Everything `pir::translate` does not encode, such as floating point, calls
  to unknown externals, and memory. Its own `UNENCODED:` reason is passed
  through, with the thread named.

## How it works

### 1. Front end: the PIR one, plus visible-operation markers (`src/prism/conc/extract.cpp`)

`pir::lower_to_ir` lowers the unit (clang `-O0`, then `mem2reg`,
`lowerswitch`, `loop-simplify`, `lcssa`, `instnamer`) and `ir::parse_module`
parses it. `pir::translate` models the memory of one function
(docs/PIR.md), not memory shared between threads. So the stage copies the
module and rewrites every *visible* operation into a **marker**, an
instruction that `translate` already understands and whose result name
`__prism.conc.<k>` indexes `Program::ops`:

```
load  iN @g            ->  %__prism.conc.k  = call iN @__VERIFIER_nondet_conc()      ; PIR Havoc = the value read
store iN v, @g         ->  %__prism.conc.k  = call iN @llvm.expect.iN(iN v, iN 0)    ; PIR Copy  = the value written
atomicrmw add @g, v    ->  read marker k; %n = add old, v; %__prism.conc.ks = expect(%n)
cmpxchg @g, c, n       ->  read marker k; eq = (old == c); new = eq ? n : old; write marker ks
lock / unlock / create / join / init -> copy marker (join copies the thread handle)
```

Everything else goes through `pir::translate` unchanged. That is why the PIR
UB checks and assertions apply in every thread. Thread entries lose their
unused `void *` parameter and their return value.

### 2. Thread graphs (`src/prism/conc/lazy.cpp`)

Each thread's PIR body is split so that every visible operation starts a
block. That start is a context-switch point. The body is then unrolled into a
DAG, with each loop run up to `unwind` times. A path that would go past the
bound is *cut*, meaning it is assumed away. This is fine because the verdict
is `BOUNDED` anyway. The nodes are numbered in topological order, and these
numbers are the Lazy-CSeq program-counter labels. `END` = number of nodes.

### 3. The sequentialised program as one formula

The driver runs `K` rounds. In each round, `T0 … TN-1` run in that order,
and then `T0` gets one final slot (as in Lazy-CSeq, so that the code after
the harness's joins runs). Each *slot* re-enters the thread's whole body:

```
runnable = active[t] && !exited && pc[t] != END
cs       = fresh;  runnable -> pc[t] <= cs <= END;  !runnable -> cs == pc[t]
reach(n) = runnable && (pc[t] == n  ||  OR_p (exec(p) && edge(p -> n)))
exec(n)  = reach(n) && n < cs
pc[t]'   = END if a return executed, else the n with reach(n) && n >= cs
```

This is Lazy-CSeq's `if (pc > L) goto saved; … L: if (L >= cs) { pc = L;
return; }` guarded-jump program, written as a formula. `lazy_text()` prints it
as pseudo-C for inspection.

**Thread-local values** are "static", as in Lazy-CSeq. Each SSA value
instance keeps its value from earlier rounds: `v = ite(exec(def), new, v)`.

**Shared state is threaded through the slots in order.** This covers
globals, mutex owners, pcs, the `active` flags, `exited`, and the "last
visible operation" record (thread, variable, write?, atomic?, op) that the
race check reads.

Assumptions are **prefix-guarded**. A violation is `ok ∧ cond`, where `ok`
is the conjunction of every assumption that comes *before* it in the global
order. A later blocking `assume` therefore cannot hide an earlier failure.

Z3 checks the disjunction of all violations. When the result is SAT, the
earliest violation in the model is reported with its interleaving. That
violation is then excluded and Z3 is asked again, up to `max_findings`.
Z3 answering UNSAT gives `BOUNDED`.

### 4. Memory model hook

Shared state is a vector of bit-vectors, `G[var]`, and the access rules are
in `Enc::visible`. When the PIR memory model lands (roadmap 2.5), a `Load`,
`Store` or `Rmw` marker will carry an (object, offset) pointer instead of a
global index. `G` will become the byte-array memory, and the race check will
compare addresses instead of variable indices. `Rewriter::shared_var` is the
single place where non-scalar shared data becomes `NEEDS-HARNESS` today.

## Correspondence with the Lean proof

**`N` threads, `K` rounds.** `proofs/techniques/PrismTechniques/LazySeqN.lean`
(docs/PROOFS_TECHNIQUES.md §5b) models the schedule this stage runs:
`prismSched N K` = `K` rounds of `T0 … T(N−1)` plus `T0`'s final slot
(`tests/test_proofs_float_conc.py` locks it to `lazy.cpp`). A thread there is
any transition system on its local state (`pc`, locals) and the shared state,
so the unrolled thread DAG with its node labels is an instance. Proved, under
sequential consistency:

- `lazy_sound`: every run of the sequentialised program is a real
  interleaving with `K·N` switches, so a `FAILED` schedule is real (modulo
  items 5–8 below);
- `lazy_covers`: every interleaving from `T0` with at most `K − 1` context
  switches in total is covered; `lazy_covers_runs`: more generally every
  schedule that splits into at most `K` increasing runs of thread ids;
- `lazy_covers_two`: with two threads (`T0` and one created thread), every
  interleaving with at most `2K` switches is covered;
- `per_thread_bound_not_enough`: for `N ≥ 3`, a bound on switches *per
  thread* is **not** enough. With `N = 3`, `K = 1`, the schedule `T0, T2, T1`
  (nobody preempted) reaches a state no run of `T0 T1 T2 T0` reaches. A
  `BOUNDED` verdict covers only schedules of round-robin shape.

Items 1–4 below are therefore covered by `LazySeqN` (item 4: loops inside a
thread are allowed; cutting paths past `unwind` only loses interleavings).
Items 5–9 remain outside any proof. The older two-thread proof follows.

`proofs/techniques/PrismTechniques/LazySeq.lean` (docs/PROOFS_TECHNIQUES.md
§5) proves the following, for **two** threads, each a **straight-line list
of atomic actions** `G → G → Prop` (actions may be nondeterministic or
blocking; thread-local variables live in `G`):

- `lazy_seq_covers`: every interleaving with at most `2K−1` context switches
  (finished or not) is matched by a run of the `K`-round sequentialised
  program;
- `lazy_seq_sound`: every such run is a real interleaving;
- the proof goes through the segment normal form `Seg`:
  `seqLR_iff_seg : SeqLR K ↔ Seg (2K) true` (alternating segments, thread 1
  first), `seg_of_star` / `star_of_seg` (interleavings ↔ normal forms).

How the implementation corresponds:

| Lean | implementation |
|---|---|
| an action `G → G → Prop` | The code a slot runs from one context-switch point to the next: one visible operation plus the invisible (thread-local) code after it, up to the next visible operation. Nondeterminism: `__VERIFIER_nondet_*`, `havoc`, the choice of branch. Blocking: `assume(owner == 0)` of a lock, `assume(finished)` of a join, `__VERIFIER_assume` |
| thread-local variables live in `G` | Value instances persist across rounds ("static locals"). Shared state (`G[var]`, owners, pcs) is threaded through the slots |
| `Seg n c p1 p2`: alternating segments; `Exec s` runs a segment; a segment may be empty (`Exec [] g g`) | A slot runs the thread from `pc[t]` to the chosen `cs`. `cs == pc[t]` is the empty segment. Slots alternate in the fixed order T0, T1, … |
| `SeqLR K` (eager guess-and-check: thread 1 runs all rounds on guessed states, then thread 2 checks the guesses) | Not used. The lazy scheme computes each round's state sequentially instead of guessing it. What the formula encodes is `Seg` itself, and Lean proves `Seg (2K) true ↔ SeqLR K` (`seqLR_iff_seg`) and `Seg ↔` interleavings (`seg_of_star`, `star_of_seg`) |
| `lazy_seq_covers` (coverage within `2K−1` switches) | For a harness `T0` and one created thread, with loop-free bodies (or loops within the unwind bound), every interleaving with at most `2K−1` switches is a model of the formula. A violation reachable in that interleaving is found |
| `lazy_seq_sound` (no spurious runs) | Every model is a real interleaving, so a `FAILED` schedule is real (modulo the trusted parts listed below) |

Where the implementation goes **beyond** what is proved:

1. **More than two threads.** `K` rounds over `N` threads is the `N`-cyclic
   generalisation of `Seg` (at most `K·N` switches with the final `T0`
   slot). The Lean model fixes two threads.
2. **Thread creation.** A not-yet-created thread has only empty segments.
   Creation is a visible operation of `T0` that sets `active`. Lean starts
   both threads at once.
3. **The final `T0` slot** is one extra segment. Lean's `SeqLR K` has exactly
   `2K`.
4. **Loops.** The Lean threads are straight-line lists. PRISM unrolls loops
   to `unwind` and *cuts* longer paths. This can only lose interleavings
   (which is why the verdict is `BOUNDED`), never add them.
5. **The formula encoding.** That `reach`/`exec`/`pc'` over the unrolled DAG
   implements "`Exec` of the segment from `pc` to `cs`" is argued in this
   document and tested (`tests/cpp/test_conc.cpp`,
   `tests/conformance/concurrency`), not proved.
6. **Context-switch points only before visible operations** (partial-order
   reduction). Invisible thread-local code commutes with the other threads'
   steps. This is the standard argument, but it is not in Lean.
7. **The properties.** Neither the race definition ("adjacent conflicting
   accesses") nor the deadlock and unlock checks are formalised.
8. **Front end.** The IR rewrite to markers and `pir::translate` are trusted
   here, in the same way as for the `pir` stage (docs/TRUSTED_BASE.md T1).
9. **Weak memory** is not modelled at all. Non-SC orders are `NEEDS-HARNESS`,
   so no verdict depends on them.

Nothing here claims a proof. `BOUNDED` means "within `K` rounds and `unwind`
iterations", and Law 2 keeps it apart from `PROVED`.

## Conformance

`tests/conformance/concurrency/` holds 18 thread programs as true/false
pairs, with 22 property labels (`norace`, `noassert`, `nodeadlock`):

- racy counter, and the same counter under a mutex;
- lost update, and the same update under a mutex;
- two locks taken in opposite orders (deadlock), and in the same order;
- SC `atomic_fetch_add`, and atomic load followed by a separate store;
- join then read, and read before join;
- a CAS spin lock, and a non-atomic test-then-set;
- a condition variable waited on with `while`, and with `if` (spurious
  wake-up);
- nondeterministic inputs;
- C11 `<threads.h>`.

`tools/conformance.py` scores these with the `conc` stage only, and scores
`conc` on these only. The labels are property-scoped: a race reported on a
`noassert` task is "failed-other-property", not a false alarm. `conc` never
proves, so its soundness line is zero wrong proofs by construction.
Completeness is 0 by definition, because every true task is `BOUNDED`.
Detection is measured as "refuted, not replayed", because a counterexample
is a schedule, not an input vector.

Result (`PRISM_BIN=build/prism python tools/conformance.py --suite
tests/conformance/concurrency --no-replay`; see also docs/CONFORMANCE.md):
0 wrong proofs, 9/9 `false` labels refuted in their property class, 13/13
`true` labels `BOUNDED`, and 0 false alarms. A non-vacuity probe appended
`assert(0)` to the end of `main` in every true task, and every probe was
`FAILED`, so the end of `main` is reachable within the bound and `BOUNDED`
is not a side effect of an unsatisfiable encoding.

### Informal check on SV-COMP `pthread-atomic` (not in the suite)

The 19 tasks of `c/pthread-atomic` at the pinned SV-COMP commit were run
through the stage, with defaults `K = 2` and unwind 4:

- **Data races.** All five `no-data-race: false` mutual-exclusion variants
  (`dekker-b`, `dekker-b-unfair`, `lamport-b`, `peterson-b`, `szymanski-b`)
  get `FAILED` data race.
- **Race-free protocols.** The five `no-data-race: true` protocols
  (`dekker`, `dekker-unfair`, `lamport`, `peterson`, `szymanski`) and
  `time_var_mutex` get `BOUNDED`. They use `__VERIFIER_atomic_*` sections,
  which the stage models as a pseudo-mutex whose holder is the only runnable
  thread.
- **Not modelled.** `pthread_rwlock_*`, the `scull` driver model and
  `qrcu-1` (a local array) are `NEEDS-HARNESS` with the construct named.
- **`abort()` rows.** `gcd-2`, `qrcu-2` and `read_write_lock-2/-2b` report
  `abort() is reachable` first. The cause is SV-COMP's
  `assume_abort_if_not`: PIR treats `abort` as a crash, not an assumption.
  This is PIR's policy, and a later row carries the property-specific
  finding.
