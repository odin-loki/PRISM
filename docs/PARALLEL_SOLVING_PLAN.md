# Parallel solving plan: SAT, SMT and CDCL at full machine width

Written 13 November 2026 from a literature survey (sources inline) and the solver code as it stands
(`src/prism/solver/portfolio.cpp`, `src/prism/pir/encode.cpp`, `src/prism/pir/houdini.inc`). Companion to
[GPU_PLAN.md](GPU_PLAN.md) (what the GPU does) and [UPGRADE_PLAN.md](UPGRADE_PLAN.md) (P0 to P6). Nothing here
is measured on PRISM yet; every gain below is a target with a gate. Published speed-ups are the authors' own
and several come from preprints (marked).

## 1. What the survey changes about the plan

1. **A 25 ms query cannot be parallelised from the inside.** PRISM's median VC takes 19 to 29 ms per solver
   member (`docs/SOLVERS.md`). Thread start-up, clause sharing and cubing cost more than that. The parallelism
   PRISM is missing is *across* queries: today one function's property VCs are solved one after another
   (`encode.cpp`, the per-property loop after the group query), each as a fresh portfolio race of up to
   `max(2, cores/jobs)` members, and functions inside a unit are sequential too. So the first win is
   scheduling, not a parallel solver.
2. **Parallel CDCL helps only the tail** (queries past 1 to 2 s), and there the best parallel method that keeps
   certified mode is **cube-and-conquer with per-cube LRAT**, which PRISM can build from parts it already has.
3. **The 16-bit multiply/divide time-outs are an encoding problem.** Carry-save (Comba) and Booth radix-4
   multiplier encodings solve in seconds what shift-add bit-blasting does not solve in 120 s (Biere, Brain,
   Kroening, Manthey, Tautschnig, PoS 2026, https://ceur-ws.org/Vol-4289/paper6.pdf: 16-bit commutativity
   5.4 s against a time-out; up to 64x across four solvers). They stay pure CNF, so LRAT still works.
4. **No GPU CDCL beats CPU CDCL on UNSAT**, and PRISM's VCs are mostly UNSAT (properties that hold). GPU
   hybrids report large gains on satisfiable instances only (TurboSAT, arXiv 2511.07737, preprint; GaloisSAT,
   arXiv 2603.28796, preprint: 8.4x SAT, 1.29x UNSAT against Kissat). The GPU stays a witness finder
   (GPU_PLAN G1, G7), never a refutation engine.

## 2. Workstreams, in order of wall-clock gain per effort

### S1. Solve across VCs, not inside them (largest win, small effort)

- **VC-level parallelism.** A function's property VCs (and the unwinding assertion) are independent queries
  on one encoding. Run them concurrently on a solver worker pool (FORGE task pool for in-process Z3 contexts,
  one context per worker; external members as child processes as today). Keep the first-violated-property
  rule by taking, among SAT answers, the property earliest in order, and answering earlier pending ones
  before reporting; the group query and halving stay as the first step.
- **Function-level parallelism inside a unit** (UPGRADE_PLAN P1.1): one task per function, not per unit, so
  a 5 MB kernel unit with 100 functions uses the machine.
- **Race the portfolio only when it pays.** The GBDT scheduler already predicts per-member time. Below a
  predicted 1 s, run Z3 alone in process (no spawns, no bit-blast); race the full portfolio only above it.
  That frees the 4 to 5 cores a 25 ms query now occupies for other VCs.
- **Memory:** every concurrent Z3 context is charged to the process memory budget (`membudget`), which
  already interrupts contexts; the pool shrinks its width when the budget is past half.
- Gate: on the P0 benchmark, functions per hour at least 4x at `--jobs 32` on 64 threads, identical verdicts
  on the conformance suite (0 wrong proofs), the same FAILED property chosen per function.

### S2. Incremental solving for the many related queries (large win, medium effort)

Houdini rounds, k-induction steps and the properties of one function are many queries over one encoding.

- **One persistent solver per function.** Assert the encoding once; each property is an activation literal
  passed as an assumption. Z3 in process supports this today (`check(assumptions)`, unsat cores); CaDiCaL 2.0
  supports it with incremental proofs (Biere et al., CAV'24,
  https://cca.informatik.uni-freiburg.de/papers/BiereFallerFazekasFleuryFroleyksPollitt-CAV24-Springer.pdf;
  IDRUP/LIDRUP, LPAR'24, https://cca.informatik.uni-freiburg.de/papers/FazekasPollittFleuryBiere-LPAR24.pdf).
- **Houdini on assumptions.** Each candidate invariant gets an activation literal; one query per round; the
  counterexample model drops every candidate it falsifies at once, and learned clauses carry over between
  rounds. With GPU_PLAN G3 the GPU removes falsified candidates before the first round.
- **Parallel Houdini.** No strong published method exists (closest: arXiv 1612.01198, 1.1 to 1.27x). Do our own:
  split candidates over N incremental solvers, broadcast every drop to all of them; the final inductive set is
  checked once on one solver, so correctness does not depend on the split.
- **Certified mode:** LIDRUP is not what cake_lpr reads. Re-solve the final query of a PROVED function
  non-incrementally with CaDiCaL LRAT, as certified mode does today; the incremental run only finds the answer.
- Gate: Houdini wall time halved on the loop corpus (`npr`, `strcat`, the libc model loops) with identical
  verdicts; k-induction query count per function down.

### S3. Better multiplier and divider encodings (targets the 30 s tail, keeps LRAT)

- Add carry-save (Comba) and Booth radix-4 multiplier encodings to PRISM's bit-blaster; encode `udiv`/`urem`
  (and signed forms) as `a = b*q + r` with `r < b` (and the Z3 total-division case for `b = 0`) on the same
  multiplier. Choose the encoding per VC (the GBDT gets the operand widths as features), or run two encodings
  as two portfolio members.
- The Lean-proved bit-blaster (`prism-bitblast`, `proofs/techniques`) must get the same encodings with their
  correctness proofs before certified mode may use them; until then they run only in the uncertified
  members, and certified mode keeps the proved encoding (Law: a certificate covers only the CNF the proved
  bit-blaster made; `docs/TRUSTED_BASE.md`).
- Gate: `mulimpl10`, `divmod16` and the multiplier VCs of `docs/SOLVERS.md` answered inside 30 s; no verdict
  change elsewhere.

### S4. Certified cube-and-conquer for the tail (the parallel method that keeps certificates)

- Trigger: a query still running past its predicted time (or past 2 s), or every query of `--certified` that
  CaDiCaL alone does not close in its first slice.
- Split: cubes on the high bits of the multiplicands, divisors and loop-bound variables (look-ahead cubing
  as in march_cu, or simply the top k bits of the widest arithmetic operands); 32 to 64 cubes.
- Solve each cube `F and C_i` with CaDiCaL plus LRAT as its own process (no clause sharing, so no proof
  merging problem). A SAT cube is a counterexample, validated in Z3 as every model is now.
- Combine: lift each cube refutation to a derivation of the clause `not C_i`, add the step that the cubes
  cover every case (the `not C_i` together are unsatisfiable, itself a small LRAT proof), renumber clause ids
  and concatenate into one LRAT proof for cake_lpr and Lean's checker. This is Heule's method for the
  Pythagorean triples and Schur number five proofs (https://arxiv.org/abs/1605.00723,
  https://cs.utexas.edu/~marijn/Schur), without the size problem: PRISM's cubes are few.
- Gate: on certified mode, the tasks that timed out (`long_mul_true`, wide multiplications) get
  PROVED-CERTIFIED or a measured reason; every combined proof checks in cake_lpr; 64-core speed-up on the
  tail recorded.

### S5. Parallel solvers as extra, uncertified tail members

- **Z3 parallel mode** (`parallel.enable=true`, cube-and-conquer). A rewrite with a dynamic partition tree,
  core-guided backjumping and backbones is in Shapiro, Lerner, Bjorner, FMCAD 2026
  (https://ilanashapiro.github.io/files/parallel_z3.pdf); check whether the vendored Z3 has it before relying on
  it (`third_party/MANIFEST.toml` pin).
- **cvc5 partitioning** (`--compute-partitions`, Wilson, Notzli, Reynolds, Cook, Tinelli, Barrett, FMCAD 2023,
  https://arxiv.org/abs/2306.05854): mixed strategies beat more partitions; PRISM runs the partitions as
  processes. cvc5 is a new external tool: `prism-deps tool` manifest row and `VENDOR_DIR` entry (CLAUDE.md).
- **Local search for SAT queries:** Z3 `smt.sls.enable=true`
  (https://microsoft.github.io/z3guide/programming/Local%20Search/), Bitwuzla's propagation-based local search,
  the GPU walkers (GPU_PLAN G7). Their answers are models, validated in Z3 as today.
- None of these makes a certificate; a PROVED from them is PROVED, never PROVED-CERTIFIED.

### S6. Clause-sharing portfolios, only if S4 is not enough

- **Mallob** (MIT since 2.0, 2025, https://github.com/domschrei/mallob; distributed LRAT proof production,
  TACAS'23, https://link.springer.com/chapter/10.1007/978-3-031-30823-9_18; won the parallel tracks of SAT
  Competition 2025 and 2026, as reported by its authors). Its proof-producing configuration uses CaDiCaL and is
  weaker than its fastest one, and composing the proof cost about 3x the solve time at 1,500 threads. Heavy
  (MPI); adopt only if certified cube-and-conquer leaves a measured gap.
- **Gimsatul** (MIT, https://github.com/arminbiere/gimsatul): fast shared-memory clause sharing, DRAT only
  (needs drat-trim before cake_lpr), not incremental. An uncertified tail member at most.

### S7. Caching (complements everything above)

The query cache keys on the normalised formula; add the whole-function verdict cache (UPGRADE_PLAN P1.2, on
`forge::cache::store`) and keep the LRAT proof with a certified result, so a replayed function costs a
lookup and a proof check.

## 3. What not to do

- GPU CDCL for UNSAT VCs (no published method wins on UNSAT; GPU_PLAN section 1).
- ParaFROST: GPL (cannot be linked, `prism-deps licence-check`), and GPU simplification does not pay on 25 ms CNFs.
- Kissat or gimsatul inside certified mode (DRAT, not LRAT).
- Counting a Z3-parallel or cvc5-partition answer as certified.
- Bitwuzla's multiplication abstraction for 16-bit work: it applies from 33 bits up by default.

## 4. Order and gates

S1 first (it also gives the P0 baseline its throughput), then S2 and S3 in parallel, then S4 on the measured
tail, S5 as members, S6 only on a measured gap. Every step keeps 0 wrong proofs on the conformance suite and
every certificate checking in cake_lpr; a step whose gate is not met is turned off, as the unwind predictor was.
