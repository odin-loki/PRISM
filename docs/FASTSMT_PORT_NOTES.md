# FastSMT: notes for a port into PRISM's solver layer

Status: notes only. Nothing from FastSMT is in this repository, nothing here
changes a solver default, and the certified path is not touched. Read from
`eth-sri/fastsmt` at commit `27e39866db08` (2.1 MB tree, Apache-2.0, copyright
ETH Zurich SRI Lab 2018); the facts below are from its sources and README, not
from running it. Paper: Balunovic, Bielik, Vechev, "Learning to Solve SMT
Formulas", NeurIPS 2018 (the speed numbers are in the paper; they were not
re-checked here).

## What it is

FastSMT is Z3 plus a learned **tactic strategy**. For a dataset of formulas it
searches the space of Z3 tactic compositions (with parameters), guided by a
neural model, and writes out one strategy in Z3's own SMT2 tactic syntax. Z3
then runs that strategy; FastSMT is not a different solver and has no solving
code of its own. A strategy chooses, per formula, among several tactic
pipelines by testing Z3 *probes* (`num-consts`, `num-exprs`, `size`, ...).

## Components (6,575 lines of Python, 179 lines of C++)

| Part | Where | Lines | Job |
|---|---|---|---|
| Strategy language | `fastsmt/language/objects.py` | 267 | `Tactic`, `AndThen`, `OrElse`, `With`, `Probe` wrappers on Z3 objects; `to_smt2()`; the list `ALL_TACTICS` (15 names: simplify, smt, bit-blast, bv1-blast, solve-eqs, aig, qfnra-nlsat, sat, max-bv-sharing, reduce-bv-size, purify-arith, propagate-values, elim-uncnstr, ackermannize_bv, skip) and `ALL_PROBES` (12) |
| Runner | `cpp/goal_runner.cpp` (+ `utils/runner.py`, `runners.py`, `parallel_run.py`) | 179 + ~380 | Applies a strategy to one formula in a **child process**: parse SMT2, `tactic(ctx, ...)` from a `Tactic(..)`/`AndThen(..)`/`With(..;k=v)` string, apply to a goal, print `res rlimit old_hash new_hash seconds` and 15 probe values of the result goal. `rlimit count` is Z3's deterministic work counter, used as the cost measure instead of wall time |
| Search | `synthesis/search_strategies.py`, `evo_strategy.py`, `learning.py`, `search/{models,neural_nets,dataset}.py` | ~2,000 | Beam/evolutionary search over tactic sequences; a neural model (tokenised formula features and tactic history, TensorboardX logging) scores candidate extensions; alternates random search and model-guided search for `--num_iters` rounds |
| Multi-strategy synthesis | `synthesis/multi/{synthesizer,tree,predicates,generator,multi_synthesis}.py` | ~1,400 | Combines the best per-formula strategies into ONE decision tree over probe predicates (`(> num-consts 48)`, `(> size 444)`, ...) with a strategy at each leaf; prints it as SMT2 (`tree.get_smt2`) |
| Evaluation | `scripts/py/validate.py`, `experiments/eval_vs_z3.py` | 185 + 270 | Runs plain Z3 and the final strategy per formula with a timeout; counts solved and total time |
| Dataset | `scripts/py/dataset_create.py` | 77 | Splits a directory `all/` of `.smt2` into `train/ valid/ test/` (default in the README 50/20/30) **by file** |

Theories: the README example is QF_NIA (leipzig). The experiment scripts and
`experiments/final_strategies/smt2/` also cover QF_NRA (hycomp, sage2), QF_NIA
(AProVE) and **QF_BV** (`bruttomesso/core`; the shipped QF_BV strategy is
`core_mul.txt`). So QF_BV is in the original work; its formulas are the
SMT-LIB `core` family, not verification conditions from C code.

## The strategy language

Z3's SMT2 tactic syntax, no extension:

```
(if (> num-consts 26)
    (then (using-params simplify :pull_cheap_ite true :hoist_mul true :push_ite_bv true
                                  :local_ctx false :elim_and false :flat true :som false)
          bv1-blast elim-uncnstr smt)
    (then simplify bit-blast sat))
```

Combinators used: `then`, `if` (probe comparison `>`, `<`, `=` against an
integer), `using-params` (tactic + `:key value` with booleans and unsigned
numbers), plain tactic names, `skip`. `or-else` exists in the Python wrapper.
The strategy is applied with `(check-sat-using <strategy>)`, or in C++ by
building the tactic from the tree (`tactic`, `&`, `with`, `cond`/`probe`).

## What depends on Z3 4.6.2

FastSMT pins Z3 4.6.2 (commit `5651d00751a1`), Python 3.5, Ubuntu 16.04
(README). PRISM links Z3 4.13.4 (`third_party/MANIFEST.toml`). The coupling:

- **Tactic and parameter names.** The strategy text names Z3 tactics and
  parameters. Checked against `third_party/z3` (4.13.4): `bv1-blast`,
  `ackermannize_bv`, `qfnra-nlsat`, `max-bv-sharing`, `ctx-simplify`,
  `reduce-bv-size`, `purify-arith`, `elim-uncnstr`, `nla2bv`
  (`nla2bv_max_bv_size`) and the simplify parameters `hoist_mul`,
  `push_ite_bv`, `blast_distinct`, `hi_div0` still exist; the `:aig_per_assertion`
  parameter of `aig` (seen in `sage2_mul.txt`) was **not found** in 4.13.4. A
  port should validate every tactic and parameter name of a strategy against
  the linked Z3 when loading it, and refuse (or drop) the unknown ones with a
  note, not silently.
- **Probes.** The runner reads 15 probes; `ALL_PROBES` has 12
  (`num-consts num-exprs size depth ackr-bound-probe is-qfbv-eq arith-max-deg
  arith-avg-deg arith-max-bw arith-avg-bw is-unbounded is-pb`). The set differs
  per Z3 version; for QF_BV only `num-consts`, `num-exprs`, `size`, `depth` and
  `is-qfbv-eq` mean anything.
- **Defaults changed.** A strategy tuned on 4.6.2 may be slower or faster on
  4.13.4 because the tactic internals and defaults moved (4.13 has the new
  simplifier pipeline and `sat`/`smt` tactic changes). **Strategies must be
  re-learned or at least re-measured on the linked Z3**; the shipped ones are
  not evidence for PRISM's Z3.
- **The cost signal.** `rlimit count` through `solver.statistics()` exists in
  4.13.4 (`src/util/statistics.cpp`), so the deterministic cost can be kept.
- **Python API.** The Python side uses `z3.Tactic`, `AndThen`, `With`,
  `parse_smt2_file`, `Z3_benchmark_to_smtlib_string`. Nothing else of Z3.
- The neural model, tokeniser and the search are Z3-version independent, but
  Python-3.5-era (TensorboardX, old PyTorch API); they are the part that is
  not needed at run time.

## What a port into PRISM touches

Run time needs only: (a) a strategy loaded from a file, (b) building the Z3
tactic from it, (c) applying it as one more portfolio member. The search and
the neural net stay offline (Python or otherwise) and write a strategy file.

| File | Change |
|---|---|
| `src/prism/solver/portfolio.cpp` | The `MemberKind::Z3` job (`z3::solver s(zc)`, around line 1140) builds `z3::tactic` from the strategy and uses `t.mk_solver()` instead; as a second member (`"z3-fastsmt"`, added next to `add("z3", ...)`) so plain Z3 stays. `rule_estimate` and the head-start logic need a row for it. SAT answers already go through the model validation in Z3; keep that. `z3_worker_main` (`prism z3-worker`) can run `(check-sat-using S)` for the child-process case. |
| `src/prism/solver/predict.cpp`, `gbdt.cpp`, `predict_default.json` | The GBDT predicts seconds per member from query features; a new member needs its own tree (train it on the same corpus) or it is simply not predicted (it then falls back to the rule estimate). `features()` already counts nodes, widths, mul/div; these play the role of FastSMT's probes. |
| `src/prism/solver/query.cpp`, `certstore.cpp` | The cache key is of the normalised formula, so it is strategy independent: an answer from a strategy is cached like any other. A cached UNSAT must stay plain; **the certified path (CaDiCaL + LRAT, Lean bit-blaster) must not use the strategy**: it re-derives its own CNF. `SolveOptions` gets a path to the strategy file (default off); `docs/SOLVERS.md` gets a row in the member table. |
| `include/prism/solver.hpp`, `src/prism/config.cpp`, `src/prism/cli.cpp` | An option/env var (`PRISM_*`, registered in `env_vars()`; `tests/cpp/test_env_registry.cpp` fails otherwise). |
| `docs/TRUSTED_BASE.md`, `docs/VERDICTS.md` | A strategy changes only which Z3 tactics run; an UNSAT is still "trusted solver, plain". A speedup is not a proof and no verdict wording changes. |

A small C++ tactic-expression reader (about 150 lines: `then`, `if` with a
probe test, `using-params`, names) is enough, since the strategy language is
Z3's own and tiny. Z3's C API can also parse it: put the strategy in a
`(check-sat-using ...)` script and evaluate it with `Z3_eval_smtlib2_string`
(what `prism z3-worker` already does) at the cost of re-parsing the formula.

## Training on PRISM's own queries

`scripts/dump_qfbv_corpus.sh` writes the queries and the split:

```
scripts/dump_qfbv_corpus.sh --prism build/prism --out /dev/shm/qfbv-corpus \
    --units-from units.txt [--compile-commands build/compile_commands.json]
```

It keeps only QF_BV queries (`rejected.tsv` lists the rest), splits by
**source file** (hash of the unit path, 80/10/10 by default), and records
sizes in `manifest.tsv` / `summary.txt`. FastSMT's own `dataset_create.py`
expects `all/` and makes `train/ valid/ test/`; rename `validation` to `valid`
or copy. Train only on `train/`, choose by `validation/`, report only on
`test/`.

## Evaluating on held-out queries

1. Fix the Z3 (the linked 4.13.4) and the timeout (the pir stage's per-query
   `--timeout`, default 30 s). Machine otherwise idle; one query at a time, as
   `prism-qa solver-bench` does (`docs/SOLVERS.md`, "Measurement").
2. For every query of `test/`, run (a) plain Z3 (`prism --solve-smt2 Q
   --z3-only`), (b) the current portfolio (`prism --solve-smt2 Q`), (c) the
   portfolio with the strategy member. Query cache off, a fresh history for
   (b) and (c) so neither learns from the other.
3. Record per query: answer, wall seconds, winner. Charge a timeout at the
   timeout. Report solved count and total time **and the share of the corpus
   the strategy covers** (queries it answered), per source file as well as in
   total (a few large files can dominate).
4. **Answers-agree check.** For every query two configurations both answered,
   `sat`/`unsat` must be equal. Any disagreement is a bug (in the strategy
   loader, in a tactic, or in the portfolio) to be reported with the query
   file, never averaged away. SAT models are validated in Z3 as always, so a
   wrong `sat` is caught there; a wrong `unsat` is not, which is why this
   check compares against plain Z3 and, for QF_BV, can be settled by the
   certificate path on the disagreeing queries.
5. If there is no gain, say so: with few training files, QF_BV queries that
   Z3 answers in milliseconds (most pir VCs, `docs/SOLVERS.md`), and the
   portfolio already winning on the hard ones, a strategy has little to win.
