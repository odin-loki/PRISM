# Concurrency: the `conc` stage

The `conc` stage checks programs that create threads. It uses lazy
sequentialisation (the Lazy-CSeq method), which bounds the number of context
switches. Every thread is checked for data races, deadlocks, bad unlocks,
memory errors and every PIR property. The memory model is sequential
consistency (SC) by default. An atomic with a weaker C11 order selects the
C11 view model. `--memory-model tso` selects x86-TSO instead.

The stage **never** reports `PROVED`. A clean result is `BOUNDED`: a
context-switch bound is not a proof (Law 2).

| | |
|---|---|
| Stage | `conc`, after `pir` in `STAGE_ORDER` (`include/prism/pipeline.hpp`) |
| Code | `src/prism/conc/stage.cpp` (stage, lowering), `extract.cpp` (front end), `lazy.cpp` (encoding and solving); API in `include/prism/conc.hpp` |
| C++ thread models | `src/prism/pir/models/cxx/bits/prism_conc.h` |
| Verdict origin | `solver` (`src/prism/verdict/verdict.cpp`, `proofs/Prism/Verdict.lean`) |
| Executes scanned code | no. It runs clang and opt to compile, then Z3 in process. It is not in `EXEC_STAGES` (Law 9) |
| Tests | `tests/cpp/test_conc.cpp` (doctest) on the corpus `tests/conc/`; `testdata/conformance/concurrency/` (scored by `prism-qa conformance`) |

Each file in `tests/conc/` says in its first comment what the stage should
report. `test_conc.cpp` runs only some of them; the rest are a corpus for
manual runs with `prism --stage conc FILE`.

## What it reports

A **harness** is `main` when `main` creates threads, directly or through
callees. Otherwise it is every function that creates threads and that no
other function calls (`conc::harnesses`). For each harness:

| status | when |
|---|---|
| `FAILED` | A violation is reachable within the bound. One row per distinct violation, up to `max_findings`. `counterexample` holds the schedule; `evidence` holds the interleaving, one visible operation per line with the values read and written |
| `BOUNDED` | No violation within `K` rounds: "no data race, deadlock, assertion failure or UB within K round(s) of N thread(s) (loop unwind U); a context-switch bound is not a proof" |
| `NEEDS-HARNESS` | The program uses something the stage does not model. The message is an `UNENCODED: ...` reason |
| `UNKNOWN` | Z3 returned unknown or timed out, or an unrolled thread exceeded `max_nodes` |
| `NOTRUN` | Z3 was not built, or clang, opt (or clang++ for C++ units) was not found (Laws 1 and 7) |
| `ERROR` | The clang front end failed on the unit |

A function that creates no threads gets no row. A unit whose source does not
mention `pthread_create`, `thrd_create`, `std::thread` or `std::jthread` is
not even lowered.

`extra` on each row names the bound: `rounds`, `unwind`, `threads`,
`context_switch_bound`, `method` (the memory model), `unwind_cut`,
`heap_slots`, `max_alloc`, and, when they apply, `c11_locations` and
`loop_thread_slots`.

### Properties

Checked in every thread:

- **Data race** (`CONC-DATA-RACE`, CWE-362). Under SC and TSO: thread *t*
  accesses a shared location, and the visible operation just before it in
  the global order was a conflicting access to the same location by another
  thread. At least one access is a write, and they are not both atomic.
  Locks, unlocks, creates and joins are visible operations themselves, so two
  accesses they order are never adjacent. Two conflicting accesses that
  nothing orders can always be scheduled back to back, so this check finds
  them, given enough rounds. Under the C11 view model the race check is
  happens-before instead ("C11 orders" below).
- **Every PIR property**: assertions, `reach_error`, `abort`, signed
  overflow, division, shift and poison checks, and uncaught exceptions. They
  come from `pir::translate` unchanged (`FUNC-CONTRACT`, `INT-SIGNED-OVF`,
  `CXX-UNCAUGHT`, ...).
- **Deadlock** (`CONC-DEADLOCK`, CWE-833). At the end of a round every live
  thread is blocked, and the program has not exited. A thread is blocked at
  a lock whose mutex is held (by anyone, itself included), at a join of an
  unfinished thread, or at a condition-variable wait with no signal since it
  started.
- **Unlock of a mutex the thread does not hold** (`LOCK-DOUBLE-UNLOCK`).
- **Memory errors in the bounded memory**: `MEM-OOB-READ`, `MEM-OOB-WRITE`,
  `MEM-USE-AFTER-FREE`, `MEM-DOUBLE-FREE`, `MEM-INVALID-FREE` (including
  `free` of a stack object).

### Options

`conc::Options`; the stage fills them from `Config` (`options_of` in
`stage.cpp`).

| option | default | set by | meaning |
|---|---|---|---|
| `rounds` | 2 | — | `K` round-robin rounds |
| `unwind` | 4 | `--unwind` | loop bound inside each thread; also the thread slots per looped creation site |
| `max_threads` | 4 | — | most thread creation sites allowed |
| `timeout_s` | 30 | `--timeout` | per solver query |
| `max_findings` | 8 | — | most distinct violations per harness |
| `max_nodes` | 20000 | — | unrolled nodes per thread (past it: `UNKNOWN`) |
| `tso` | false | `--memory-model tso` | x86-TSO instead of SC |
| `tso_buffer` | 2 | `--store-buffer k` | TSO store-buffer entries per thread |
| `max_alloc` | 4096 | — | largest symbolic `malloc` size explored, in bytes |

With `N` threads (the harness is `T0`), a run has `K·N` slots plus a final
slot for `T0`.

## Modelled API

| construct | model |
|---|---|
| `pthread_create(&h, NULL, f, arg)`, `thrd_create(&h, f, arg)` | Marks a thread slot active. `f` must be a defined function. Each creation site has a static thread index. `arg` may be null or a pointer into the bounded memory; the thread entry reads it as its argument |
| creation in a loop (handles in an array) | One thread slot per unrolled execution, up to `unwind` slots per site |
| creation by a created thread | Encoded when each such site runs in one thread at most once and creates a slot above its creator's |
| `pthread_join(h, NULL)`, `thrd_join(h, NULL)` | Blocks until thread `h` has finished. A non-null result pointer is `NEEDS-HARNESS` |
| `pthread_mutex_lock/unlock`, `mtx_lock/unlock` | Lock is `assume(owner == 0); owner = self`. Unlock checks `owner == self`, then sets `owner = 0`. The mutex may be a global or live in the bounded memory (struct field, array element, heap) |
| `pthread_mutex_trylock`, `mtx_trylock` | Returns 0 and takes the mutex if it is free. Otherwise returns `EBUSY` (16) or `thrd_busy` (1) |
| `pthread_mutex_init(&m, NULL)`, `mtx_init` | `owner = 0`. Attributes are `NEEDS-HARNESS` |
| `pthread_cond_wait(&c, &m)`, `cnd_wait` | Unlock `m` and join the wait set; block until a signal or broadcast on `c` after the release; lock `m` |
| `pthread_cond_signal/broadcast`, `cnd_signal/broadcast` | Wake every waiter (POSIX allows the extra wake-ups) |
| `pthread_cond_init(&c, NULL)`, `cnd_init`, `*_destroy`, `sched_yield`, `thrd_yield` | No effect: every schedule is explored anyway |
| `pthread_exit`, `thrd_exit` in a thread entry | The thread returns |
| `exit`, `abort`, a failed assertion, `main` returning | The whole program stops, and no thread runs after it |
| scalar integer globals (`i1`..`i64`, or a struct of one, such as `std::atomic<int>`) | Shared variables. Plain loads and stores are visible operations |
| arrays, structs, heap, escaping locals, address-taken or mixed-width globals | The bounded memory ("Memory" below) |
| `load atomic`, `store atomic`, `atomicrmw` (`xchg add sub and or xor nand max min umax umin`), `cmpxchg` (a weak one may also fail spuriously), `fence` | One atomic visible step. An RMW or `cmpxchg` reads and writes in the same step. The order selects the memory model ("C11 orders") |
| `malloc`, `calloc`, `operator new`, `free`, `operator delete` | Heap slots in the bounded memory, one per unrolled execution of the allocation site |
| `__VERIFIER_nondet_*`, `__VERIFIER_assume` | A symbolic value, and an assumption |
| `__VERIFIER_atomic_begin/end`, calls to defined `__VERIFIER_atomic_*` functions (SV-COMP) | An atomic section: a pseudo-mutex, and while one thread holds it no other thread is runnable |

**C++ threads.** The stage lowers C++ units with
`-D__PRISM_CONC_THREADS=1 -fignore-exceptions` against the PRISM models
(`bits/prism_conc.h`). Then opt runs `sroa,mem2reg` (plus `sccp` when the
source names a weaker memory order). `std::thread`, `std::mutex`,
`lock_guard`, `unique_lock`, `condition_variable` and `std::atomic` then
reach the encoder as the pthread and `__atomic` calls above. `joinable()` is
always true. libstdc++'s own `std::thread` (a virtual `_State` object) and
OpenMP regions are `NEEDS-HARNESS`. The pir stage's in-model scheduler
(`bits/prism_sched.h`) is a separate, weaker model; it does not use
`prism_conc.h`.

### Not modelled

Each of these gives `NEEDS-HARNESS` with an `UNENCODED:` reason:

- atomics with a `syncscope`;
- a thread argument that is not null and not into the bounded memory (stack
  scalars, scalar globals), and a join result pointer;
- more creation sites than `max_threads`; a non-looped creation site run more
  than once, or reached by several threads; a looped site run more than
  `unwind` times;
- mutex or condition-variable attributes;
- external globals and aggregates, `thread_local` aggregates, and
  initialisers that hold pointers;
- pointers that leave the bounded memory (stored into it from outside,
  passed to unknown functions);
- floating point in a threaded program, irreducible control flow, and an
  exception that escapes a thread;
- `--memory-model tso` together with a C11 weaker order (see "Weak memory");
- everything `pir::translate` does not encode, such as calls to unknown
  externals (for example `pthread_rwlock_*`). Its own `UNENCODED:` reason is
  passed through.

Timed waits, `try_lock_for`, recursive or timed mutexes, `jthread` stop
tokens and atomic wait/notify are not in `prism_conc.h`; such units fail to
compile or are `UNENCODED`.

A source that names a weaker memory order which does not reach the IR (for
example through a `<stdatomic.h>` shim that drops it) is caught by a source
scan in `stage.cpp`. A clean result, or a result that is only data races,
becomes `NEEDS-HARNESS` ("encoded as seq_cst: a failure found is a real run,
but a clean result is no proof under the weaker order").

## How it works

### 1. Front end: PIR plus visible-operation markers (`extract.cpp`)

`pir::lower_to_ir` lowers the unit (clang `-O0`, then `mem2reg`,
`lower-switch`, `loop-simplify`, `lcssa`, `instnamer`), and
`ir::parse_module` parses it. `pir::translate` models the memory of one
function, not memory shared between threads. So the stage copies the module
and rewrites every *visible* operation into a **marker**: an instruction
that `translate` already understands, whose result name `__prism.conc.<k>`
indexes `Program::ops`.

```
load  iN @g            ->  %__prism.conc.k  = call iN @__VERIFIER_nondet_conc()      ; PIR Havoc = the value read
store iN v, @g         ->  %__prism.conc.k  = call iN @llvm.expect.iN(iN v, iN 0)    ; PIR Copy  = the value written
atomicrmw add @g, v    ->  read marker k; %n = add old, v; %__prism.conc.ks = expect(%n)
cmpxchg @g, c, n       ->  read marker k; eq = (old == c); new = eq ? n : old; write marker ks
lock / unlock / create / join / init -> copy marker (join copies the thread handle)
```

An access through the bounded memory has two markers: the head (the i64
address; the context-switch point) and the tail (`ks`, the value read or
written in the same step).

Everything else goes through `pir::translate` unchanged. That is why the PIR
UB checks and assertions apply in every thread.

A function whose pointer parameter receives bounded-memory pointers at some
call sites and other pointers at others is cloned (`name__prism_mem`) for
the bounded-memory sites. The extraction is then rebuilt, up to three
rounds, so each copy has one pointer kind per parameter.

### 2. Thread graphs (`lazy.cpp`)

Each thread's PIR body is split so that every visible operation starts a
block. That start is a context-switch point. The body is then unrolled into
a DAG, each loop up to `unwind` times. A path that would go past the bound is
*cut* (assumed away); the verdict is `BOUNDED` anyway, and `extra.unwind_cut`
says whether it happened. The nodes are numbered in topological order. These
numbers are the Lazy-CSeq program-counter labels; `END` is the number of
nodes.

### 3. The sequentialised program as one formula

The driver runs `K` rounds. In each round `T0 … TN-1` run in that order.
Then `T0` gets one final slot, so the code after the harness's joins runs.
Each slot re-enters the thread's whole body:

```
runnable = active[t] && !exited && pc[t] != END
cs       = fresh;  runnable -> pc[t] <= cs <= END;  !runnable -> cs == pc[t]
reach(n) = runnable && (pc[t] == n  ||  OR_p (exec(p) && edge(p -> n)))
exec(n)  = reach(n) && n < cs
pc[t]'   = END if a return executed, else the n with reach(n) && n >= cs
```

This is Lazy-CSeq's guarded-jump program (`if (pc > L) goto saved; …
L: if (L >= cs) { pc = L; return; }`) written as a formula.
`conc::lazy_text()` prints it as pseudo-C for inspection.

- **Thread-local values** are "static", as in Lazy-CSeq. Each SSA value keeps
  its value from earlier rounds: `v = ite(exec(def), new, v)`.
- **Shared state is threaded through the slots in order**: shared variables,
  the memory, mutex owners, condition-variable generations, pcs, `active`
  flags, `exited`, and the "last visible operation" record the race check
  reads.
- **Assumptions are prefix-guarded.** A violation is `ok ∧ cond`, where `ok`
  is the conjunction of every assumption before it in the global order. A
  later blocking `assume` cannot hide an earlier failure.

Z3 checks the disjunction of all violations. When it is SAT, the earliest
violation in the model is reported with its interleaving. That violation is
then excluded and Z3 is asked again, up to `max_findings`. UNSAT gives
`BOUNDED`. A later query that times out leaves the found rows and sets
`extra.incomplete`.

## Memory

Shared data that is not a plain scalar integer global lives in a bounded
memory: arrays and structs (`Program::objects`), heap allocations, the
harness's aggregate locals, locals that escape their frame, and scalar
globals whose address is taken or that are accessed at more than one type.

- **Addresses.** Object `k` has i64 addresses `((k + 1) << 32) | offset`, so
  objects never alias. Heap slots and escaping locals get their own object
  ids the same way. The extractor rewrites `getelementptr` on such an object
  (or on a pointer derived from one, through phis, selects and pointers
  stored in the memory) into i64 arithmetic with the module's data layout.
- **Bytes.** The encoder keeps one Z3 array `M` from byte address to byte.
  An access of `w` bits reads or writes its `ceil(w/8)` bytes,
  little-endian, so overlapping accesses of different widths agree.
- **Heap.** Each unrolled execution of an allocation site is one heap slot,
  live from its allocation to its `free`. A symbolic size is bounded by
  `max_alloc`. An escaping local is a slot per activation that dies when its
  frame returns.
- **Checks.** Every access is bounds-checked against its object and checked
  against freed slots. `free` checks for double free and for pointers that
  are not the start of a heap allocation.
- **Races** compare start addresses. Overlapping accesses that start at
  different bytes are not race-checked against each other.

## Weak memory

### x86-TSO

`--memory-model tso` (`Config::conc_tso`, `conc::Options::tso`) checks x86-TSO
instead of SC, for scalar globals and the bounded memory.

- Each thread has a FIFO store buffer of `--store-buffer k` entries
  (default 2).
- A plain store is appended. When the buffer is full, the oldest entry
  commits first.
- A thread's own plain loads forward per byte from the newest buffered store
  that covers that byte, else read `M`.
- Any other visible operation of the thread (atomics, locks, fences, create,
  join, alloc, free) drains its buffer. A join also drains the joined
  thread's buffer.
- Before every visible step each buffer may commit its oldest entry,
  nondeterministically.
- The race check of a buffered store runs when it commits (it becomes
  visible then). Loads are checked when issued.

A bounded `k` under-approximates TSO. Litmus tests such as store buffering
(`tests/conc/tso_sb.c`) need `k ≥ 1`.

TSO and the C11 view model are exclusive. A program with a weaker C11 order
run under `--memory-model tso` is `NEEDS-HARNESS` ("UNENCODED:
--memory-model tso together with C11 weaker orders"). TSO is a hardware model
for SC-compiled programs; a C11 program is checked against C11 alone. Run it
under the default model, whose C11 encoding already contains every TSO
behaviour of a race-free program.

### C11 orders (view model)

When an atomic access has a `relaxed`, `acquire`, `release` or `acq_rel`
order (`Program::views`), `lazy.cpp` runs a promise-free view machine in the
style of the Promising semantics, restricted to RC11 (no load buffering, as
RC11 forbids po;rf cycles). `extract.cpp` records the order of every atomic.
The C++ `<atomic>` model passes the order through, and the stage folds it
with `sccp` (only for sources that name a weaker order).

- **Messages.** Each such location keeps one message per store-site
  execution plus its initial message: a symbolic distinct bv16 timestamp
  (the modification order), the value, and the view released with it.
- **Views.** A thread's view has one timestamp per location and one clock per
  thread. A store takes a timestamp above its view. A load reads any written
  message at or above its view. Acquire (or a later acquire fence) joins the
  message view. Release (or an earlier release fence) attaches the thread
  view. A relaxed store carries the last release fence or release store to
  that location (release sequences). An RMW reads message `ts`, writes
  `ts + 1` and carries the read view on. A failed compare-exchange is a load
  with the failure order.
- **Bounded memory.** When an atomic in the bounded memory has a weaker
  order, the view gets one more entry: an array from address to timestamp.
  Each memory atomic store or RMW execution is a message (address,
  timestamp, value, view). The plain contents of `M` are one more message
  per address, whose timestamp a plain store sets. Every load of the memory,
  plain or atomic, reads one of the messages at its address at or above its
  view.
- **Synchronisation.** Create, join, mutexes and condition variables
  transfer views. A wait is unlock, block, re-lock; the re-lock acquires the
  view of the last unlock.
- **SC fences.** One SC view in the fences' total order (RC11 `psc_F`). A
  seq_cst fence joins it into the thread view and stores the result back.
  Store buffering with seq_cst fences between relaxed accesses is forbidden
  (`tests/conc/c11_sc_fence_sb.c`).
- **Races** are happens-before (epochs against the view clocks), for scalars
  and the bounded memory. A non-atomic access races with any conflicting
  access it is not ordered with. An atomic access races only with
  non-atomic ones. Values of non-atomic locations stay SC.

Approximations (each only adds behaviours, so clean results stand):

- seq_cst accesses (not fences) to a location that also has weaker accesses
  are treated as acq_rel. RC11's psc constraints for SC accesses are not
  encoded. Locations with only seq_cst accesses stay SC.
- An atomic location in the memory is identified by its start address, so
  atomic accesses of different widths or overlapping starts are distinct
  locations.

`extra.method` names the model. `extra.c11_locations` counts the scalar
locations.

## Correspondence with the Lean proofs

Two Lean files model the schedule. See
[PROOFS_TECHNIQUES.md](PROOFS_TECHNIQUES.md) (the lazy sequentialisation
sections) for the proofs themselves.

**`N` threads, `K` rounds**:
`proofs/techniques/PrismTechniques/LazySeqN.lean`. `prismSched N K` is `K`
rounds of `T0 … T(N−1)` plus `T0`'s final slot.
`tests/cpp/test_repolint.cpp` ("repolint: lazy.cpp schedule mirrors
LazySeqN.lean") locks it to `lazy.cpp` and checks the theorems' axioms. A
thread there is any transition system on its local state and the shared
state, so the unrolled thread DAG is an instance. Proved, under SC:

- `lazy_sound`: every run of the sequentialised program is a real
  interleaving, so a `FAILED` schedule is real (modulo the unproved items
  below);
- `lazy_covers`: every interleaving from `T0` with at most `K − 1` context
  switches is covered; `lazy_covers_runs`: every schedule that splits into
  at most `K` increasing runs of thread ids is covered;
- `lazy_covers_two`: with two threads, every interleaving with at most `2K`
  switches is covered;
- `per_thread_bound_not_enough`: for `N ≥ 3` a bound on switches *per
  thread* is not enough. A `BOUNDED` verdict covers only schedules of
  round-robin shape.

**Two threads, straight-line actions**:
`proofs/techniques/PrismTechniques/LazySeq.lean` proves `lazy_seq_covers`
(every interleaving with at most `2K−1` switches is matched),
`lazy_seq_sound` and `seqLR_iff_seg`, through the segment normal form
`Seg`.

Not proved:

1. **The formula encoding.** That `reach`/`exec`/`pc'` over the unrolled DAG
   implements "run the segment from `pc` to `cs`" is argued here and tested,
   not proved.
2. **Loops.** PRISM unrolls loops to `unwind` and cuts longer paths. This can
   only lose interleavings, never add them.
3. **Context-switch points only before visible operations** (partial-order
   reduction). Invisible thread-local code commutes with the other threads'
   steps. This is the standard argument; it is not in Lean.
4. **Thread creation** as a visible operation that sets `active`, and
   looped creation slots.
5. **The properties.** The race definitions, the deadlock and unlock checks
   and the memory checks are not formalised.
6. **Front end.** The rewrite to markers and `pir::translate` are trusted,
   as for the `pir` stage ([TRUSTED_BASE.md](TRUSTED_BASE.md)).
7. **Weak memory.** The proofs assume SC. The TSO and C11 encodings are
   tested (`tests/conc/tso_*`, `tests/conc/c11_*`), not proved.

Nothing here claims a proof. `BOUNDED` means "within `K` rounds and `unwind`
iterations", and Law 2 keeps it apart from `PROVED`.

## Conformance

`testdata/conformance/concurrency/` holds 18 thread programs as true/false
pairs with 22 property labels (`norace`, `noassert`, `nodeadlock`), counted
2026-11-05 at commit 991c4eafa:

- racy counter, and the same counter under a mutex;
- lost update, and the same update under a mutex;
- two locks taken in opposite orders (deadlock), and in the same order;
- SC `atomic_fetch_add`, and an atomic load followed by a separate store;
- join then read, and read before join;
- a CAS spin lock, and a non-atomic test-then-set;
- a condition variable waited on with `while`, and with `if`;
- nondeterministic inputs;
- C11 `<threads.h>`.

`src/tools/qa/conformance.cpp` scores these with the `conc` stage only, and
scores `conc` on these only (`src/tools/qa/support/task.cpp`). The labels
are property-scoped: a race reported on a `noassert` task is
"failed-other-property", not a false alarm. `conc` never proves, so its
soundness line is zero wrong proofs by construction, and completeness is 0
by definition (every true task is `BOUNDED`). Detection is measured as
"refuted, not replayed": a counterexample is a schedule, not an input
vector.

```
build/prism-qa conformance --suite testdata/conformance/concurrency --no-replay
```

See [CONFORMANCE.md](CONFORMANCE.md) for the scoring rules.
