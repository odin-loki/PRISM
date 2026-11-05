# Solver portfolio, query cache and certified mode

The solver library answers one question: is this formula satisfiable? The
`pir` stage sends every verification condition (VC) through it. It runs
several solvers in parallel, caches answers, validates every counterexample
in Z3, and can turn an UNSAT answer into a checked LRAT certificate
(`PROVED-CERTIFIED`). What a certified verdict still trusts is in
[TRUSTED_BASE.md](TRUSTED_BASE.md).

| | |
|---|---|
| API | `include/prism/solver.hpp` (`prism::solver::solve`) |
| Portfolio, scheduler, watchdog | `src/prism/solver/portfolio.cpp` |
| Query normalisation, cache key, portable SMT-LIB2 | `src/prism/solver/query.cpp`, `query.hpp` |
| Certificate store (packing, size cap, pruning) | `src/prism/solver/certstore.cpp`, `include/prism/solver_certs.hpp` |
| Lean bit-blaster bridge | `src/prism/solver/leanbb.cpp` |
| CNF, DIMACS, ProbSAT walker | `src/prism/solver/cnf.cpp` |
| Processes, tool lookup, LRAT checkers | `src/prism/solver/util.cpp` |
| Learned scheduler | `src/prism/solver/predict.cpp`, `gbdt.cpp`, `predict_default.json`, `include/prism/solver_predict.hpp`, `include/prism/solver_gbdt.hpp` |
| Training and replay tools | `src/tools/prism_ai/` (`prism_ai predict`, `replay`, `e2e`; replay is `sched.cpp`) |
| Benchmark | `src/tools/qa/solver_bench.cpp` (`prism-qa solver-bench`) |
| Tests | `tests/cpp/test_main.cpp` ("solver: ..." cases), `tests/cpp/test_ai_assist.cpp` (predictor) |

## API

```cpp
#include "prism/solver.hpp"
namespace ps = prism::solver;

ps::SolveOptions o;      // timeout_s, certified, portfolio, cache_dir, ...
ps::SolveResult r = ps::solve(ctx, violation /* sat <=> property violated */, o);
ps::verdict_status(r);   // "PROVED-CERTIFIED" | "PROVED" | "FAILED" | "TIMEOUT" | "UNKNOWN" | "ERROR"
```

The formula is satisfiable exactly when the property is violated.

- `Sat` is a counterexample. Its model has been evaluated against the
  original formula in Z3 before it is returned.
- `Unsat` means the property holds. Plain UNSAT trusts the solver
  (`PROVED`). Certified UNSAT means cake_lpr accepted an LRAT proof of the
  exact CNF (`PROVED-CERTIFIED`).
- `Unknown`, `Timeout` and `Error` are no answer. They are never a clean
  result (Laws 1 and 7).

`SolveResult` holds `kind`, `winner`, `model` (name to SMT-LIB literal),
`certified`, `certificate_info`, `cnf_sha256`, `query_hash`, `cache_hit`,
`note`, `ran`, `missing`, `times`, `bucket`, `wall_s` and `watchdog`.
`note` records every member that failed, was rejected or was missing, and
every reason certification did not happen. Nothing is dropped quietly
(Law 7).

`verdict_status` maps `certified == true` to `laws::PROVED_CERTIFIED`.
`kProvedCertified` in `solver.hpp` is only an alias of it.

## Query cache

- **Key.** The formula is translated into a fresh Z3 context and simplified
  with the default rewriter. Its constants are renamed `prism!v0..n` in
  order of first occurrence. It is printed with
  `Z3_benchmark_to_smtlib_string`. The key is the SHA-256 of that text with
  a schema line (`prism-solver-query v1`) and the Z3 version in front.
  Renaming the variables gives the same key. A cached model is mapped back
  to the new names.
- **Store.** `<cache_dir>/queries/<h[0:2]>/<h>.json` holds the kind, the
  winner, the model in canonical names, `certified`, `certificate_info` and
  `cnf_sha256`. A certified entry also keeps its LRAT proof, packed, in
  `certs/<h>.lratz`. The CNF is not kept; a hit bit-blasts again.
- **Location.** `SolveOptions::cache_dir`, else
  `$XDG_CACHE_HOME/prism/solver`, else `~/.cache/prism/solver`. The pir
  stage passes `--solver-cache DIR`.
- **Rules.**
  - A cached SAT model is validated again in Z3. If it fails, the entry is
    ignored with a note.
  - A cached plain UNSAT answers plain requests. A certified request solves
    again for a certificate. If that gives no answer, the cached plain UNSAT
    stands, uncertified. Certification never loses an answer.
  - A certified request needs a certified entry. On a hit the formula is
    bit-blasted again, its CNF must match `cnf_sha256`, and the checkers
    check the stored proof again (unpacked into a private work directory).
  - UNKNOWN and TIMEOUT are never cached.
- **Certificate store cap.** `certs/` is capped at
  `SolveOptions::cert_cache_max_bytes`, else `$PRISM_CERT_CACHE_MAX` (a size
  such as `2G` or `500M`; `0` keeps no proof), else 2 GiB. After each store
  it is pruned least-recently-used (file mtime; a re-checked hit touches its
  proof) down to 80% of the cap. A proof in use by this process is never
  pruned. A checker reads its own unpacked copy, so pruning by another
  process cannot pull a proof from under a check. A proof larger than the cap
  is not kept, and its entry is recorded as plain unsat
  (`certificate not cached: ...` in the note). A pruned proof means the next
  certified request solves again.
- **Packing.** A lossless varint encoding of the LRAT text, with hint and
  deleted clause ids stored as deltas. The round trip is byte-identical.
- **Stale work directories.** The first certified request of a run removes
  `prism-solve-*` work directories in the temp dir whose process no longer
  exists (left by killed runs).
- **Solve times** are always recorded, even with `use_cache = false`
  (see "Scheduler").

## Portfolio

| Member | Input | Found at | What it may answer |
|---|---|---|---|
| `z3` | in process, a private translated context | always (`z3_in_process`) | SAT (model validated) / UNSAT (plain) |
| `bitwuzla` | SMT-LIB2 file + `(get-value ...)` | tool lookup below | SAT (validated) / UNSAT (plain) |
| `cadical` | DIMACS (bit-blast) | tool lookup below | SAT (assignment checked on the CNF, then validated) / UNSAT (plain; in certified mode it also writes LRAT) |
| `kissat` | DIMACS | tool lookup below | SAT (validated) / UNSAT (plain) |
| `sls` | DIMACS, ProbSAT in process | always, when the formula bit-blasts | SAT only (validated). It never answers UNSAT |
| `extra_solvers` | SMT-LIB2 or DIMACS | caller-supplied (`ExternalSolver`) | same rules |

All members run in parallel, as threads or child processes. Each child
process is in its own process group and is killed on cancel or timeout. The
first **definitive** answer wins. A SAT answer is accepted only after the
model evaluates the original formula to `true` in Z3. A rejected model is
noted and the other members keep running. When a validated model contradicts
an earlier UNSAT, the counterexample wins and the note says `DISAGREEMENT`.
In certified mode CaDiCaL-with-LRAT always runs and is waited for.

`SolveOptions::portfolio = false` runs Z3 alone, plus the
certificate chain when certified (`prism --solve-smt2 FILE --z3-only` does this for one query).

**Tool lookup** (`find_tool`, `util.cpp`), first match wins:

1. `--tool NAME=PATH` (`SolveOptions::tool_paths`);
2. `SolveOptions::tool_dirs`: `<dir>/<name>/<sha>/{bin/,}<exe>`;
3. the pinned build `<tools_home>/<name>/<manifest commit>/`, where
   `tools_home` is `$PRISM_TOOLS_DIR` or `~/.prism/tools`;
4. `PATH`.

Without `--allow-exec`, `tools_home` is not searched when it lies inside the
scanned tree (`refuse_tools_under`, Law 9). A member whose binary is not
found is listed in `SolveResult::missing` and in the note.

**Bitwuzla input.** Bitwuzla and any SMT-LIB2 extra solver get a portable
copy of the formula (`portable_smt2` in `query.cpp`). Z3 prints a
1-argument `(or x)` and its own `bvsmul_noovfl` / `bvsmul_noudfl` /
`bvumul_noovfl`, which Bitwuzla rejects. The rewrite is an equivalence:
1-argument `and`/`or` are dropped, and the overflow predicates are stated on
the double-width product. A doctest checks the equivalence with Z3.

**Memory budget.** Under a process memory budget
([MEMORY_RULES.md](MEMORY_RULES.md)):

- A query with more than 1000 nodes per budget MB is `Unknown` up front,
  with a `memory budget: ...` note. Z3 does not poll interrupts while it
  prints or translates such a query.
- A large query (default 20000 nodes; 2000 once the process tree holds half
  the budget) runs Z3 in a killable child, `prism z3-worker`
  (`$PRISM_Z3_WORKER` names another executable).
- Each solver child gets a resident-memory cap: the room left in the budget
  (this process's peak RSS plus the caps already granted to live children),
  shared by the children that may run at once, at least 256 MB.

### Scheduler

- `features()` computes the largest bit-vector width, the node count, and
  whether the query has arrays, floating point, UF, arithmetic, quantifiers
  or bit-vector mul/div. These give a bucket such as
  `QF_BV|w32|n100|muldiv`.
- Feature rules (`rule_estimate`) give each member an expected time. Z3 is
  preferred for arrays, UF or arithmetic. Bitwuzla is preferred for
  floating point, wide, large or mul/div queries.
- `<cache_dir>/solve_times.json` records `n`, `total` and `wins` per bucket
  and member. A recorded mean replaces the rule estimate.
- The member with the lowest mean that has won in the bucket starts alone
  for `min(3 × mean + 0.2 s, 30% of timeout)`. The others then join.
- With no such member, in-process Z3 starts alone for
  `min(0.15 s, 10% of timeout)`. Most pir VCs are answered in milliseconds,
  and then no bit-blast, process spawn or walker is paid for.
- The learned scheduler, when it applies, replaces both of these (next
  section).
- The CaDiCaL-with-LRAT member of certified mode is never delayed.
- The ProbSAT walker gets `max(1 s, 10% of timeout)` (`sls_budget_s`) and
  then gives its core back.
- `max_parallel` defaults to the hardware thread count.

## Learned scheduler

A gradient-boosted tree model (GBDT) per member predicts that member's
seconds from the query features. The predictions replace the rule and
history estimates. The member predicted fastest leads by
`min(3 × predicted + 0.2 s, 30% of timeout)`. The note says
`scheduler: <member> leads by <s> (model, bucket ...)`.

- **What it changes.** Only the order of the members and who gets the head
  start. Every answer still comes from a solver, and every SAT model is still
  validated in Z3.
- **When it is off.** Certified requests always use the rules.
  `PRISM_SOLVER_PREDICT=0` switches it off for every request.
- **Which model.** `$PRISM_SOLVER_MODEL`, else
  `<cache_dir>/predict_model.json`, else the built-in model. CMake embeds
  `src/prism/solver/predict_default.json` (see `CMakeLists.txt`). A model
  file whose schema or feature list does not match is ignored. A model file
  with `enabled: false` switches prediction off.
- **Deterministic.** The trees are fixed; there is no randomness at run time.
- **No unwind model.** pir does not predict the unwind bound
  (`solver_predict.hpp`). An unwind model was measured and lost verdicts
  (more `BOUNDED` than fixed unwind 8), so it is not wired in.

**How the built-in model is made.**

1. Collect: `PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*"`
   with `DIR/roots.txt` listing source roots. Every encodable function goes
   through pir. For a sample of its VCs every member runs alone (a timeout is
   a censored time). The run writes `solve_runs.jsonl` and `bound_runs.jsonl`.
2. Replay (`prism_ai replay`, `src/tools/prism_ai/sched.cpp`): the
   scheduler of `portfolio.cpp` replayed on the alone-times for several
   policies and core counts, on source files held out by a hash of the path.
   It assumes no contention between cores.
3. End to end (`prism_ai e2e`): the real portfolio (`prism --solve-smt2`) on
   held-out VCs, the policies interleaved per VC so load changes hit them
   alike.
4. Export:

```
build/prism_ai e2e --prism build/prism --solve-log DIR/solve_runs.jsonl --out E2E
build/prism_ai predict --solve-log DIR/solve_runs.jsonl --bound-log DIR/bound_runs.jsonl \
  --repeat-solve-log REP/solve_runs.jsonl --repeat-bound-log REP/bound_runs.jsonl \
  --end-to-end E2E/e2e.json --out src/prism/solver/predict_default.json
```

`REP` is a second collection of the held-out files, used to measure
run-to-run noise.

**The measurement that switched it on** (2026-09-23, commit 0c1c0beb4; 4
cores shared with other jobs, so every number is noisy). Training data was
2,060 distinct pir VCs from `testdata/pir`, `testdata/conformance` and
`testdata`. 30% of the source files were held out (599 VCs, 133 files).

| | rules | history | GBDT |
|---|---:|---:|---:|
| replay, 2 cores, held-out total | 17.31 s | 15.89 s | 13.02 s |
| real portfolio, 781 held-out VCs, run 2 | 43.35 s | 41.73 s | 35.91 s |

Run-to-run noise was 10–16% for the replay and 0.5% for end-to-end run 2.
No verdict differed between policies.

What a contributor should know about it:

- **The gain is "Bitwuzla first".** A static "Bitwuzla first" rule was within
  1% of the model in the replay. Without Bitwuzla installed the model is
  within noise of the rules.
- **Ordering alone does nothing** on these VCs. Z3's 0.15 s head start
  already answers most of them. All of the gain is in who leads.
- **The training VCs are small.** On hard queries the model has almost no
  data. `<cache_dir>/solve_log.jsonl` keeps collecting production lines for
  a retrain.

## Stalls and the watchdog

A member that does not stop when told to must not hold `solve()` past its
timeout. The watchdog bounds the part of a query PRISM controls and records
the rest.

- **Jobs own their state.** Job threads share their state with `solve()`
  through `Jobs`: the board, the private Z3 contexts with the formula in
  each, the CNF and the stop flags. A job still running
  `SolveOptions::watchdog_grace_s` (default 2 s) after it was stopped
  (answer found, or deadline) is detached, and `solve()` returns. The answer
  (or `TIMEOUT`) stands. The detached job can only write into its own share
  of `Jobs`.
- **Unreaped children.** A killed member process that is not reaped 2 s
  after `SIGKILL` (uninterruptible sleep) is left to a detached reaper
  thread (`Proc::unreaped`). The run returns.
- **Every event is recorded** (Law 7): in `SolveResult::watchdog`, as a
  `WATCHDOG:` line in the note, and as a line in
  `<cache_dir>/watchdog.jsonl` (hash, bucket, events, timeout, wall).
- **Overruns.** A plain query whose whole call takes longer than its timeout
  plus the grace plus 1 s is recorded the same way, with the time of each
  phase: setup (normalisation, cache, history), files (work directory and
  SMT-LIB2 file), solving, stopping and recording. The next stall then says
  where it happened.
- **What no timeout covers.** Time in `solve()`'s own thread after the
  answer (stopping, the history rewrite, the log append, removing the work
  directory), and a process that is not scheduled at all (I/O or page
  reclaim on a machine short of memory). Overrun records show these.

Tests (`tests/cpp/test_main.cpp`):

- "solver: the watchdog detaches a job that ignores its stop and records it":
  a fault-injected Z3 job (`SolveOptions::debug_stall_s`) sleeps through its
  stop; `solve()` returns `TIMEOUT` within the grace, with the event in the
  result, the note and `watchdog.jsonl`.
- "solver: repeated portfolio runs stay within timeout plus grace": many
  short queries through every installed member, no watchdog event.

To hunt a stall by hand: run
`prism --solve-smt2 VC --timeout 8 --solver-cache FRESH_DIR` over a directory
of VCs (`PRISM_PREDICT_COLLECT` and `prism_ai e2e` write them), then read
`FRESH_DIR/watchdog.jsonl`.

## Certified mode

`SolveOptions::certified = true` (`prism --certified`).

**The Z3-tactics chain.** QF_BV formula → Z3 `simplify`, `bit-blast`,
`simplify`, `tseitin-cnf` → DIMACS (the variable map is kept by
construction) → CaDiCaL `--lrat=true --binary=false --unsat` →
`cake_lpr cnf lrat` must print `s VERIFIED UNSAT`. drat-trim's `lrat-check`,
when found, is a second opinion that can only veto. It is not applicable to
a CNF that already contains the empty clause, and the info says so. The CNF
hash is checked before and after. `SolveResult::cnf_sha256` is the hash of
the exact CNF cake_lpr checked.

**The Lean-proved bit-blaster.** `SolveOptions::bitblaster` is `Auto` (the
default), `Lean` or `Z3`. With `Auto`, a certified request whose formula is
inside the proved fragment, and whose Lean tools are built (`lake build` in
`proofs/techniques`: `prism-bitblast`, `prism-lrat-check`), is bit-blasted by
the Lean-proved `toCNF`. Lean's verified LRAT checker must then accept the
proof as well as cake_lpr. Otherwise the Z3 chain is used, and
`certificate_info` says
`bitblast: z3 tactics, unproved (Lean bit-blaster not used: <why>)`.
A Lean DAG larger than 2 MB of text goes to the Z3 chain too
(`kLeanDagMaxBytes`). See [TRUSTED_BASE.md](TRUSTED_BASE.md) sections 1.1
and 1.2.

**What cannot be certified.** Formulas with arrays, floating point, UF,
arithmetic or quantifiers get `not certifiable: <reason>`. They keep their
plain answer. Kissat writes no LRAT, so only CaDiCaL makes certificates.
Bitwuzla's UNSAT answers are always plain `PROVED`.

**Failure keeps the plain answer.** Any failure in the chain leaves the
plain result, with the reason in the note.

**Budgets.**

| option | default | meaning |
|---|---|---|
| `timeout_s` | 30 s | the answer is due within this, as in plain mode |
| `cert_timeout_s` | `max(60 s, 4 × timeout_s)` | from the start of the query, how long CaDiCaL-with-LRAT (and its bit-blast) may keep running once some member has answered UNSAT |
| `check_timeout_s` | `max(60 s, 4 × timeout_s)` | each LRAT checker; checking a multiplier's proof can take longer than finding it |
| `checker_mem_mb` | `$PRISM_CHECKER_MEM`, else 4096 MB | cake_lpr's heap (`--CML_HEAP_SIZE`) and the resident cap of the Lean tools |

Running out of any budget costs only the certificate. The note says why
(for example `not certified: cadical did not finish within the certificate
budget (N s)` or `ran out of memory`), and the plain UNSAT stands. cake_lpr
and Lean's checker run at the same time; both must accept.

**Known plain answer.** A certified request whose plain answer is already
known (a cached plain unsat, or `SolveOptions::known_unsat` from the pir
stage) runs only the certificate member until the certificate budget. A
formula that cannot be certified then returns the known answer at once.

**CaDiCaL `--unsat`.** Every certificate uses `--unsat`. On the
Lean-bit-blasted multiplication-overflow CNFs it was the fastest preset and
gave the smallest proofs (2026-09-23, commit 3f205be60, CaDiCaL 3.0.1; user
CPU seconds on a shared 4-core machine):

| CNF | default | `--unsat` |
|---|---|---|
| `mul_true` (32-bit `smulo`, 101k vars) | 9.1 s, 160 MB LRAT | 7.2 s, 117 MB |
| `widen_mul_true` (64-bit `smulo` of sign-extended 32-bit values, 397k vars) | 194 s | 76 s |

**Two squarings that stay uncertified** (2026-09-24, commit 25303f02d,
CaDiCaL 3.0.1, `--unsat`):

- `fn_macro_true` (32-bit `a*a`, `|a| ≤ 46340`): CaDiCaL answers in 65 s
  with a 2.09 GB LRAT proof. cake_lpr needs 163 s to check it alone, more
  than the default checker budget. Splitting on the sign bit does not help:
  the `a < 0` half alone is 65 s and 1.77 GB.
- `long_mul_true` (64-bit `a*a`, `|a| ≤ 3·10⁹`): `a ≥ 0` is UNSAT in 2.5 s,
  but `a < 0` has no answer in 150 s, with any preset tried and on cubes of
  the high input bits. Cube-and-conquer would not help here, so it is not
  implemented. (It would only be sound with every cube's proof lifted into
  one LRAT proof checked against the original CNF.)

## Use in the pir stage

The `pir` stage sends every VC through `solve()`: one query per inserted
property and one for the unwinding assertion (see
[PIR.md](PIR.md#solving-roadmap-31--32)). The stage always uses the
portfolio and the query cache.

| option | effect |
|---|---|
| `--solver-cache DIR` | `SolveOptions::cache_dir` |
| `--timeout S` | budget per query |
| `--certified` | `SolveOptions::certified` |
| `PRISM_CERTIFY_BUDGET` | seconds of certification per PROVED function (default `max(240 s, 8 × --timeout)`; 0: none) |

With `--certified`, a function becomes `PROVED-CERTIFIED` only when it has at
least one VC and every VC is covered by a checked certificate. The stage
first asks for one certificate of the disjunction of all its VCs, split in
halves when a checker runs out of memory (`pir::CheckOptions::certify_combined`).
Otherwise it asks for one certificate per VC. Both set `known_unsat`. A
function with no VCs stays `PROVED` with
`certify_note = "no verification conditions (nothing to certify)"`. A
function whose budget runs out stays `PROVED` with a note. Memory-model VCs
are QF_BV, so they are certified like the rest.

The k-induction step (`PROVED-UNBOUNDED`) is answered by in-process Z3 alone:
no portfolio, no query cache, no certificate.

## ProbSAT

`probsat()` in `cnf.cpp` is ProbSAT with the polynomial break-only
distribution: `cb` = 2.38 for 3-CNF, 3.0 for 4, 3.7 for 5 and 5.4 for longer
clauses; `eps` = 1. It keeps an unsatisfied-clause list and true-literal
counts. It is a counterexample finder only: "not found" means nothing.

Its CUDA mirror, `src/cuda/probsat.cu` (one walker per thread, the host
re-checks the assignment), is built only with `PRISM_CUDA=ON`. It has not
been run on a GPU. Whether it finds counterexamples faster than the CPU
walker is not measured.

## Tools

The external members are pinned in `third_party/MANIFEST.toml` and built
with `prism-deps tool NAME` into `~/.prism/tools/<name>/<commit>/`:

| tool | role |
|---|---|
| `cadical` | member; the only certificate producer (LRAT) |
| `kissat` | member |
| `bitwuzla` | member (needs meson, ninja, GMP and MPFR to build) |
| `cake_lpr` | LRAT checker that decides certification |

drat-trim (`lrat-check`) is not pinned. It is used as a veto when it is
found. The Lean tools (`prism-bitblast`, `prism-lrat-check`) come from
`lake build` in `proofs/techniques`. A member takes part whenever its binary
is found; otherwise it is listed as missing. The exact builds a certified
verdict trusts are in [TRUSTED_BASE.md](TRUSTED_BASE.md) section 4.

## Benchmarks

**Portfolio vs Z3 alone on the conformance suite's pir VCs.**
`prism-qa solver-bench` (`src/tools/qa/solver_bench.cpp`) runs
`prism --pir-vcs` to write every VC of every encodable function of the
conformance tasks (unwind 8), then answers each VC three times back to back
with `prism --solve-smt2`: Z3 alone (`--z3-only`), the portfolio with no
history, and the portfolio with the history it built on the VCs before. The
query cache is off, the timeout is 30 s per VC, and solver wall time is
summed.

```
build/prism-qa solver-bench --prism build/prism --out solver-bench-out
```

Result (2026-09-23, commit c77c15397; 4 shared cores, load average 7–9):
4,179 VCs, all solved in every pass, no disagreement. Z3 alone 229.0 s;
portfolio without history 225.7 s; portfolio with history 211.4 s. That run
was before the portable SMT-LIB2 rewrite let Bitwuzla answer pir VCs and
before the learned scheduler; it has not been rerun since. Pass-after-pass
runs (all of one pass, then the next) are not fair comparisons on a shared
machine; only interleaved runs are.

**Hard generated queries.** `./prism_tests -tc="solver bench*" --no-skip`
runs 14 generated bit-vector queries (multiplier equivalence, `udiv`/`urem`
identities, semiprime factoring, popcount) through Z3 alone and the
portfolio. With Bitwuzla installed (2026-09-23, commit caf8eaf41, a loaded
machine): Z3 alone 190.9 s, portfolio 164.9 s, portfolio with history
121.3 s. The SAT solvers win the factoring queries, which Z3 alone times out
on. Without history the portfolio loses on arithmetic-heavy UNSAT queries
that Z3 wins alone, because Z3 then shares the cores. The buckets are
coarse, so a query can inherit a head start from unlike queries in its
bucket.
