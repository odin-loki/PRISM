# Symbolic similarity search (`prism sim`)

Odin, 18 Nov 2026: "an ANN similarity search on the database that judges similarity like a symbolic regression would do
well... the symbolic overload in PRISM. So that will mean we are using FastSMT and an ANN similarity search to speed up
search." This is the PRISM side: a metric for FORGE's ANN template (`forge::ann`, see the FORGE PR `leandb-ann`) over
SMT-LIB formulas and translatable Lean goals, an opt-in CLI, a strategy-hint API for the FastSMT port, and a Lean-goal
adapter. **Opt-in only.** No verdict path, solver default or certified path reads any of it; the strategy hints are not
wired into the solver (the FastSMT port is Odin's).

## What is compared

Symbolic regression ranks expressions by shape and by the values they produce on sample inputs. The metric does both
(`include/prism/sim/symbolic_metric.hpp`, `src/sim/`).

1. **Term and canonical form.** The term type is PRISM's own `z3::expr` (what `solver/query.cpp` and the pir encoder use),
   parsed by Z3's SMT-LIB reader; `sim::from_z3` copies it into a small hash-consed DAG (Bool, QF_BV, linear Int, arrays,
   opaque applications; iterative, so let-chains thousands deep are fine). `canonicalise` flattens and orders the AC
   operators (`bvadd bvmul bvand bvor bvxor and or xor`; `=`, `distinct`, `bvnand/nor/xnor`, `bvcomp` are only ordered),
   unwraps one-argument `and`/`or` (pir VCs have them), and renames variables by first use in a depth-first walk of the
   ordered term. Operands are ordered by a variable-name-free structural hash; operands that tie (they differ only by which
   variables they use) keep their input order, so the form is sound but not complete (graph canonicalisation, not an
   isomorphism test). Literals keep their value in the term (the evaluator needs it); the structural vector sees
   0, 1, -1, powers of two, MIN and MAX of the sort and abstracts every other literal to its sort and width.
2. **Structural vector.** For every node and every depth 1..3, the hash of the subtree cut at that depth (variables hash by
   sort only, AC children sorted), weight 1/depth; an operator histogram (log2(1 + count)); bit widths; array nesting
   depth; size and variable-count buckets. Sparse, sorted by feature id. Distance: 1 - weighted Jaccard.
3. **Semantic signature.** The term is evaluated on K = 128 fixed seeded assignments: 24 diagonal probes (every variable
   the same corner value: 0, 1, -1, MIN, MAX, 2, 4, 16, 0x55.., 0xAA.., 3, 255), 64 probes with an independent corner per
   variable, 40 uniformly random. The output of each probe is hashed into 2 bits (a Bool is replicated); the signature is
   256 bits. Distance: Hamming / 256.
   * Variables are matched by canonical order, and because structure can order the variables of two equivalent formulas
     differently (`x - y` against `(-y) + x`), the signature is the lexicographic minimum over all permutations of the
     variable slots inside each sort group when the total number of permutations is at most 24 (up to 4 variables of one
     sort). With more variables the canonical order alone is used.
   * Formulas with different variable counts or sorts are compared on the same probe set: an unused variable changes
     nothing; a variable of another sort gets a value from that sort's own generator, so the formulas then differ wherever
     they depend on it. Sorts are never coerced.
   * Semantics: Bool, QF_BV with SMT-LIB division by zero and shifts of at least the width, Int as wrapping signed 128 bits
     (probed with small values), arrays as a base hash plus store overrides. Everything else (UF, FP, Real, bit-vectors wider
     than the 128-bit evaluation word; 64 bits in the MSVC build) is an opaque function of its arguments' values: equal
     applications agree, nothing more is claimed.
   * A signature that is the same on every probe (`sym_vec::sem_constant`) carries little information. Verification
     conditions that are almost never true have this property; their structure then decides.
4. **Combined.** `d = alpha * d_struct + (1 - alpha) * d_sem`, `alpha = 0.5` by default (`symbolic_metric{alpha}`). It is a
   pseudo-metric in [0, 1]: d(x, x) = 0 and d is symmetric (tested); alpha- and AC-equivalent formulas have the same
   sym_vec, so d = 0. The triangle inequality holds for each part (Jaccard distance, Hamming) but the semantic part is a
   minimum over permutations, so it is only approximately a metric; HNSW does not need more.
5. **ANN.** `forge::ann::hnsw<sym_vec, symbolic_metric>` (and `flat_index` for the exact result), `forge::ann::codec<sym_vec>`
   for persistence. Optional reranking: `rerank` with the exact metric on the top candidates, then `rerank_ted`, a bounded
   Zhang-Shasha tree edit distance on the tree expansions of the canonical terms (at most 160 nodes each, otherwise the
   candidate keeps its distance).

## Use

```
prism sim index DIR_OR_FILES... [-o prism-sim.idx] [--labels strategies.tsv] [--threads N] [--alpha A]
prism sim query FILE.smt2 [-i prism-sim.idx] [-k 10] [--exact] [--ted]
prism sim hint  FILE.smt2 [-i prism-sim.idx] [-k 15]
```

`index` takes `.smt2` files or directories of them, and `.goals` files (`id <TAB> Lean statement` per line, translated by the
Lean adapter). `--labels` is a TSV `name <TAB> strategy <TAB> seconds`; the strategy and time become the entry's label.
The index file is `SIM1` + entries; the HNSW graph is rebuilt on load (deterministic for a fixed seed).

* **Strategy hints (for the FastSMT port).** `sim::strategy_hint(index, sym_vec, {k, power, min_sim})`: the k nearest
  labelled formulas vote on their strategies with weight `(1 - d)^power`; the result is ranked by score, then mean
  recorded seconds (ascending), then name; it is empty when no neighbour is labelled. API and test only.
* **Lean goals.** `sim::lean_goal_to_term` (include/prism/sim/lean_goal.hpp) translates `theorem`/`lemma`/`example`
  statements over BitVec n, Nat/Int (+ - * / %), Bool and Fin with `∀` binders and hypotheses, into the same term type; the
  Lean database stores the resulting sym_vec for every statement that translates, and `prism leandb search --mode sym|hybrid`
  uses it (docs/LEANDB.md). Nat is an Int without the non-negativity side condition and Nat subtraction is not truncated
  (the adapter says so in its header); Fin n is an Int. What does not translate (Real, structures, lists, `x.toNat`, ...)
  is reported with a reason, never guessed.

## Tests (`tests/cpp/test_sim.cpp`)

Pseudo-metric laws on eight formulas (d(x,x) = 0, symmetry, [0,1], distinct formulas at d > 0); alpha, AC and combined
renamings at d = 0; literal abstraction; 20 term pairs that Z3 proves equivalent in the test (`x*2` vs `x<<1`, `x-y` vs
`x+(-y)`, `~(~x)`, both De Morgan laws, `-x` vs `~x+1`, rotations, `ashr` by 31, ...) all at semantic distance 0; the
evaluator checked against Z3's own `simplify` on 19 operators (signed division, remainders, shifts, concat, extend, rotate,
repeat, ...); 18 pairs Z3 proves non-equivalent, of which the signature misses only the single-point differences (the miss
rate is printed); HNSW recall@10 against the exact index on 600 random bit-vector formulas; codec and index round trip;
strategy vote on a synthetic labelled set; tree edit distance; Lean goals equal to the SMT formula they translate.

## Measured (18 Nov 2026)

Corpus: PRISM's own verification conditions. `PRISM_SIM_EVAL=DIR prism_tests -tc="sim eval collect*"` lowers the C and C++
sources under `testdata/` through the pir front end and writes the first 6 VCs per function (unwind 8); then
`-tc="sim eval measure*"` embeds, indexes and measures. **The collection was stopped by hand after 994 distinct VCs from 164
files** (in path order, so it is `testdata/conformance/prism/{c,cxx,...}` up to `cxx_auto_false.cpp`; `testdata/pir`,
`planted`, `tp`, `fp` were not reached). Exact duplicate SMT-LIB text was kept once. The machine had a load average of about
50 on 64 cores, so all times are inflated and noisy. Raw output: `sim_eval.json` from that run (numbers below are copied from it).

| measurement | value |
|---|---|
| VCs / mean DAG nodes | 994 / 47 |
| embed (canonicalise, features, 128-probe signature) | 24 ms per formula, one thread (the CLI uses all cores: 1009 files in 7.1 s with 4 threads) |
| HNSW build (M 16, efConstruction 200) | 1.9 s for 994 vectors |
| query, exact scan / HNSW | 1.1 ms / 0.25 ms (ef 32), 0.36 ms (ef 64), 0.57 ms (ef 128) |
| HNSW recall@10 against the exact index (300 queries) | 0.9994 (ef 32), 0.9997 (ef 64, 128, 256) |

**Near duplicates** (nearest other formula, after exact-text deduplication): **56%** of the VCs have a neighbour at d = 0
(same canonical form and same signature), **71%** within d < 0.05, **87%** within d < 0.10. For 58% the nearest neighbour is
structurally identical (it differs by literals, variable names or operand order), and for 39% it has the same signature but a
different structure. So most of this corpus is made of a few hundred shapes; exact-hash deduplication (plan step 3.4) leaves
more than half of it as near duplicates, and clustering (step 3.7) has real redundancy to remove.

**The semantic part is nearly constant on this corpus.** 92.8% of the VCs evaluate to the same output on all 128 probes
(`sem_constant`): a VC is satisfiable only when the property is violated, which random and corner inputs almost never hit.
So on pir VCs the metric is in practice the structural one; the semantic distance earns its keep on formulas that are
frequently true (the equivalence tests, Lean goals). A probe set that targets the formula's own constants (for example
values taken from its equalities) would help; it is not built.

**Strategy selection (FastSMT setting), held out by source file.** Labels: each VC solved alone by Z3 under six strategies
(default solver, `qfbv`, `simplify;bit-blast;sat`, `smt`, `simplify;solve-eqs;bit-blast;sat`, `simplify;propagate-values;
ctx-simplify;smt`), 5 s timeout, best of 3 runs, answers identical wherever two strategies answered (0 disagreements).
Split by `sha256(file)`: 697 train VCs, 297 test VCs (30% of files). Total seconds on the 297 test VCs:

| policy | seconds |
|---|---:|
| plain Z3 (default solver) | 0.546 |
| best single strategy chosen on train (`propagate-smt`) | 7.094 |
| kNN vote (`strategy_hint`, k = 15; the neighbours' best strategies vote) | 5.217 |
| kNN regression of each strategy's log time over the 15 neighbours, take the smallest (evaluation only) | 0.417 |
| the same, deviating from the default only when predicted at least 20% faster | 0.460 (19 deviations) |
| oracle (fastest strategy per VC) | 0.377 |

What this says, without dressing it up. (1) The vote that the strategy-hint API implements is much worse than plain Z3 here:
the neighbours' best strategy is usually a tie of microseconds, and where it is wrong it is wrong by seconds (the tail:
`qfbv` totals 2.4 s and `propagate-smt` 7.1 s on the test set against 0.55 s for the default). A vote of winners ignores how
badly a strategy loses. (2) Predicting each strategy's time from the neighbours and taking the minimum recovers most of the
oracle gap (0.417 s against 0.546 s default and 0.377 s oracle, 24% under the default), but the totals are about 1.4 ms per VC,
the difference is 0.13 s over 297 VCs, there is no confidence interval, and the run was on a loaded machine: treat it as
"headroom exists, the time-regression form finds some of it", not as a measured speed-up. (3) These VCs are easy (the median
Z3 time is about 1 ms), as docs/SOLVERS.md found for the portfolio; the stop rule of the FastSMT plan (no gain on kernel VCs:
stop) cannot be decided from this corpus. (4) The portfolio replay data of docs/SOLVERS.md (the 599 held-out VCs and the learned
Bitwuzla-first scheduler) records only sha256 and features, not the SMT-LIB text, so it cannot be joined to formulas and the
comparison with the plain **portfolio** was not made; the baseline here is plain Z3.

**Plan for a proper labelled set.** Re-run the collection (`prism_tests -tc="ai-assist predict collect*"`) with the SMT-LIB
text written next to each record (one line in `test_ai_assist.cpp`, `vc.smt2`), over the clang-pass kernel VCs, label every
formula with time per strategy at a fixed timeout (docs/PLAN_FASTSMT_TRAINING_AND_DATA.md step 5), split by file, and run
`sim eval measure` on it. The API should then also take the per-strategy time table of each labelled formula (the regression
form), not only its single best strategy; this run shows why.

**Lean goals.** `lean_goal_to_term` is tested against hand-written goals and on the fixture database; the translation rate on
the real database is in `docs/LEANDB.md` ("Measured").
