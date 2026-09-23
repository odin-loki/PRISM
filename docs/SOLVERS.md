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
- The ProbSAT walker gets `max(1 s, 10 % of timeout)` and then gives its core
  back.
- `max_parallel` defaults to the hardware thread count.

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

## Use in the pir stage

The `pir` stage sends every verification condition through `solve()`: one
query per inserted property and one for the unwinding assertion
([PIR.md](PIR.md#solving-roadmap-31--32)). The stage always uses the
portfolio and the query cache (`--solver-cache DIR`); `--timeout S` is the
budget per query; `--certified` sets `SolveOptions::certified`, and a
function becomes `PROVED-CERTIFIED` only when every one of its VCs came back
`certified`. The k-induction step is still answered by Z3 alone.

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

Bitwuzla was not built. It needs meson plus the GMP and MPFR development
packages, none of which are on this machine. It takes part whenever a
`bitwuzla` binary is found. The exact commits are listed in `TRUSTED_BASE.md`
§4.

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
