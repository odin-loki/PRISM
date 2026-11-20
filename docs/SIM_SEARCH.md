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
3. **Semantic signature** (`src/sim/eval.cpp`). The term is evaluated on K = 128 fixed seeded assignments: 24 diagonal
   probes (every variable the same corner value: 0, 1, -1, MIN, MAX, 2, 4, 16, 0x55.., 0xAA.., 3, 255), 64 probes with an
   independent corner per variable, 40 uniformly random. Three parts, all in `sym_vec`:
   * **Root signature** `sem`: the root's output on every probe, 2 bits each (a Bool is replicated, anything else hashed),
     256 bits. Variables are matched by canonical order, and because structure can order the variables of two equivalent
     formulas differently, the signature is the lexicographic minimum over all permutations of the variable slots inside
     each sort group when there are at most 24 of them in total (up to 4 variables of one sort); with more, the canonical
     order alone is used. This invariance is exactly alpha-equivalence: a consistent renaming of all variables, which is what
     the canonical form already identifies. So `x - y` and `y - x` are alpha-equivalent and have distance 0 under the default
     metric (the law "alpha-equivalent formulas have d = 0" requires it).
   * **Sub-term fingerprints** `sub`: a verification condition is a Boolean that is almost never true, so its root signature
     is constant (92.8% of the PRISM VCs measured below). The evaluator therefore also hashes, for every non-Boolean,
     non-trivial sub-term (not a variable or literal), its values on every 4th probe into one 64-bit fingerprint, keeps the 64
     largest sub-terms, and stores the sorted set (again the lexicographic minimum over the slot permutations). Equal computed
     sub-functions (`a*b + 5`) get equal fingerprints whatever the formula around them and whatever literal they are compared
     with.
   * **Name-keyed root signature** `named`: the root signature with variables slotted by the rank of their name hash instead
     of canonical order, so it is position- and name-sensitive: `x - y` and `y - x` differ. It is not alpha-invariant, so it
     is off by default (`symbolic_metric::named_weight = 0`); with a weight it mixes in `named_distance` for corpora where
     names are shared and meaningful.
   * **Distance** (`semantic_distance`): Hamming over the root signature when the root varies over the probes or is not a
     Boolean (terms such as `x ^ x` and `0` are the same function); when both roots are Boolean and constant, 1 - Jaccard of
     the sub-term fingerprint sets. The switch means the triangle inequality can fail; d(x,x) = 0, symmetry and [0,1] hold.
   * Formulas with different variable counts or sorts are compared on the same probe set: an unused variable changes
     nothing; a variable of another sort gets a value from that sort's own generator. Sorts are never coerced.
   * Semantics: Bool, QF_BV with SMT-LIB division by zero and shifts of at least the width, Int as wrapping signed 128 bits
     (probed with small values), arrays as a base hash plus store overrides. Everything else (UF, FP, Real, bit-vectors wider
     than the 128-bit evaluation word; 64 bits in the MSVC build) is an opaque function of its arguments' values.
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

* **Strategy hints (for the FastSMT port).** `sim::strategy_hint(index, sym_vec, hint_params)`. Entries carry the table of
  seconds of every strategy tried on them (`sym_index::add(name, vec, best, seconds, times)`; the TSV for `--labels` takes a
  4th column `strategy=seconds,...`). The default method is **kNN regression of expected time**: each strategy's predicted
  seconds is the similarity-weighted (`(1 - d)^power`) arithmetic mean of its recorded seconds over the k nearest labelled
  formulas, the result is ranked by predicted seconds, and a **guard** keeps `default_strategy` first unless the best
  prediction is at least `min_gain` (20%) faster. The arithmetic mean is deliberate: a geometric mean ignores a strategy that
  takes seconds on one neighbour in fifteen. The **vote** (`how = vote`, `prism sim hint --vote`) ranks the neighbours' best
  strategies by similarity-weighted votes and stays as an option; without time tables the regression falls back to it. API
  and test only, not wired into the solver.
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
strategy hints on a synthetic labelled set (regression default, a case where the vote picks a strategy that loses 5 s on a third of the neighbours and the regression does not, the vote option, the fall back to the vote); name-keyed `x - y` against `y - x`; sub-term separation of constant formulas; tree edit distance; Lean goals equal to the SMT formula they translate.

## Measured (18 Nov 2026, second round)

Corpus: PRISM's own verification conditions. `PRISM_SIM_EVAL=DIR prism_tests -tc="sim eval collect*"` lowers the C and C++
sources under `testdata/` through the pir front end and writes the first 6 VCs per function (unwind 8); then
`-tc="sim eval measure*"` embeds, indexes and measures. **The collection was stopped by hand after 994 distinct VCs from 164
files** (in path order: `testdata/conformance/prism/...` up to `cxx_auto_false.cpp`; `testdata/pir`, `planted`, `tp`, `fp`
were not reached). Exact duplicate SMT-LIB text was kept once. The machine had a load average of about 50 on 64 cores, so all
times are inflated and noisy (the embed time varied from 13 to 39 ms between runs).

| measurement | first round (root signature only) | now (root + sub-term fingerprints) |
|---|---|---|
| VCs whose root signature is constant | 92.8% | 92.8% (unchanged: the root is the same) |
| VCs with an informative semantic part (varying root, or a non-empty fingerprint set) | 7.2% | **99.9%** |
| nearest other VC at d = 0 | 56% | 48% |
| nearest other VC at d < 0.05 / d < 0.10 | 71% / 87% | 61% / 76% |
| nearest VC structurally identical / same semantics, different structure | 58% / 39% | 50% / 36% |
| embed, one thread | 24 ms per formula | 13 to 39 ms |
| HNSW build, 994 vectors (M 16, efConstruction 200) | 1.9 s | 1.7 s |
| HNSW recall@10, ef 32 / 64 / 128 / 256 | 0.9994 / 0.9997 / 0.9997 / 0.9997 | 0.984 / 0.996 / 0.998 / 1.000 |
| query, exact / HNSW (ef 64) | 1.1 ms / 0.36 ms | 0.86 ms / 0.36 ms |

The sub-term fingerprints make the semantic part informative on nearly every VC, and they separate formulas the structure
alone treats as one (the near-duplicate rate at d = 0 drops from 56% to 48%). FORGE measured recall@10 of 0.86 at ef 64 on
hard random data; this corpus is redundant, and the default `ef_search` of `sym_index` is 128.

**Strategy selection (FastSMT setting), held out by source file.** Labels: each VC solved alone by Z3 under six strategies
(default solver, `qfbv`, `simplify;bit-blast;sat`, `smt`, `simplify;solve-eqs;bit-blast;sat`, `simplify;propagate-values;
ctx-simplify;smt`), 5 s timeout, best of 3 runs, answers identical wherever two strategies answered (0 disagreements).
Split by `sha256(file)`: 697 train VCs, 297 test VCs. `alpha` (the weight of the structural part) is chosen on a validation
split of the train files (never on the test set). Total seconds on the 297 test VCs, API default (expected-time regression with
the 20% guard) at the validated `alpha = 1.0`:

| policy | seconds |
|---|---:|
| plain Z3 (default solver) | 0.546 |
| best single strategy chosen on train (`propagate-smt`) | 7.094 |
| **API default: kNN expected-time regression, guarded** | **0.404** |
| the same, unguarded | 0.402 |
| vote (option) | 0.434 |
| oracle (fastest strategy per VC) | 0.377 |

Typical formula (geometric mean of time / default time, 1 ms offset): API 0.910, vote 0.923, oracle 0.879. Formulas more than
25% faster / slower than the default: API 17 / 5, vote 13 / 5. No API pick lost more than 50 ms against the default.

**What this does and does not show.** (1) The API default is 26% under plain Z3 in total (0.404 s against 0.546 s) and within
7% of the oracle, with no large loss. The difference is 0.14 s over 297 VCs, there is no confidence interval, and the run was
on a loaded machine. (2) **The comparison is fragile.** One test formula (`vcs/863.smt2`) costs 5 s under every strategy but
the default; whichever method picks it dominates the total. In the first round the vote lost (5.2 s) and a log-time regression
won (0.42 s); with the new signature the vote wins (0.43 s) and the same log-time regression loses (5.4 s); with the
expected-time regression the totals are 0.40 s at `alpha = 1.0` and 5.4 s at every `alpha < 1`, because the semantic part pulls
that one formula's neighbours towards a dangerous strategy. The geometric-mean ratios are stable across all of these (0.91 to
0.98). So "regression beats vote" is **not** established by this data, and neither is the value of the semantic component for
strategy selection: validation picked `alpha = 1.0` (structure only). (3) These VCs are easy (the median Z3 time is about 1
ms), as docs/SOLVERS.md found for the portfolio; the stop rule of the FastSMT plan (no gain on kernel VCs: stop) cannot be
decided from this corpus. (4) The portfolio replay data of docs/SOLVERS.md records only sha256 and features, not the SMT-LIB
text, so it cannot be joined to formulas; the baseline here is plain Z3, not the portfolio.

**Plan for a proper labelled set.** Re-run the collection (`prism_tests -tc="ai-assist predict collect*"`) with the SMT-LIB
text written next to each record (one line in `test_ai_assist.cpp`, `vc.smt2`), over the clang-pass kernel VCs, label every
formula with time per strategy at a fixed timeout (docs/PLAN_FASTSMT_TRAINING_AND_DATA.md step 5), split by file, and report
the geometric-mean ratio and a bootstrap interval over files next to the totals.

**Lean goals.** `lean_goal_to_term` is tested against hand-written goals and on the fixture database; the translation rate on
the real database is in `docs/LEANDB.md` ("Measured").
