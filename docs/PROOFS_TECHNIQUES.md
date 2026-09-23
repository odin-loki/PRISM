# PRISM verification techniques, proved in Lean 4

This document covers the Lean project `proofs/techniques/`. It proves that the
*algorithms and designs* behind several PRISM verdicts are sound. These are
roadmap 5.4 (bit-blaster) and the 8.2 rows "k-induction", "Houdini invariant
filter", "Contracts and PROVED-ASSUMING", "Bit-blaster", "Concurrency (lazy
sequentialisation)" and, in part, "Floating point" (the rounding kernel; the
IEEE operations, exception flags and PRISM's floating-point checks are in
`proofs/refinement`, [PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md)).

It is a separate Lake project from `proofs/` (verdict lattice) and
`proofs/semantics/` (PIR semantics). It has its own toolchain and no
dependencies: core Lean 4 and its bundled `Std` only, no Mathlib.

| | |
|---|---|
| Toolchain | `leanprover/lean4:v4.34.0` (`proofs/techniques/lean-toolchain`) |
| Build | `cd proofs/techniques && lake build` |
| Axiom audit | `lake env lean PrismTechniques/Audit.lean` (also runs as part of `lake build`) |
| End-to-end certified mode | `lake exe certified-demo "$(dirname "$(elan which lean)")/cadical"` |
| Proved bit-blaster as a program | `lake exe prism-bitblast < formula.sexp` (DIMACS + variable map) |
| Lean's verified LRAT checker as a program | `lake exe prism-lrat-check [--dag formula.sexp] query.cnf proof.lrat` |
| CI | `.github/workflows/proofs-techniques.yml` |

## Trust statement

- **No unfinished proofs, no new axioms.** No file contains `sorry`, `admit`
  or `native_decide`, and no file declares an `axiom`; CI greps for all of
  them.
- **The audit is enforced.** `PrismTechniques/Audit.lean` defines
  `#assert_axioms`. It collects every axiom a theorem depends on,
  transitively, and fails the build on anything other than `propext`,
  `Classical.choice` and `Quot.sound`. That catches `sorryAx`, and also
  `Lean.ofReduceBool`, the axiom `native_decide` and `bv_decide` add to trust
  compiled code. A negative test was run locally: a `sorry` theorem and a
  `native_decide` theorem both fail the audit.
- **The trusted base.** For every result below it is the Lean 4 kernel plus
  the three standard axioms. For `certified_unsat` it also includes the proofs
  inside core Lean's `Std` (the LRAT checker and its soundness proof), which
  the same kernel checks.

## Axioms (output of `#assert_axioms`, recorded 2026-09-23)

```
KInduction.reach_iff_path                  [propext, Quot.sound]
KInduction.kinduction_sound                [propext, Quot.sound]
KInduction.step_iff_havoc_query_unsat      [propext, Classical.choice, Quot.sound]
KInduction.step_iff_havoc_split            []
KInduction.step_one_iff                    [propext, Quot.sound]
KInduction.step_mono                       [propext, Quot.sound]
KInduction.kinduction_strengthened         [propext, Quot.sound]
KInduction.kinduction_rel_sound            [propext, Quot.sound]
KInduction.kinduction_frame_sound          [propext, Quot.sound]
KInduction.step_frame_of_step              []
Houdini.houdini_rounds_le                  [propext, Quot.sound]
Houdini.houdini_inductive                  [propext, Quot.sound]
Houdini.houdini_sound                      [propext, Quot.sound]
Houdini.houdini_maximal                    [propext, Quot.sound]
Houdini.houdini_then_kinduction            [propext, Quot.sound]
Contracts.modular_sound                    [propext]
Contracts.compose_layer                    [propext]
Contracts.contract_violation_breaks_modularity []
Contracts.proved_assuming_is_implication   []
Contracts.proved_assuming_not_proved       [propext]
Contracts.discharge                        []
Contracts.harness_assumption_discharged    [propext]
Contracts.caller_safe                      [propext]
Bitblast.encode_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.sat_of_cnf_sat                    [propext, Classical.choice, Quot.sound]
Bitblast.cnf_sat_of_sat                    [propext, Classical.choice, Quot.sound]
Bitblast.toCNF_equisat                     [propext, Classical.choice, Quot.sound]
Bitblast.toCNF_unsat_imp                   [propext, Classical.choice, Quot.sound]
Bitblast.certified_unsat                   [propext, Classical.choice, Quot.sound]
Bitblast.subOp_spec                        [propext, Classical.choice, Quot.sound]
Bitblast.negOp_spec                        [propext, Classical.choice, Quot.sound]
Bitblast.uleOp_spec                        [propext, Classical.choice, Quot.sound]
Bitblast.sleOp_spec                        [propext, Classical.choice, Quot.sound]
Bitblast.shlOp_spec                        [propext, Classical.choice, Quot.sound]
Bitblast.lshrOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.ashrOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.shlW_rep                          [propext, Quot.sound]
Bitblast.lshrW_rep                         [propext, Quot.sound]
Bitblast.ashrW_rep                         [propext, Classical.choice, Quot.sound]
Bitblast.zextW_rep                         [propext, Quot.sound]
Bitblast.sextW_rep                         [propext, Classical.choice, Quot.sound]
Bitblast.extractW_rep                      [propext, Quot.sound]
Bitblast.concatOp_spec                     [propext, Quot.sound]
Bitblast.uaddoOp_spec                      [propext, Classical.choice, Quot.sound]
Bitblast.saddoOp_spec                      [propext, Classical.choice, Quot.sound]
Bitblast.usuboOp_spec                      [propext, Classical.choice, Quot.sound]
Bitblast.ssuboOp_spec                      [propext, Classical.choice, Quot.sound]
Bitblast.umuloOp_spec                      [propext, Classical.choice, Quot.sound]
Bitblast.smulHiOp_spec                     [propext, Classical.choice, Quot.sound]
Bitblast.smulLoOp_spec                     [propext, Classical.choice, Quot.sound]
Bitblast.smulOverflow_hi_lo                [propext]
Bitblast.smulOverflow_expr                 [propext, Quot.sound]
Bitblast.udivOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.uremOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.sdivOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.sremOp_spec                       [propext, Classical.choice, Quot.sound]
Bitblast.BVExpr.denote_congr               [propext, Quot.sound]
Bitblast.dag_sat_imp                       [propext, Classical.choice, Quot.sound]
Bitblast.certified_dag_unsat               [propext, Classical.choice, Quot.sound]
Bitblast.checkDag_sound                    [propext, Classical.choice, Quot.sound]
Bitblast.denote_eq_denoteExec              [propext, Quot.sound]
LazySeq.lazy_seq_covers                    [propext, Quot.sound]
LazySeq.lazy_seq_sound                     [propext, Quot.sound]
LazySeq.lazy_seq_reach_iff                 [propext, Quot.sound]
LazySeqN.slots_of_star                     [propext, Quot.sound]
LazySeqN.star_of_slots                     [propext, Quot.sound]
LazySeqN.slots_mono                        [propext, Quot.sound]
LazySeqN.length_prismSched                 [propext]
LazySeqN.rr_covers_runs                    [propext, Classical.choice, Quot.sound]
LazySeqN.lazy_sound                        [propext, Quot.sound]
LazySeqN.lazy_covers_runs                  [propext, Classical.choice, Quot.sound]
LazySeqN.lazy_covers                       [propext, Classical.choice, Quot.sound]
LazySeqN.lazy_between                      [propext, Classical.choice, Quot.sound]
LazySeqN.lazy_covers_two                   [propext, Classical.choice, Quot.sound]
LazySeqN.per_thread_bound_not_enough       [propext, Quot.sound]
FloatRound.rne_exact                       [propext, Quot.sound]
FloatRound.rne_half_ulp                    [propext, Quot.sound]
FloatRound.rne_nearest                     [propext, Quot.sound]
FloatRound.rne_tie_even                    [propext, Classical.choice, Quot.sound]
FloatRound.binary16_sig_bound              [propext, Quot.sound]
```

Every name above is in the namespace `PrismTechniques`.

---

## 1. k-induction (`PrismTechniques/KInduction.lean`)

**Model.** A transition system has states `S`, initial states `I : S → Prop`
and a transition relation `T : S → S → Prop`; the property is `P : S → Prop`.
`Reach I T` is the inductive set of reachable states. `Path T π n` means
`∀ i < n, T (π i) (π (i+1))`.

```lean
def Base (I : S → Prop) (T : S → S → Prop) (P : S → Prop) (k : Nat) : Prop :=
  ∀ (π : Nat → S) (n : Nat), n < k → I (π 0) → Path T π n → P (π n)
def Step (T : S → S → Prop) (P : S → Prop) (k : Nat) : Prop :=
  ∀ (π : Nat → S), Path T π k → (∀ i, i < k → P (π i)) → P (π k)
```

| Theorem | Statement |
|---|---|
| `kinduction_sound` | `Base I T P k → Step T P k → ∀ s, Reach I T s → P s`. It holds for every `k`; `k = 0` is degenerate but sound. |
| `step_iff_havoc_query_unsat` | `Step T P k ↔ ¬ HavocQuerySat T P k`, where `HavocQuerySat T P k := ∃ π, Path T π k ∧ (∀ i < k, P (π i)) ∧ ¬ P (π k)`. This is the solver query: the first state `π 0` is havocked (unconstrained, not required to be reachable). |
| `step_iff_havoc_split` | For `S = F × L` (unmodified part × loop-modified part): `Step T P k ↔ ∀ f l π, π 0 = (f, l) → Path T π k → (∀ i < k, P (π i)) → P (π k)`. This only restates the step premise; it says that havocking the loop state is exactly what the premise quantifies over. |
| `step_one_iff` | `Step T P 1 ↔ ∀ s s', P s → T s s' → P s'`: 1-induction is an ordinary inductive invariant. |
| `step_mono` | `Step T P k → Step T P (k+1)`. |
| `kinduction_strengthened` | `Base I T (P ∧ J) k → Step T (P ∧ J) k → ∀ s, Reach I T s → P s`. |
| `kinduction_rel_sound` | `(∀ s, Reach I T s → J s) → Base I T P k → StepRel T J P k → ∀ s, Reach I T s → P s`. Here `StepRel` may assume `J` on all `k+1` window states. |
| `kinduction_frame_sound` | `(∀ s, I s → J s) → (∀ s s', J s → P s → T s s' → J s') → Base I T P k → StepFrame T J P k → ∀ s, Reach I T s → P s`, where `StepFrame` havocs the first window state only up to `J`. This is the pir stage's step for memory-writing loops (docs/PIR.md "k-induction with memory"): `J` = memory equals the prefix's outside the loop's write footprint (initialised flags inside it only grow when every write initialises), same object table. `J`'s preservation by the C++ loop body is not proved here (it needs a frame lemma over `PrismSem/Memory.lean`, which has no initialised flags). |
| `step_frame_of_step` | `Step T P k → StepFrame T J P k`. |

## 2. Houdini (`PrismTechniques/Houdini.lean`)

**Algorithm.** Houdini is a total Lean function. Lean checks that it
terminates: every round that continues makes the list strictly shorter.

```lean
def round (pres : List C → C → Bool) (cs : List C) : List C := cs.filter (pres cs)
def loop (pres) (cs) : List C :=
  if (round pres cs).length < cs.length then loop pres (round pres cs) else cs
termination_by cs.length
def houdini (init : C → Bool) (pres : List C → C → Bool) (cands : List C) : List C :=
  loop pres (cands.filter init)
```

**Oracles.** The solver is modelled by two Boolean oracles, bundled in
`Oracles S C` as `holds`, `init` and `pres`. Soundness needs only that a
`true` answer is right:

- `InitSound O I := ∀ c, O.init c = true → ∀ s, I s → O.holds c s`
- `PresSound O T := ∀ cs c, O.pres cs c = true → ∀ s s', Conj O cs s → T s s' → O.holds c s'`

A wrong "no", a timeout or an "unknown" only removes more candidates.

| Theorem | Statement |
|---|---|
| `houdini_rounds_le` | `rounds pres (cands.filter init) ≤ cands.length + 1` (bounds the number of solver rounds) |
| `houdini_inductive` | `InitSound O I → PresSound O T →` the conjunction `J` of the survivors satisfies `(∀ s, I s → J s) ∧ (∀ s s', J s → T s s' → J s')` |
| `houdini_sound` | `InitSound O I → PresSound O T → ∀ c ∈ houdini O.init O.pres cands, ∀ s, Reach I T s → O.holds c s`, for **any** `cands`, including ones proposed by an AI model |
| `houdini_maximal` | Assumes complete oracles (`InitComplete`, `PresComplete`). Then every `A ⊆ cands` with `Inductive O I T A` satisfies `A ⊆ houdini …`, so the result is the largest inductive subset. |
| `houdini_then_kinduction` | The survivors feed `kinduction_rel_sound`: `Base I T P k → StepRel T (Conj O survivors) P k → ∀ s, Reach I T s → P s` |

## 3. Contracts and `PROVED-ASSUMING` (`PrismTechniques/Contracts.lean`)

**Model.** The language is first-order with calls, over a signature
`Fn`, `Arg Ret : Fn → Type`.

```lean
inductive Prog (Fn) (Arg Ret : Fn → Type) (R : Type)
  | ret (r : R) | ub | call (f : Fn) (a : Arg f) (k : Ret f → Prog Fn Arg Ret R)
def run (impl : (f : Fn) → Arg f → Option (Ret f)) : Prog Fn Arg Ret R → Option R  -- none = UB
def Satisfies impl spec f := ∀ a, (spec f).req a → ∃ b, impl f a = some b ∧ (spec f).ens a b
def WP spec : Prog Fn Arg Ret R → (R → Prop) → Prop
  | .ret r, Q => Q r | .ub, _ => False
  | .call f a k, Q => (spec f).req a ∧ ∀ b, (spec f).ens a b → WP spec (k b) Q
```

| Theorem | Statement |
|---|---|
| `modular_sound` | `(∀ f, Satisfies impl spec f) → WP spec p Q → ∃ r, run impl p = some r ∧ Q r`. There is no UB in the caller or in any called function, and callers may rely on `ensures`. |
| `compose_layer` | If the lower layer satisfies its contracts and each body satisfies `∀ g a, (spec₂ g).req a → WP spec (body g a) ((spec₂ g).ens a)`, then `∀ g, Satisfies (fun g a => run impl (body g a)) spec₂ g`. Applied layer by layer, this covers any acyclic call graph. |
| `contract_violation_breaks_modularity` | There are `impl`, `spec` and `p` with `WP spec p (fun _ => True)` and `run impl p = none`. A caller's proof alone is not whole-program safety. |
| `proved_assuming_is_implication` | `ProvedAssuming A Safe ↔ Proved (fun x => A x → Safe x)` |
| `proved_assuming_not_proved` | `∃ A Safe : Nat → Prop, ProvedAssuming A Safe ∧ ¬ Proved Safe`, witnessed by `100 / x` assuming `x ≠ 0`. |
| `discharge` | `ProvedAssuming A Safe → Proved A → Proved Safe` |
| `harness_assumption_discharged` | A function proved `PROVED-ASSUMING H` (no UB when `H` holds), called only at call sites that prove `H` (`WP` with `req := H`), gives `∃ r, run impl p = some r ∧ Q r`. |
| `caller_safe` | Worked example: a guarded division caller. |

## 4. Bit-blaster (`PrismTechniques/Bitblast*.lean`)

Files: `Bitblast.lean` (bit-level facts, gates, Tseitin clauses, the
builder specification), `BitblastOps.lean` (operator circuits),
`BitblastDiv.lean` (divider), `BitblastEncode.lean` (expression language,
encoder, `toCNF`, the main theorems, sharing), `BitblastSexp.lean` (the text
format the executables read; not part of any proof).

**Fragment** (roadmap 3.2 step 2: everything the PIR verification conditions
use). `BVExpr : Nat → Type`, widths per variable:

| Group | Constructors | Circuit |
|---|---|---|
| Leaves | `var base` (a block of input bits), `const` | wires |
| Bitwise | `not`, `and`, `or`, `xor` | one gate per bit |
| Arithmetic | `add`, `sub`, `neg`, `mul` | ripple-carry adder; `x + ~~~y + 1`; `0 - x`; shift-and-add |
| Division | `udiv`, `urem`, `sdiv`, `srem` | restoring divider (`divRec`), SMT-LIB division by zero, SMT-LIB sign cases |
| Shifts | `shl`, `lshr`, `ashr` by a variable; `shlC`, `lshrC`, `ashrC` by a constant | barrel shifters (`shiftLeftRec` …); wires |
| Width | `zext n`, `sext n`, `extract lo len`, `concat` | wires |
| Choice | `ite` (1-bit condition) | multiplexer |
| Predicates (1 bit) | `eq`, `ult`, `ule`, `slt`, `sle` | xor + and-chain; carry chains |
| Overflow (1 bit) | `uaddo`, `saddo`, `usubo`, `ssubo`, `umulo`, `smulHi`, `smulLo` | carry out; sign tests; double-width products |

Semantics (`BVExpr.denote ρ`) is core Lean's `BitVec` operation for every
constructor: `BitVec.saddOverflow`, `BitVec.umulOverflow`,
`BitVec.sshiftRight'`, `BitVec.extractLsb'`, and so on. The two exceptions
are spelled out: division is SMT-LIB's (`smtUdiv x 0 = allOnes`, `x % 0 = x`,
`smtSdiv`/`smtSrem` by the standard's four sign cases), because Z3 and the
PIR encoder use SMT-LIB semantics; and signed multiplication overflow is
split into its two halves `smulHi` (`2^(w-1) ≤ x.toInt * y.toInt`) and
`smulLo` (`x.toInt * y.toInt < -2^(w-1)`), because Z3's
`bvmul_no_overflow` / `bvmul_no_underflow` are separate predicates;
`smulOverflow_hi_lo` proves `BitVec.smulOverflow = smulHi || smulLo`.
`FSat φ := ∃ ρ, φ.denote ρ = 1#1`.

**Encoding.**

- The encoder builds an and/or/xor circuit and Tseitin-encodes each gate into
  `Std.Sat.CNF Nat`, the CNF type the verified LRAT checker takes.
- Input bit `j` is variable `2j`, variable `1` is the constant `true`, and
  gate `k` is variable `2k+3`.
- `toCNF φ` contains every gate's defining clauses, the unit clause `[1]`,
  and a unit clause asserting the output bit.
- The circuit caches its gate count (`Circuit.len`, invariant `Circuit.WF`)
  and the clauses are emitted by a tail-recursive loop (`clausesAcc`, proved
  equal to the specification `clausesL`), so the compiled encoder is linear
  in the number of gates. Measured: a 64-bit signed-multiplication overflow
  check (396k variables, 660k clauses) in 1.4 s; 20 000 chained 32-bit
  definitions (15.4M clauses) in 34 s.

**Proof structure.**

- `Consistent α c` means that `α` satisfies every gate definition;
  `clausesL_all_iff` proves the clauses hold exactly when it does.
- Every circuit builder is proved against `Rep c ls X` ("under every
  assignment consistent with `c`, the literals `ls` are the bits of `X α`")
  and `Spec`. `Op1`/`Op2` package "the builder computes the `BitVec`
  operation `F`", so `encode_spec` is one line per constructor
  (`case1`/`case2`).
- `extend ρ c` extends any input assignment to a consistent one, by
  evaluating the gates in order (`extendL_consistent`, `extendL_input`).

| Theorem | Statement |
|---|---|
| `encode_spec` | `c.WF → Spec c (encode e c) (fun α => toBits (e.denote (inputOf α)))`, every constructor |
| `sat_of_cnf_sat` | `(toCNF φ).Sat α → φ.denote (inputOf α) = 1#1` |
| `cnf_sat_of_sat` | `φ.denote ρ = 1#1 → (toCNF φ).Sat (extend ρ (encode φ Circuit.empty).2)` |
| `toCNF_equisat` | `(∃ α, (toCNF φ).Sat α) ↔ FSat φ` (equisatisfiable, both directions) |
| `toCNF_unsat_imp` | `(toCNF φ).Unsat → ∀ ρ, φ.denote ρ ≠ 1#1` (the direction certified mode needs) |
| `certified_unsat` | `Std.Tactic.BVDecide.LRAT.check cert (toCNF φ) = true → ∀ ρ, φ.denote ρ ≠ 1#1` |
| `dag_sat_imp` | `defsOK 0 g.defs → g.eval ρ = 1#1 → FSat g.toExpr` |
| `certified_dag_unsat` | `defsOK 0 g.defs → LRAT.check cert (toCNF g.toExpr) = true → ∀ ρ, g.eval ρ ≠ 1#1` |
| `checkDag_sound` | `checkDag g cert = true → ∀ ρ, g.eval ρ ≠ 1#1` (`checkDag` is what `prism-lrat-check --dag` runs) |

**Sharing (`Dag`).** Z3 terms are DAGs; writing one as a tree can be
exponentially larger. A `Dag` is a list of definitions `(w, base, e)` ("input
bits `base … base+w-1` name the value of `e`") and a top formula. Its meaning,
`Dag.eval ρ`, evaluates the definitions in order (`evalDefs`, each on the
assignment so far) and then the top formula. `defsOK` (checked at run time by
both executables) requires each definition to read only bits below its own
base and the definitions to be laid out upwards; `BVExpr.denote_congr` (an
expression depends only on the bits below `reads`) then gives `dag_sat_imp`:
a satisfying input of the DAG extends to one of the flat formula
`(v₁ = e₁) ∧ … ∧ (vₙ = eₙ) ∧ top`.

**Reuse of core Lean's `bv_decide` development.** The arithmetic facts are
core lemmas from `Init.Data.BitVec.Bitblast` / `Lemmas`:

- adder/subtractor: `BitVec.carry`, `carry_succ`, `getLsbD_add`,
  `getLsbD_add_add_bool`, `neg_eq_not_add`;
- comparisons: `ult_eq_not_carry`, `ule_eq_carry`, `slt_eq_ult`,
  `sle_eq_not_slt`, `msb_eq_getLsbD_last`;
- multiplier: `mulRec`, `mulRec_succ_eq`, `getLsbD_mul`;
- shifts: `shiftLeftRec` / `ushiftRightRec` / `sshiftRightRec` with
  `shiftLeft_eq_shiftLeftRec`, `shiftRight_eq_ushiftRightRec`,
  `sshiftRight_eq_sshiftRightRec`, `and_twoPow`, `toNat_twoPow_of_lt`;
- width: `getLsbD_setWidth`, `getLsbD_signExtend`, `getLsbD_extractLsb'`,
  `getLsbD_append`, `getLsbD_shiftConcat`;
- overflow: `saddOverflow_eq`, `ssubOverflow_eq`, `umulOverflow_eq`,
  `two_pow_le_toInt_mul_toInt_iff`, `toInt_mul_toInt_lt_neg_two_pow_iff`;
- division: `divRec`, `divSubtractShift`, `udiv_eq_divRec`,
  `umod_eq_divRec`, `umod_zero`;
- the certificate step: `Std.Tactic.BVDecide.LRAT.check_sound`.

The circuits, the Tseitin clauses, the builder specification and the
equisatisfiability and sharing proofs are PRISM's own; `bv_decide`'s AIG
bit-blaster uses different data structures.

**Executables.** Both are `lean_exe` targets of this project and are built by
`lake build` (and in CI):

- `prism-bitblast` reads a formula in the S-expression format of
  `BitblastSexp.lean` on stdin (`(dag (def W BASE e)* e)`, one constructor
  name per `BVExpr` constructor) and writes `c prism-bitblast 1`, one
  `c var BASE WIDTH V0 … V(W-1)` line per free input variable, and then
  `Std.Sat.CNF.dimacs (dagCNF g)`: exactly `toCNF` of the flattened DAG. The
  executed code is the proved code, modulo the Lean compiler.
  `CNF.dimacs` numbers CNF variable `v` as DIMACS variable `v+1`, so input bit
  `j` is DIMACS variable `2j+1`. `--eval` instead evaluates `Dag.eval` under
  `(rho (BASE WIDTH VALUE)*)` assignments; PRISM's C++ serializer is tested
  against it. (The compiled `denote` is `denoteExec` through the proved `@[csimp]`
  lemma `denote_eq_denoteExec`: it only avoids core's `x.toNat <<< s` for
  huge left-shift amounts, which would abort the program.)
- `prism-lrat-check CNF LRAT` parses DIMACS (strictly) and runs
  `Std.Tactic.BVDecide.LRAT.check`, the checker `certified_unsat` relies on.
  `prism-lrat-check --dag FORMULA CNF LRAT` rebuilds the CNF from the formula
  with the proved bit-blaster, insists that the CNF file is byte for byte
  that CNF, and runs `checkDag g cert` — the exact premise of
  `checkDag_sound`, with no DIMACS parser in between. Both print
  `s VERIFIED UNSAT` (exit 0) or `s NOT VERIFIED` (exit 1).

PRISM's certified mode uses both (`src/prism/solver/leanbb.cpp`, see
`docs/TRUSTED_BASE.md`).

**End to end.** `lake exe certified-demo <cadical>` runs the full certified
path on concrete formulas, one or more per operator group:

1. bit-blast the formula with `toCNF`;
2. write DIMACS;
3. run CaDiCaL, which is shipped inside the Lean toolchain, with `--lrat`;
4. parse the certificate with `Std`'s LRAT parser;
5. run the verified checker.

This is an *executed* check of `certified_unsat`'s premise, not a kernel
proof. The kernel did not reduce `LRAT.check` when tried: `decide +kernel` on
a certificate of 11 steps got stuck. Local run:

```
x <u x: UNSAT, 138 clauses, 67 LRAT steps, verified checker: true
x + y != y + x: UNSAT, 330 clauses, 177 LRAT steps, verified checker: true
x <s y && y <s x: UNSAT, 293 clauses, 50 LRAT steps, verified checker: true
(x & y) != ~(~x | ~y): UNSAT, 106 clauses, 34 LRAT steps, verified checker: true
ite(x <u y, x, y) >u y: UNSAT, 362 clauses, 50 LRAT steps, verified checker: true
x * 3 != x + x + x: UNSAT, 1634 clauses, 540 LRAT steps, verified checker: true
x * y != y * x (6-bit): UNSAT, 1520 clauses, 4389 LRAT steps, verified checker: true
x - y != x + -y: UNSAT, 466 clauses, 314 LRAT steps, verified checker: true
x << 1 != x + x (shift by a variable-width amount): UNSAT, 898 clauses, 191 LRAT steps, verified checker: true
(x >>a 7) != -(x >>l 7): UNSAT, 194 clauses, 91 LRAT steps, verified checker: true
saddo(x,y) differs from sext9 x + sext9 y != sext9 (x + y): UNSAT, 505 clauses, 224 LRAT steps, verified checker: true
ssubo(x,y) differs from sext9 x - sext9 y != sext9 (x - y): UNSAT, 505 clauses, 244 LRAT steps, verified checker: true
umulo(x,y) differs from the high half of zext x * zext y being nonzero (6-bit): UNSAT, 6084 clauses, 1909 LRAT steps, verified checker: true
smulo(x,y) differs from sext12 x * sext12 y != sext12 (x * y) (6-bit): UNSAT, 10003 clauses, 21023 LRAT steps, verified checker: true
x != (x udiv y) * y + (x urem y) (6-bit): UNSAT, 3116 clauses, 5395 LRAT steps, verified checker: true
x != (x sdiv y) * y + (x srem y) (6-bit): UNSAT, 4128 clauses, 11970 LRAT steps, verified checker: true
x udiv 0 != ~0 (SMT-LIB division by zero): UNSAT, 1994 clauses, 125 LRAT steps, verified checker: true
concat (extract 4 4 x) (extract 0 4 x) != x: UNSAT, 58 clauses, 49 LRAT steps, verified checker: true
x <u y (satisfiable): SAT (138 clauses) — no certificate, nothing to check
x * y = 35 (satisfiable): SAT (1362 clauses) — no certificate, nothing to check
```

## 5. Lazy sequentialisation (stretch) (`PrismTechniques/LazySeq.lean`)

**Scope.** The model is deliberately small:

- two threads, each a straight-line list of atomic actions `G → G → Prop`
  over a shared state `G` (actions may be nondeterministic or blocking);
- the real interleaving semantics, `Step`/`Star`, counts context switches;
- the sequentialised program `SeqLR K` is the Lal–Reps round-robin
  guess-and-check reduction.
  - Thread 1 runs all `K` rounds first, on round-indexed copies of the shared
    state. Each round after the first starts from a guessed state.
  - Thread 2 then runs its `K` rounds.
  - A run is kept only if the state thread 2 reaches at the end of round `r`
    equals the guess for round `r+1`.

Several things are not modelled:

- Lazy-CSeq's *lazy* re-execution of prefixes;
- loops inside threads;
- thread creation and more than two threads;
- memory models weaker than sequential consistency.

| Theorem | Statement |
|---|---|
| `lazy_seq_covers` | `Star ⟨P1, P2, g0, true, 0⟩ ⟨q1, q2, g', c, sw⟩ → sw + 1 ≤ 2*K → ∃ e1 e2, P1 = e1 ++ q1 ∧ P2 = e2 ++ q2 ∧ SeqLR K e1 e2 g0 g'`. Every interleaving with at most `2K−1` switches, finished or not, is covered. |
| `lazy_seq_sound` | `0 < K → SeqLR K e1 e2 g0 g' → ∃ c sw, sw + 1 ≤ 2*K ∧ Star ⟨e1 ++ q1, e2 ++ q2, g0, true, 0⟩ ⟨q1, q2, g', c, sw⟩`. There are no spurious runs. |
| `lazy_seq_reach_iff` | For `0 < K`: a state is reachable within the switch bound **iff** the sequentialised program reaches it. |

## 5b. Lazy sequentialisation: `N` threads, `K` rounds (`PrismTechniques/LazySeqN.lean`)

This is the schedule the `conc` stage runs (`src/prism/conc/lazy.cpp`,
[CONCURRENCY.md](CONCURRENCY.md)): `K` rounds in which threads `0, 1, …, N-1`
each get one slot, then one final slot for the harness `T0`.
`tests/test_proofs_float_conc.py` locks `prismSched` to the loops in
`lazy.cpp` and to its `context_switch_bound = rounds * N`.

**Model (sequential consistency).**

- A thread `t` has a local state `L` (its `pc` and locals) and takes atomic
  steps `S t l g l' g'` on the shared state `G`. A step is any relation, so
  nondeterminism, blocking `assume`s (locks, joins), branches and loops are
  all allowed. Straight-line code, `lazy.cpp`'s unrolled thread DAG (with the
  node label as `pc`) and code with unbounded loops are all instances.
- `Step`/`Star`: the interleaving semantics. Either the current thread steps,
  or control switches to any thread `t < N` (counted).
- `Slots π`: the sequentialised program for a slot pattern `π`. In slot `t`,
  thread `t` resumes from its saved local state, runs zero or more steps (up
  to a nondeterministic context-switch point) and saves its state.
- `prismSched N K = rr N K ++ [0]`, with
  `rr N K = (List.replicate K (List.range N)).flatten`.

| Theorem | Statement |
|---|---|
| `slots_of_star`, `star_of_slots` | Normal form. An interleaving with `sw` switches is exactly a run of `Slots σ` for its schedule `σ` (the thread of each of its `sw + 1` segments). |
| `slots_mono` | `σ` a subsequence of `π` → every run along `σ` is a run along `π` (the extra slots run zero steps). |
| `length_prismSched` | `(prismSched N K).length = K * N + 1`, so `K·N` switches. |
| `lazy_sound` | `0 < N → Slots S (prismSched N K) ls g ls' g' → ∃ c, Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, K * N⟩`. Every run of the sequentialised program is a real SC interleaving: no spurious counterexample. |
| `rr_covers_runs`, `lazy_covers_runs` | An interleaving whose schedule splits into at most `K` strictly increasing runs of thread ids `< N` is covered. This is the exact shape `K` round-robin rounds admit. |
| `lazy_covers` | `0 < N → Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, sw⟩ → sw + 1 ≤ K → Slots S (prismSched N K) ls g ls' g'`. Every interleaving from `T0` with at most `K − 1` switches **in total** is covered, for any `N`. |
| `lazy_covers_two` | `N = 2` (harness and one thread): every interleaving with at most `2K` switches is covered. That is the full `K·N` bound. |
| `lazy_between` | Both directions in one statement: the reachable states of the sequentialised program lie between the interleavings with `≤ K − 1` switches and those with `≤ K·N` switches. |
| `per_thread_bound_not_enough` | The limit. With `N = 3`, `K = 1`, the schedule `0, 2, 1` (every thread runs once and no thread is preempted) reaches a state that no run of `prismSched 3 1 = [0, 1, 2, 0]` reaches. A bound on context switches **per thread** does not imply coverage for `N ≥ 3`; the schedule must fit the round-robin shape. |

**What is and is not covered.**

- Covered: any number of threads and rounds, threads with any control flow
  (in particular bounded loops) under SC, the harness's final slot, and both
  directions (coverage and soundness).
- Not covered by `K` rounds: schedules with more than `K` round-robin
  "wrap-arounds" (e.g. `T2` before `T1` in every round). A `BOUNDED` verdict
  is only about schedules that fit the pattern, and no finite `K` covers
  every schedule (unbounded rounds are not claimed).
- Not modelled: relaxed or weak memory (non-SC orders are `NEEDS-HARNESS` in
  the stage), and the Z3 encoding of the slots in `lazy.cpp` (the `pc`/`cs`
  formula, the static-locals encoding, the race, deadlock and unlock
  monitors). What is proved is the scheduling argument that the encoding
  relies on. Loop unrolling inside a thread cuts paths past `unwind`; that is
  the same bounded unwinding as BMC and is not part of this proof.

## 6. Floating point: the rounding kernel (`PrismTechniques/FloatRound.lean`)

This file holds the kernel only. The IEEE formats, correctly rounded
`+ - × ÷` with special values and flags, and the proofs about PRISM's
floating-point checks build on it in `proofs/refinement/PrismRefine/Float.lean`
and `FloatOps.lean` ([PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md), "Floating
point"). A Flocq-scale library (all rounding modes, real-valued semantics of
`sqrt`, `fma`, libm) is still out of reach.

**What is proved.** The shared kernel of every IEEE operation:
round-to-nearest, ties-to-even, of a magnitude `n` to a multiple of the ulp
`P`.

```lean
def rne (n P : Nat) : Nat :=
  if 2 * (n % P) > P ∨ (2 * (n % P) = P ∧ (n / P) % 2 = 1) then n / P + 1 else n / P
```

| Theorem | Statement |
|---|---|
| `rne_exact` | `0 < P → n % P = 0 → rne n P * P = n` |
| `rne_half_ulp` | `0 < P → 2 * dist (rne n P * P) n ≤ P` |
| `rne_nearest` | `0 < P → ∀ m, dist (rne n P * P) n ≤ dist (m * P) n` (the rounding is correct) |
| `rne_tie_even` | `2 * (n % P) = P → rne n P % 2 = 0` |
| `binary16_sig_bound` | `n < 2^(11+k) → rne n (2^k) ≤ 2^11` (the binary16 significand, including the carry-out case) |

---

## Gap to the C++ implementation (what these proofs do and do not cover)

These are proofs of **algorithms and designs** over abstract models. The C++
engine (`src/prism/`) is **not** extracted from them, and nothing here proves
that the C++ code implements them. Closing each gap would take the work below.

### k-induction

`src/prism/bmc.cpp:k_induction` builds a base BMC query (`bmc_function` at
`unwind`) and step queries for `kstep ∈ {1, 2}` (`k_step_body` repeats the
loop body). The theorem needs three facts that were not established here.

1. The step query must start from a state in which **every loop-modified
   variable is unconstrained** (`HavocQuerySat`). In the C++ code the step
   body is re-analysed as a fresh function body, so variables declared before
   the loop must become free symbols. That has to be checked in the encoder.
2. The base case must cover at least `kstep` iterations: `Base I T P k` with
   `k ≤ unwind`.
3. The encoded `T` and `P` must match the program semantics. That is the
   encoder-soundness theorem of roadmap 5.3, owned by `proofs/semantics/`.

If the C++ step query asserts `P` in every copy instead of assuming it in the
first `k`, the check is stronger and still sound.

### Houdini

There is no Houdini implementation in the C++ engine yet (roadmap 4.2). The
Lean `houdini` function is the reference.

- An implementation must match `loop`/`round` exactly.
- It may drop candidates on any non-`true` solver answer.
- It must not keep a candidate unless the solver returned UNSAT for its
  preservation query (`PresSound`).

A differential test against `#eval houdini …` on small candidate sets would
tie the two together.

### Contracts and `PROVED-ASSUMING`

The model is first-order and non-recursive, with UB as `Option.none`.

- Recursive functions would need a termination measure, or a partial-
  correctness semantics with fixpoint induction.
- Pointer and heap effects need the memory model from roadmap 2.5 and frame
  conditions in `WP`.
- The report must list, for every `PROVED-ASSUMING` verdict, the assumptions
  it depends on (`proved_assuming_is_implication`). Upgrading such a verdict
  to `PROVED` requires a proof of the assumptions at every call site
  (`discharge`, `harness_assumption_discharged`).

### Bit-blaster

Closed for the certified path by **extraction** (option 1 of the earlier
plan): PRISM's certified mode runs the compiled `toCNF` (`prism-bitblast`)
and Lean's verified LRAT checker (`prism-lrat-check --dag`) next to cake_lpr
(`src/prism/solver/leanbb.cpp`, `portfolio.cpp`; `docs/TRUSTED_BASE.md`).
What remains between the proofs and a `PROVED-CERTIFIED` verdict:

- **The Z3 → S-expression serializer** (`to_lean_dag`, C++). It is small, it
  maps each Z3 operator to one constructor (a few by definition: `bvuge x y`
  is `ule y x`, `=>` is `or (not a) b`, rotations are `concat` of
  `extract`s), and it is tested on random assignments against Z3's evaluator
  and against `Dag.eval` itself (`prism-bitblast --eval`) in
  `tests/cpp/test_leanbb.cpp`. Formulas using anything else (`bvsmod`,
  arrays, UF, …) fall back to Z3's tactics and say so.
- **The formula itself**: the PIR encoder and property instrumentation that
  produce the Z3 VC (roadmap 5.3; `proofs/semantics` models them, nothing is
  extracted).
- **The Lean compiler** that compiles `toCNF`, `checkDag` and the S-expression
  parser, and the parser (`BitblastSexp.lean`, unproved). A parser bug can
  only change *which* formula is checked; the round-trip test compares the
  parsed formula's `Dag.eval` with Z3.
### Lazy sequentialisation

The C++ `conc` stage (roadmap 2.6, [CONCURRENCY.md](CONCURRENCY.md)) is not
extracted from this model. `LazySeqN.lean` (§5b) proves its schedule — `N`
threads, `K` rounds, the harness's final slot, threads with any control flow —
sound and covering under SC. Still outside the proof:

- the Z3 formula `lazy.cpp` builds for a slot (`reach`/`exec`/`pc'` over the
  unrolled DAG) and the static-locals encoding;
- context-switch points only before visible operations (partial-order
  reduction);
- the data-race, deadlock and unlock checks as properties;
- relaxed memory.

### Floating point

This project proves the rounding kernel. `proofs/refinement`
(`Float.lean`, `FloatOps.lean`, [PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md))
builds on it: correctly rounded `+ - × ÷` for any binary format with the IEEE
special values, the exception flags, and the exact equivalence of PRISM's
FLOAT-CAST-OVF / FLOAT-OVERFLOW / FLOAT-INVALID / FLOAT-DIV-ZERO conditions
with IEEE 754 and C11 (with the two stated differences). Z3's floating-point
theory itself stays trusted (it is assumed to implement IEEE 754).
