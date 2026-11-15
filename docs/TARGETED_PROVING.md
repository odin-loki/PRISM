# Targeted proving: idiom library, contracts, parallel search, complexity limits

Written 15 November 2026 after the hard benchmark (testdata/bench/hard):
38 functions, median 0.8 s, but the four slowest (ring_sim128_ok 300 s,
ring_counters_ok 211 s, nested_bad 167 s, tlv_walk_ok 166 s) end BOUNDED
after searching for a loop invariant they never find. Most of that time is
Houdini step queries reaching their 10 s limit and deep unrolling. The
search is generic: it does not know that a loop is a ring-index update, a
TLV walk or a counted fill. The aim is to prove such functions in seconds by
recognising what they are and starting from what is already known, and to
spend search only where nothing is known.

The laws stay: nothing in a library is trusted. Every invariant, contract
or summary taken from a library is re-checked by the solver for the function
at hand (base, step and the final query); a library entry can only make a
proof faster or possible, never make one by itself.

## 1. What "no answer" can and cannot mean

| result | what it means | how it is reported |
|---|---|---|
| PROVED / PROVED-UNBOUNDED | as today | unchanged |
| BOUNDED | no violation up to unwind k | unchanged |
| **no invariant in family F** (new) | Houdini reached its fixpoint with every query answered (no unknown, no limit): no conjunction of the candidates in F is an inductive invariant that proves the properties. A definite, re-checkable statement about F, not about the program | `extra.invariant_search = "exhausted: <family> (<n> candidates, fixpoint without proof)"`; the verdict stays BOUNDED (the program may still be correct) |
| **not decided within complexity C** (new) | a query reached its deterministic resource limit (Z3 `rlimit`), the same on every machine and every run | `extra.complexity_limit = "<C> rlimit units at <query>"`; UNKNOWN / BOUNDED as today, but reproducible |
| TIMEOUT | wall-clock limit (kept as a backstop only) | unchanged |

A complexity limit never proves that a function has no proof; it makes
"not decided" reproducible and comparable across machines and runs (so the
merge gate and the verdict cache can rely on it). Only the "exhausted"
row is a proven negative, and only about the template family.

## 2. Workstreams

### W1. Complexity limits instead of time limits (pir/encode, solver)
- Every Z3 query gets a deterministic `rlimit` budget (per VC, per Houdini
  query, per function) derived from the function's size; wall-clock limits
  stay as a much larger backstop (`--timeout` still caps a stuck query).
- Houdini records whether its fixpoint was reached with every query
  answered: then "exhausted: family F" (row 1 above).
- Option `--complexity N` (and `PRISM_PIR_RLIMIT`) sets the scale; reports
  carry the units spent per function (a metric, below).
- Gate: verdicts on the conformance suite equal or stronger at the default
  scale; two runs on different core counts give identical verdicts
  (today's wall-clock limits do not).

### W2. Parallel properties and parallel searches (pir/encode)
- The property VCs of one function are asked concurrently on the task
  graph (prism::pool::TaskGraph); the reported violation is still the
  first in order (lowest-index SAT), the group/halving logic is kept.
- Per function, the strategies run as a race instead of a sequence:
  plain unwinding, deepening, k-induction, Houdini (and library-seeded
  Houdini, W3); the first conclusive, validated answer wins; the others are
  interrupted (Z3_interrupt).
- Gate: identical verdicts on the conformance suite and the hard benchmark;
  wall time of the slowest benchmark functions cut (target in §4).

### W3. Idiom library (mined, re-checked)
- Mine the corpus (testdata, conformance, sv-comp, the hard benchmark, the
  OS-Proving core-tier contracts) for loop and function idioms: group loops
  by an abstract loop shape (header phis and their update operations, exit
  condition, memory access pattern; `fncache::shape_key` extended to one
  key per loop), and record which invariants closed each shape.
- `src/prism/pir/idioms/`: a library of idioms, each with: the shape
  pattern, parameterised invariant templates (e.g. ring index: `0 <= head
  - tail <= cap`, `idx == i & (cap-1)`; counted fill: `forall j < i:
  a[j] == v`; TLV walk: `off <= len`, `off + hdr <= len -> ...`), and the
  metric of how often it closed a loop.
- Houdini starts with the matching idioms' instantiated invariants first
  (and only them, before the generic family): most loops close in the
  first round. Still re-checked: base, step, final query.
- Gate: the four slow hard-benchmark functions PROVED or PROVED-UNBOUNDED,
  or "exhausted" with a reason; no verdict lost elsewhere.

### W4. Contract library for common data structures and loops
- Verified C models with ACSL contracts (`requires` / `ensures` /
  `assigns`, loop invariants) for the data structures and loops that recur:
  ring buffer (power-of-two and modulo), singly/doubly linked list and
  `container_of`, bitmap (set/clear/test/find-first), reference count,
  fixed-size allocator / free list, hash table (open and chained), stack,
  queue, binary search, insertion sort, memcpy/memset/strlen-style scans,
  TLV/varint parsers, state-machine step, checksum/CRC loops, min/max/sum
  folds, bounded string copy.
- Each contract is proved by PRISM itself (contracts stage and pir) and
  listed with its assumption id; call sites that match a modelled callee
  use the contract instead of inlining (assume ensures / check requires),
  so callers are proved modularly.
- The OS-Proving mined contracts (1.1 M functions, model-written) are
  candidates, not inputs: each is a HYPOTHESIS until a prover proves the
  function against it (Law 4).
- Gate: every library contract proved; the kernel-model share of
  NEEDS-HARNESS reasons (`prism library stats`) falls.

### W5. Targets and metrics for everything
- Per function in every report: `extra.metrics` = rlimit units spent per
  phase (encode, VCs, unwinding, k-induction, Houdini), number of queries,
  idiom matched (or none), library contracts used, strategy that won.
- Per run: share of functions answered by an idiom or contract, by
  generic search, and not answered; complexity spent per proof.
- `prism library stats` adds the idiom table (shape, count, closed, open).

## 3. File ownership (so work can run in parallel)

| workstream | owns |
|---|---|
| W1 + W2 | `src/prism/pir/encode.cpp`, `houdini.inc` (except the idiom hook), `src/prism/solver/` |
| W3 | `src/prism/pir/idioms/` (new), one hook in Houdini's `generate()`, a miner tool in `src/tools/` |
| W4 | `src/prism/pir/models/` (new kernel/data-structure models), `testdata/contracts/`, contract docs |
| W5 | reporting fields, `src/prism/library.cpp` stats, docs |

## 4. Targets (measured on testdata/bench/hard, 4 cores, `-j 4`, fresh caches)

| metric | 15 Nov 2026 | target |
|---|---|---|
| slowest function | 300 s (BOUNDED) | < 30 s, PROVED or "exhausted" with a reason |
| functions over 60 s | 5 | 0 |
| whole benchmark wall | 385 s | < 60 s |
| loops closed by an idiom or contract | 0 | > 80% of the benchmark's loops |
| verdicts identical across `-j 1` / `-j 4` / two machines | wall-clock dependent | identical (complexity limits) |
| wrong proofs (conformance) | 0 | 0 |
