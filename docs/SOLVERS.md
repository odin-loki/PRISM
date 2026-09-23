# Solver portfolio, query cache and certified mode

Roadmap Part 3.1 (portfolio and cache), 3.2 (certified mode) and 3.3 (a CPU
reference for the stochastic local search counterexample finder). The code is
in `include/prism/solver.hpp` and `src/prism/solver/`. It is C++ only
(roadmap D8). What a certified verdict still trusts is in
[`TRUSTED_BASE.md`](TRUSTED_BASE.md).

## API

```cpp
#include "prism/solver.hpp"
namespace ps = prism::solver;

ps::SolveOptions o;      // timeout_s, certified, portfolio, cache_dir, ...
ps::SolveResult r = ps::solve(ctx, violation /* sat <=> property violated */, o);
ps::verdict_status(r);   // "PROVED-CERTIFIED" | "PROVED" | "FAILED" | "TIMEOUT" | "UNKNOWN" | "ERROR"
```

`SolveResult` holds `kind`, `winner`, `model` (name → SMT-LIB literal,
already validated), `certified`, `certificate_info`, `query_hash`,
`cache_hit`, `note`, `ran`, `missing`, `times`, `bucket` and `wall_s`.
`note` records every member that failed, was rejected or was missing, and
every reason certification did not happen. Nothing is dropped quietly
(Law 7).

## Query cache

- **Key.** The formula is translated into a fresh Z3 context and simplified
  with the default rewriter. Its constants are renamed `prism!v0..n` in order
  of first occurrence, and it is printed with
  `Z3_benchmark_to_smtlib_string`. The key is the SHA-256 of that text, with a
  schema line and the Z3 version in front. Renaming the variables gives the
  same key, and the cached model is mapped back to the new names.
- **Store.** `<cache_dir>/queries/<h[0:2]>/<h>.json` holds the kind, the
  winner, the model in canonical names, `certified`, `certificate_info` and
  `cnf_sha256`. Certified entries also keep `certs/<h>.cnf` and
  `certs/<h>.lrat`. The default `cache_dir` is
  `$XDG_CACHE_HOME/prism/solver`, or `~/.cache/prism/solver` when that is not
  set.
- **Rules.**
  - A cached SAT model is validated again in Z3, and if it fails the entry is
    ignored with a note.
  - A cached plain UNSAT answers plain requests. A certified request solves
    again for a certificate; if that gives no answer at all, the cached plain
    UNSAT stands, uncertified (certification never loses an answer).
  - The LRAT checkers get `check_timeout_s`, default `max(60 s, 4 x
    timeout_s)`: checking a multiplier's proof can take longer than finding it.
  - A certified request needs a certified entry. On that hit the formula is
    bit-blasted again, its CNF must match `cnf_sha256`, and cake_lpr checks
    the stored proof again.
  - UNKNOWN and TIMEOUT are never cached.

## Portfolio

| Member | Input | Found at | What it may answer |
|---|---|---|---|
| `z3` | in-process, a private translated context | always (`z3_in_process`) | SAT (model validated) / UNSAT (trusted, plain) |
| `bitwuzla` | SMT-LIB2 file + `(get-value ...)` | `~/.prism/tools/bitwuzla/<sha>/`, `PATH` | SAT (validated) / UNSAT (plain) |
| `cadical` | DIMACS (Z3 bit-blast) | `~/.prism/tools/cadical/<sha>/bin/`, `PATH` | SAT (assignment checked on the CNF, then validated) / UNSAT (plain; in certified mode it also writes LRAT) |
| `kissat` | DIMACS | `~/.prism/tools/kissat/<sha>/bin/`, `PATH` | SAT (validated) / UNSAT (plain) |
| `sls` | DIMACS, ProbSAT in-process | always, when bit-blastable | SAT only (validated). It never answers UNSAT. |
| `extra_solvers` | SMT-LIB2 or DIMACS | caller-supplied | same rules |

All members run in parallel threads or processes (posix_spawn, one process
group each, killed on cancel or timeout). The first **definitive** answer
wins. A SAT answer is accepted only after the model evaluates the original
formula to `true` in Z3. A rejected model is noted and the others keep
running. In certified mode, CaDiCaL-with-LRAT is always run and waited for.
When a validated model contradicts an earlier UNSAT, the counterexample wins
and the note says `DISAGREEMENT`.

**Scheduler.**

- `features()` computes the maximum bitvector width, node count, arrays,
  floating point, UF, arithmetic, quantifiers and bitvector mul/div. These
  give a bucket such as `QF_BV|w32|n100|muldiv`.
- Feature rules give each member an expected time. For example, Z3 goes
  first for arrays, UF or arithmetic, and Bitwuzla goes first for floating
  point, wide or mul/div queries.
- `<cache_dir>/solve_times.json` records `n`, `total` and `wins` per bucket
  and member. A recorded mean replaces the rule estimate.
- The member with the lowest mean that has won in the bucket starts alone for
  `min(3 × mean + 0.2 s, 30 % of timeout)`. The others then join.
- With no such member (no history for the bucket), in-process Z3 starts
  alone for `min(0.15 s, 10 % of timeout)`. Most pir VCs are answered in
  milliseconds, and then no bit-blast, process spawn or walker is paid for.
  The CaDiCaL-with-LRAT member of certified mode is never delayed.
- **Learned scheduler (on by default).** A GBDT per member predicts its
  seconds from the query features (`src/prism/solver/predict.cpp`, the
  built-in model `predict_default.inc`, or `$PRISM_SOLVER_MODEL` /
  `<cache_dir>/predict_model.json`, which replace it). The predictions
  replace the estimates, and the member predicted fastest leads by
  `min(3 × predicted + 0.2 s, 30 % of timeout)`, instead of the history
  leader or Z3. It only orders members and sets the head start: every answer
  still comes from a solver, SAT is still validated in Z3, and certified
  requests keep the rules. `PRISM_SOLVER_PREDICT=0` switches it off. The
  measurement that switched it on is below ("Learned scheduler").
- The ProbSAT walker gets `max(1 s, 10 % of timeout)` and then gives its core
  back.
- `max_parallel` defaults to the hardware thread count.
- Bitwuzla (and any SMT-LIB2 extra solver) gets a portable copy of the
  formula (`detail::portable_smt2`): Z3 prints 1-argument `(or x)` and its own
  `bvsmul_noovfl` / `bvsmul_noudfl` / `bvumul_noovfl`, which Bitwuzla
  rejects. Before this, Bitwuzla failed on 119 of the first 120 `tests/pir` VCs (it
  answered "unknown" in milliseconds, so it never won). The rewrite is an
  equivalence (1-argument `and`/`or` dropped; the overflow predicates stated
  on the double-width product); the doctest checks it with Z3.

## Certified mode

`SolveOptions::certified = true`. The chain is QF_BV → Z3 `simplify`,
`bit-blast`, `simplify`, `tseitin-cnf` → DIMACS (the variable map is kept by
construction) → CaDiCaL `--lrat=true --binary=false` → `cake_lpr cnf lrat`
must print `s VERIFIED UNSAT`. drat-trim `lrat-check` can veto. The CNF hash
is checked before and after. Any failure leaves the plain result with the
reason in the note. Formulas with arrays, floating point, UF, arithmetic or
quantifiers get `not certifiable: <reason>`. `verdict_status` maps
`certified == true` to `laws::PROVED_CERTIFIED` (the verdict module's own
spelling; `kProvedCertified` is only an alias of it), and
`SolveResult::cnf_sha256` is the hash of the exact CNF cake_lpr checked.

`SolveOptions::bitblaster` (`auto`, the default; `lean`; `z3`) picks who makes
the CNF. With `auto`, a certified request whose formula is inside the proved
fragment and whose Lean tools are built (`lake build` in `proofs/techniques`:
`prism-bitblast`, `prism-lrat-check`) is bit-blasted by the Lean-proved
`toCNF`, and Lean's verified LRAT checker must accept the proof next to
cake_lpr; otherwise the Z3 chain above is used and `certificate_info` says
`bitblast: z3 tactics, unproved (Lean bit-blaster not used: <why>)`
([TRUSTED_BASE.md](TRUSTED_BASE.md) sections 1.1 and 1.2).

## Use in the pir stage

The `pir` stage sends every verification condition through `solve()`: one
query per inserted property and one for the unwinding assertion
([PIR.md](PIR.md#solving-roadmap-31--32)). The stage always uses the
portfolio and the query cache (`--solver-cache DIR`); `--timeout S` is the
budget per query; `--certified` sets `SolveOptions::certified`, and a
function becomes `PROVED-CERTIFIED` only when it has at least one VC and
every one of its VCs came back `certified` (a function with none stays
`PROVED`, `certify_note = "no verification conditions (nothing to
certify)"`). Memory-model VCs are QF_BV (the default `MemEncoding::Bv`), so
they are certified like the rest. The k-induction step is still answered by
Z3 alone.

## ProbSAT (roadmap 3.3)

`probsat()` in `cnf.cpp` is ProbSAT with the polynomial break-only
distribution (cb = 2.38 for 3-CNF and up to 5.4 for long clauses, eps = 1).
It keeps an unsatisfied-clause list and true-literal counts. It is a
counterexample finder only. Its CUDA mirror, `src/cuda/probsat.cu` (one
walker per thread, host re-check of the assignment), is built only with
`PRISM_CUDA=ON`. It has been type-checked with clang's CUDA front end
against a stub runtime header, but it has **never been compiled by nvcc or
run**, because this machine has no GPU. The roadmap's GPU exit criterion
("finds counterexamples faster than CPU-only") is not measured.

## Tools

The solvers are built from source under `~/.prism/tools/<name>/<commit>/bin/`:

- CaDiCaL: `./configure && make`
- Kissat: `./configure && make`
- cake_lpr: `make`, which assembles the shipped CakeML `cake_lpr.S` with gcc
- drat-trim: `make`, which gives `drat-trim` and `lrat-check`

- Bitwuzla 0.9.1: `python scripts/fetch_deps.py --tool bitwuzla` (meson +
  ninja, needs the GMP and MPFR development packages). The recipe clones its
  CaDiCaL subproject by pinned commit, because the meson wrap's GitHub archive
  download is refused by some proxies.

A member takes part whenever its binary is found. The exact commits are
listed in `TRUSTED_BASE.md` §4 and `third_party/MANIFEST.toml`.

## Measurement: the conformance suite's pir VCs (roadmap 3.1 exit criterion)

The exit criterion is "the portfolio beats Z3 alone on total time over the
conformance suite's VCs". `tools/solver_bench.py` measures exactly that:
`prism --pir-vcs` writes every VC of every encodable function of the 263
conformance tasks (`tests/conformance/prism` and `sv-comp`, unwind 8), and
`prism --solve-smt2` answers each VC three times back to back: Z3 alone
(`--z3-only`), the portfolio with no history (a fresh history per VC), and
the portfolio scheduling from the history it built on the VCs before
(what the pir stage does across a run). Query cache off, timeout 30 s per
VC, solver wall time summed (process start-up excluded), one VC at a time.

```
python tools/solver_bench.py --prism build/prism --out solver-bench-out
```

Result (2026-09-23, 4 cores shared with other agents' builds, load
average 7–9 during the run):

| pass | total solver s | solved | sat / unsat | timeouts | answered by |
|---|---:|---:|---|---:|---|
| Z3 alone | 229.0 | 4179 / 4179 | 499 / 3680 | 0 | z3 4179 |
| portfolio, no history | 225.7 | 4179 / 4179 | 499 / 3680 | 0 | z3 4158, cadical 12, sls 7, kissat 2 |
| portfolio + history | **211.4** | 4179 / 4179 | 499 / 3680 | 0 | z3 3923, cadical 255, kissat 1 |

- **The criterion is met on this suite, narrowly:** −1.4 % without history
  and −7.7 % with it. No VC disagreed between the passes.
- The wins are few and large: `char_promote_true` (16.6 s → 0.3 s,
  CaDiCaL), `widen_mul_true` (7.7 s → 3.8 s, Kissat) and the sv-comp `jain_*`
  counterexamples (2.5–3.5 s → 0.05–0.4 s, CaDiCaL / ProbSAT).
- The losses are contention: `long_mul_true` (a 64-bit multiplication, 4.7 s
  for Z3 alone) takes 16.9 s once the other members join Z3 on the cores.
- **The Z3 head start matters.** Before it, the portfolio started every
  member at once on each of the 4179 mostly trivial VCs and paid a bit-blast
  and two process spawns each time: 532 s against Z3's 444 s in a
  pass-after-pass run. Those pass-after-pass runs (all three passes one after
  the other) are **not** usable as evidence either way: a first run gave the
  portfolio a win (556 s vs 612 s) only because the test suite was running
  during the Z3 pass, and a Z3-only rerun of the final binary under lower load
  took 270 s where the first pass had taken 449 s. The interleaved run above
  is the only fair one.
- 65 conformance functions are not encoded by pir (pointer parameters,
  arrays): their VCs do not exist and are not in the count.

## Learned scheduler (roadmap 3.1 "learned scheduler", 9.3), measured 2026-09-23

**Data.** `PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*"`
with `DIR/roots.txt` = `tests/pir`, `tests/conformance` (prism, sv-comp,
esbmc-cpp, libc-models, concurrency) and `testdata`: 2,247 source files, in a
hash-shuffled order. Every encodable function goes through pir; for up to 6
VCs per function (unwind 8) **every member runs alone**: Z3, Bitwuzla,
CaDiCaL, Kissat (bit-blast included) with an 8 s timeout, and the ProbSAT
walker with a 0.25 s budget. A timeout is a censored time (">= 8 s"). Result:
3,172 VC records, **2,060 distinct VCs** (identical SMT-LIB2 kept once) from
412 files; 1,745 functions with the verdict at unwind 1, 2, 4, 8, 16.

- Fastest member alone: Bitwuzla 1,065, Z3 663, Kissat 174, CaDiCaL 111,
  ProbSAT 46; one VC (`long_mul_true`, a 64-bit multiplication) times out in
  every member.
- 1,816 unsat, 243 sat, **0 disagreements** between members.
- Almost everything is easy: median 19–29 ms per member, p90 33–63 ms.

**Split.** 30% of the **source files** held out (deterministic hash of the
path): 1,461 training VCs from 279 files, 599 held-out VCs from 133 files.
No VC is in both halves.

**Noise.** The 679 held-out files were collected a second time
(`collect-repeat`) and the rule policy replayed on both: 10–16% apart. The
machine had 4 cores shared with five other agents (load average 12–29), so
every number below is inflated and noisy.

**Replay** (`tools/prism_ai/sched.py`). The portfolio scheduler of
`portfolio.cpp` replayed on the alone-times: expected-time order, the leader's
head start (else Z3's 0.15 s), `k` cores, first answer wins, 8 s timeout. It
assumes no contention between cores and a bit-blast per DIMACS member.
Held-out totals over 599 VCs:

| policy | k = 1 total s / timeouts | k = 2 | k = 5 | median s | p90 s | k = 2 vs rules (95% CI, files resampled) |
|---|---|---|---|---:|---:|---|
| rules (Z3 first, today without history) | 26.67 / 1 | 17.31 / 0 | 17.31 / 0 | 0.022 | 0.043 | |
| history (per-bucket means, today with history) | 16.73 / 0 | 15.89 / 0 | 15.88 / 0 | 0.021 | 0.041 | −13.3% .. −2.2% |
| **GBDT** (squared loss, timeout = 8 s) | **13.05 / 0** | **13.02 / 0** | **13.00 / 0** | 0.018 | 0.033 | −30.7% .. −18.2% |
| AFT GBDT (censored Tobit loss) | 13.05 / 0 | 13.02 / 0 | 13.00 / 0 | 0.018 | 0.033 | −30.7% .. −18.2% |
| k-NN (k = 15, median log time) | 14.78 / 0 | 13.97 / 0 | 13.93 / 0 | 0.019 | 0.034 | −26.5% .. −10.5% |
| winner classifier (k-NN vote), first member only | 13.60 / 0 | 13.36 / 0 | 13.31 / 0 | 0.019 | 0.033 | −29.2% .. −15.6% |
| same models, ordering only (no head start) | = rules | = rules | = rules | | | ±0 |
| static rule "Bitwuzla first" (diagnostic) | 13.32 / 0 | 13.09 / 0 | 13.04 / 0 | 0.018 | 0.033 | −30.6% .. −17.2% |
| oracle (fastest member alone) | 10.82 / 0 | | | | | |

**End to end** (the real portfolio, `prism --solve-smt2`, 781 VCs of the
133 held-out files, 8 s timeout, the four passes interleaved per VC so load
changes hit them alike; solver wall time summed):

| run | rules | rules again (noise) | history | GBDT model |
|---|---:|---:|---:|---:|
| 1 | 59.97 s | 58.08 s (3.2%) | 55.89 s | 56.66 s (48.66 s without the one stall) |
| 2 | 43.35 s | 43.56 s (0.5%) | 41.73 s | **35.91 s** (−17.2% vs rules, −13.9% vs history) |

No verdict differed between passes (0 disagreements). Winners with the
model: Bitwuzla 720, Z3 60, CaDiCaL 1.

**Decision: on by default.** The GBDT beats the rules and the history on
held-out files at every core count, by more than the run-to-run noise
(replay: −25% vs rules, −18% vs history at k = 2, noise ≤ 15.7%; end-to-end
run 2: −17% / −14% with 0.5% noise), with no more timeouts. It is exported
as `src/prism/solver/predict_default.inc` (`predict.py --emit-inc`), is
deterministic (fixed trees, no randomness), only sets order and head start,
never applies to certified requests, and `PRISM_SOLVER_PREDICT=0` restores
the rules. What to know about it:

- **The gain is "Bitwuzla first".** Bitwuzla only started answering pir VCs
  with the portable SMT-LIB2 fix above; a static "Bitwuzla first" rule is
  within 1% of the model in the replay. The model still lets Z3 lead where
  it was faster in training (Z3 won 60 of the 781 end-to-end VCs), but a
  reader should not expect more from it than that.
- **Without Bitwuzla** (a machine that has not run
  `fetch_deps.py --tool bitwuzla`) the model is within noise of the rules:
  replay k = 2 18.98 s vs 18.60 s (+2%), k = 1 identical.
- **Ordering alone does nothing** on these VCs: every "-order" variant equals
  the rules, because Z3's 0.15 s head start already answers most of them.
  All of the gain is in who gets the head start.
- **AFT = GBDT** here: only 8 of about 10,000 member runs were censored, too few
  for a censored loss to change a split.
- **Two unexplained stalls.** Once a Bitwuzla-alone collection run reported
  117 s on a VC Bitwuzla answers in 16 ms (8 s timeout), and once an
  end-to-end model pass did not return within 120 s (the stall above, counted
  as a timeout). Neither reproduced (400 reruns of the second VC, worst
  0.44 s). At one check the machine had about 1 GB of free memory and no swap.
  A stall costs time, never an answer.
- These VCs are small. On hard queries (the generated benchmark above) the
  training data has almost nothing to say; `<cache>/solve_log.jsonl` keeps
  collecting production lines for a retrain.

**Unwind prediction (9.3), off.** 525 held-out functions; the unwind tried
first vs the verdict at unwind 16:

| policy | agreement with unwind 16 | seconds | first counterexample | BOUNDED | no verdict |
|---|---:|---:|---:|---:|---:|
| fixed 8 (today) | 99.4% | 66.2 | 38.0 | 21 | 0 |
| fixed 16 | 100% | 115.9 | 74.1 | 19 | 0 |
| GBDT | 98.3% | 52.9 | 28.2 | 24 | 0 |
| k-NN (largest neighbour label) | 99.6% | 112.8 | 69.1 | 20 | 0 |
| oracle | 100% | 52.3 | 22.7 | 19 | 0 |

Run-to-run noise of the fixed-8 time: 12%. The GBDT is faster by more than
the noise but **loses verdicts** (3 more BOUNDED); the k-NN keeps them (one
BOUNDED fewer) but costs 70% more time. Neither passes "no verdict lost and
less time", so the unwind model stays off and is not wired into `pir`. No
policy can change soundness: a smaller unwind only turns a verdict into an
honest `BOUNDED`, and no function was `PROVED` at a small unwind and `FAILED`
at 16 (0 contradictions).

Reproduce (REP: the same collection with `files_done.txt` pre-filled with
the training files, so only held-out files are timed again):

```
python tools/prism_ai/sched_e2e.py --prism build/prism --solve-log DIR/solve_runs.jsonl --out E2E
python tools/prism_ai/predict.py --solve-log DIR/solve_runs.jsonl --bound-log DIR/bound_runs.jsonl \
  --repeat-solve-log REP/solve_runs.jsonl --repeat-bound-log REP/bound_runs.jsonl \
  --end-to-end E2E/e2e.json --out model.json --emit-inc src/prism/solver/predict_default.inc
```

## Measurement: portfolio vs Z3 alone (generated queries)

The run was `./prism_tests -tc="solver bench*" --no-skip` (the case is
skipped by default). The benchmark has 14 generated bitvector queries:

- unsat equivalence of `a*b` and an unrolled shift-and-add multiplier (8, 10
  and 12 bits);
- unsat `udiv`/`urem` identity (8, 12 and 16 bits);
- sat factoring of a semiprime without overflow (24, 28, 32 and 40 bits);
- unsat naive popcount against SWAR popcount (32 and 64 bits);
- two easy identities.

The timeout is 30 s per query, and a timeout counts as 30 s. There were
three passes: Z3 alone (`portfolio=false`), the portfolio with an empty
solve-time history, and the portfolio again scheduling from that history.
The query cache was off for all three.

**Machine caveat:** 4 cores shared with several other build and test jobs.
The load average was **37–46** for the whole run, so every number is
inflated and noisy. The run shows the parallel members competing for cores,
not solver speed on an idle machine.

| query | Z3 alone (s) | | portfolio, no history (s) | | winner | portfolio + history (s) | | winner |
|---|---:|---|---:|---|---|---:|---|---|
| mulimpl8 | 2.846 | unsat | 4.239 | unsat | cadical | 1.360 | unsat | cadical |
| mulimpl10 | 30.016 | timeout | 30.049 | timeout | | 30.013 | timeout | |
| mulimpl12 | 30.036 | timeout | 30.064 | timeout | | 30.052 | timeout | |
| divmod8 | 0.444 | unsat | 0.590 | unsat | cadical | 0.396 | unsat | cadical |
| divmod12 | 28.139 | unsat | 30.060 | timeout | | 30.036 | timeout | |
| divmod16 | 30.021 | timeout | 30.043 | timeout | | 30.041 | timeout | |
| factor24 | 1.075 | sat | 1.006 | sat | kissat | 0.317 | sat | kissat |
| factor28 | 10.281 | sat | 0.654 | sat | kissat | 0.528 | sat | kissat |
| factor32 | 30.024 | timeout | 0.814 | sat | kissat | 0.608 | sat | kissat |
| factor40 | 30.021 | timeout | 22.957 | sat | cadical | 7.346 | sat | cadical |
| popcount32 | 0.831 | unsat | 3.145 | unsat | z3 | 0.665 | unsat | z3 |
| popcount64 | 8.413 | unsat | 25.131 | unsat | z3 | 7.233 | unsat | z3 |
| xorswap32 | 0.023 | unsat | 0.039 | unsat | z3 | 0.033 | unsat | z3 |
| shiftadd32 | 0.021 | unsat | 0.034 | unsat | z3 | 0.045 | unsat | z3 |
| **total** | **202.2** | 9 solved | **178.8** | 10 solved | | **138.7** | 10 solved | |

What the run shows:

- **SAT factoring.** Kissat and CaDiCaL solve the factoring queries far
  faster than Z3's internal SAT engine. factor32 and factor40 time out for Z3
  alone.
- **Contention with no history.** Without history the portfolio loses badly
  on the arithmetic-heavy unsat queries that Z3 wins, because Z3 then shares
  the cores. popcount64 went from 8.4 s to 25.1 s.
- **History fixes most of that.** Once history exists, the head start brings
  those queries back to about Z3's own time (popcount64 7.2 s).
- **divmod12 is a real loss.** Z3 alone proves it in 28.1 s, and the
  portfolio times out in both passes. Z3 needs nearly the whole budget, and
  once the other members join it no longer gets enough CPU. The buckets are
  coarse: divmod12 shares `QF_BV|w32|n100|muldiv` with the factoring queries,
  where Kissat won. So the history gives Kissat, not Z3, the head start. A
  finer bucket, or the learned model the roadmap plans, is needed here.
- **Totals.** The portfolio beats Z3 alone on total time on this small set:
  138.7 s with history and 178.8 s without, against 202.2 s. It also solves
  one more query. This set is not the roadmap's conformance suite, so the
  3.1 exit criterion is **not** established by it.

Two earlier runs of the same set under the same load, both without history
scheduling, gave these totals (Z3 alone vs portfolio):

- before the SLS cap: 201.5 s vs 197.6 s;
- after the SLS cap: 208.1 s vs 187.9 s.


### With Bitwuzla (2026-09-23)

The same benchmark was rerun with Bitwuzla 0.9.1 installed. The machine was
shared with a conformance run, so the wall times are noisy; the verdicts are
not. Totals were 190.9 s for Z3 alone, 164.9 s for the portfolio, and 121.3 s
for the portfolio with scheduler history.

| query | Z3 alone | portfolio (winner) |
|---|---|---|
| mulimpl8 | 2.8 s unsat | 1.7 s unsat (bitwuzla) |
| mulimpl10 | timeout | 9.7 s unsat (bitwuzla) |
| divmod12 | 21.1 s unsat | 28.1 s unsat (bitwuzla) |
| factor32 | timeout | 0.7 s sat (kissat) |
| factor40 | timeout | 23.1 s sat (cadical) |
| popcount64 | 6.4 s unsat | 28.2 s unsat (z3; 6.9 s with history) |

Bitwuzla closes `mulimpl10`, which Z3 alone does not close in 30 s. Its
UNSAT answers stay plain `PROVED`, never `PROVED-CERTIFIED`: only CaDiCaL LRAT
proofs checked by cake_lpr are certified.
