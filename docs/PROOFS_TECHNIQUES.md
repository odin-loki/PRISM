# PRISM verification techniques, proved in Lean 4

This document covers the Lean project `proofs/techniques/`. It proves that the
*algorithms and designs* behind several PRISM verdicts are sound. These are
roadmap 5.4 (bit-blaster) and the 8.2 rows "k-induction", "Houdini invariant
filter", "Contracts and PROVED-ASSUMING", "Bit-blaster", "Concurrency (lazy
sequentialisation)" and, in part, "Floating point".

It is a separate Lake project from `proofs/` (verdict lattice) and
`proofs/semantics/` (PIR semantics). It has its own toolchain and no
dependencies: core Lean 4 and its bundled `Std` only, no Mathlib.

| | |
|---|---|
| Toolchain | `leanprover/lean4:v4.34.0` (`proofs/techniques/lean-toolchain`) |
| Build | `cd proofs/techniques && lake build` |
| Axiom audit | `lake env lean PrismTechniques/Audit.lean` (also runs as part of `lake build`) |
| End-to-end certified mode | `lake exe certified-demo "$(dirname "$(elan which lean)")/cadical"` |
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
LazySeq.lazy_seq_covers                    [propext, Quot.sound]
LazySeq.lazy_seq_sound                     [propext, Quot.sound]
LazySeq.lazy_seq_reach_iff                 [propext, Quot.sound]
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

## 4. Bit-blaster (`PrismTechniques/Bitblast.lean`, `BitblastEncode.lean`)

**Fragment.** `BVExpr : Nat → Type` has the constructors `var` (a block of
input bits), `const`, `not`, `and`, `or`, `xor`, `add` (ripple-carry), `mul`
(shift-and-add), `ite` (with a 1-bit condition), and the 1-bit results `eq`,
`ult` and `slt`. Its semantics, `BVExpr.denote ρ`, is core Lean's `BitVec`.
`FSat φ := ∃ ρ, φ.denote ρ = 1#1`.

**Encoding.**

- The encoder builds an and/or/xor circuit.
- Each gate is Tseitin-encoded into `Std.Sat.CNF Nat`, the CNF type the
  verified LRAT checker takes.
- Input bit `j` is variable `2j`, variable `1` is the constant `true`, and
  gate `k` is variable `2k+3`.
- `toCNF φ` contains every gate's defining clauses, the unit clause `[1]`,
  and a unit clause asserting the output bit.

**Proof structure.**

- `Consistent α gs` means that `α` satisfies every gate definition.
  `clauses_all_iff` proves that the clauses hold exactly when `Consistent`
  does.
- `encode_spec`: under every consistent `α`, the encoder's output literals are
  `toBits (e.denote (inputOf α))`.
- `extend ρ gs` extends any input assignment to a consistent one, by
  evaluating the gates in order (`extend_consistent`, `extend_input`). This
  relies on the circuit being well-formed: gates only read earlier variables.

| Theorem | Statement |
|---|---|
| `encode_spec` | `WF gs → Spec gs (encode e gs) (fun α => toBits (e.denote (inputOf α)))` |
| `sat_of_cnf_sat` | `(toCNF φ).Sat α → φ.denote (inputOf α) = 1#1` |
| `cnf_sat_of_sat` | `φ.denote ρ = 1#1 → (toCNF φ).Sat (extend ρ (encode φ []).2)` |
| `toCNF_equisat` | `(∃ α, (toCNF φ).Sat α) ↔ FSat φ` (equisatisfiable, both directions) |
| `toCNF_unsat_imp` | `(toCNF φ).Unsat → ∀ ρ, φ.denote ρ ≠ 1#1` (the direction certified mode needs) |
| `certified_unsat` | `Std.Tactic.BVDecide.LRAT.check cert (toCNF φ) = true → ∀ ρ, φ.denote ρ ≠ 1#1` |

**Reuse of core Lean's `bv_decide` development.**

- The arithmetic facts are core lemmas:
  - the adder: `BitVec.carry`, `BitVec.carry_succ`, `BitVec.getLsbD_add`;
  - unsigned comparison: `BitVec.ult_eq_not_carry`;
  - signed comparison: `BitVec.slt_eq_ult` and
    `BitVec.msb_eq_getLsbD_last`;
  - the multiplier: `BitVec.mulRec`, `BitVec.mulRec_succ_eq`,
    `BitVec.getLsbD_mul`.
- The certificate step is `Std.Tactic.BVDecide.LRAT.check_sound`.
- The circuit, the Tseitin clauses and the equisatisfiability proof are
  PRISM's own. `bv_decide`'s AIG bit-blaster uses different data structures,
  so its lemmas were not reused directly.

**End to end.** `lake exe certified-demo <cadical>` runs the full certified
path on concrete formulas:

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

## 6. Floating point (stretch, small piece) (`PrismTechniques/FloatRound.lean`)

**Why the full row is out of reach.** It would need an IEEE 754 library on
the scale of Flocq. That means formats with exponent ranges, subnormals,
overflow, signed zeros, NaNs and all rounding modes, plus the real-valued
semantics of every operation. Core Lean has none of this, and building it is
research-scale.

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

The C++ engine sends bitvector queries to Z3; there is no C++ bit-blaster or
LRAT path yet (roadmap 3.2). There are two ways to make a certified verdict
rest on these proofs:

1. **Extract.** Compile `toCNF` from Lean, via `lake build` of a
   `lean_exe`/shared library, and call it from C++. Then the CNF that
   CaDiCaL/Kissat solve is produced by the proved code, and `certified-demo`
   is already the prototype of this path.
2. **Validate per run.** Keep a C++ bit-blaster, but re-run the Lean `toCNF`
   and require the two CNFs to be identical before a certificate counts.

In both cases the formula handed over must be the one produced by the proved
encoder (roadmap 5.3). Two more restrictions apply:

- The fragment has no shifts by variable amounts, `udiv`/`urem`, extract or
  concat, and no arrays.
- `var` names a block of input bits, and the front end must give different
  program variables disjoint blocks.

### Lazy sequentialisation

There is no concurrency stage in the C++ engine yet (roadmap 2.6). The Lean
result justifies the eager round-robin reduction for two threads with
straight-line code. Four extensions are needed before it covers the planned
stage:

- per-thread loops, which need a bounded unrolling argument;
- more than two threads, which generalises `Seg` to a round-robin over `n`
  threads;
- Lazy-CSeq's lazy re-execution;
- data-race detection as a property.

### Floating point

Only the rounding kernel is proved. Nothing yet connects it to Z3/Bitwuzla's
floating-point theory or to IEEE 754 as a whole. PRISM's floating-point
verdicts therefore stay trusted, not proved, until a Flocq-style library
exists in Lean.
