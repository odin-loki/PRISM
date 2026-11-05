# PRISM verification techniques, proved in Lean 4

The Lean project `proofs/techniques/` proves that the *algorithms and
designs* behind several PRISM verdicts are sound: k-induction, the Houdini
invariant filter with the pir stage's loop-cut encoding and its sequential
`assume` guard, contracts and `PROVED-ASSUMING`, the bit-blaster that
certified mode runs, lazy sequentialisation (the `conc` stage's schedule)
and the floating-point rounding kernel. The IEEE operations, exception flags
and PRISM's floating-point checks build on that kernel in
`proofs/refinement` ([PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md)).

It is a separate Lake project from `proofs/` (verdict lattice) and
`proofs/semantics/` (PIR semantics), with no dependencies: core Lean 4 and
its bundled `Std` only, no Mathlib.

| | |
|---|---|
| Toolchain | `leanprover/lean4:v4.34.0` (`proofs/techniques/lean-toolchain`) |
| Build | `cd proofs/techniques && lake build` |
| Axiom audit | `lake env lean PrismTechniques/Audit.lean` (also part of `lake build`) |
| End-to-end certified mode | `lake exe certified-demo "$(dirname "$(elan which lean)")/cadical"` |
| Proved bit-blaster as a program | `lake exe prism-bitblast < formula.sexp` (DIMACS + variable map) |
| Lean's verified LRAT checker as a program | `lake exe prism-lrat-check [--dag formula.sexp] query.cnf proof.lrat` |
| CI | `.github/workflows/proofs-techniques.yml`; independent recheck in `.github/workflows/proofs-recheck.yml` |

## Trust statement

- **No unfinished proofs, no new axioms.** No file contains `sorry`, `admit`
  or `native_decide`, and no file declares an `axiom`; CI greps for all of
  them.
- **The audit is enforced.** `PrismTechniques/Audit.lean` defines
  `#assert_axioms`, applied to 91 theorems. It collects every axiom a
  theorem depends on, transitively, and fails the build on anything other
  than `propext`, `Classical.choice` and `Quot.sound`. That catches
  `sorryAx`, and also `Lean.ofReduceBool`, the axiom `native_decide` and
  `bv_decide` add to trust compiled code.
- **The trusted base** for every result below is the Lean 4 kernel plus the
  three standard axioms. For `certified_unsat` it also includes the proofs
  inside core Lean's `Std` (the LRAT checker and its soundness proof), which
  the same kernel checks.

Every theorem name below is in the namespace `PrismTechniques`.

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
| `step_iff_havoc_split` | For `S = F × L` (unmodified part × loop-modified part): havocking the loop state is exactly what the step premise quantifies over. |
| `step_one_iff` | `Step T P 1 ↔ ∀ s s', P s → T s s' → P s'`: 1-induction is an ordinary inductive invariant. |
| `step_mono` | `Step T P k → Step T P (k+1)`. |
| `kinduction_strengthened` | `Base I T (P ∧ J) k → Step T (P ∧ J) k → ∀ s, Reach I T s → P s`. |
| `kinduction_rel_sound` | `(∀ s, Reach I T s → J s) → Base I T P k → StepRel T J P k → ∀ s, Reach I T s → P s`. Here `StepRel` may assume `J` on all `k+1` window states. |
| `kinduction_frame_sound` | `(∀ s, I s → J s) → (∀ s s', J s → P s → T s s' → J s') → Base I T P k → StepFrame T J P k → ∀ s, Reach I T s → P s`, where `StepFrame` havocs the first window state only up to `J`. This is the pir stage's step for memory-writing loops (`docs/PIR.md`, "k-induction with memory"): `J` = memory equals the prefix's outside the loop's write footprint, same object table. |
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
| `houdini_maximal` | With complete oracles (`InitComplete`, `PresComplete`), every `A ⊆ cands` with `Inductive O I T A` satisfies `A ⊆ houdini …`: the result is the largest inductive subset. |
| `houdini_then_kinduction` | The survivors feed `kinduction_rel_sound`: `Base I T P k → StepRel T (Conj O survivors) P k → ∀ s, Reach I T s → P s` |

## 2b. The pir loop cut and the sequential `assume` guard (`PrismTechniques/LoopCut.lean`)

The pir stage proves loops with invariants (`docs/PIR.md`, "Loop
invariants"; `src/prism/pir/houdini.inc`): it encodes the function with
**every loop cut once**, havocs each loop's header state, assumes the loop's
surviving candidates there, and checks that they hold at the header's entry
(*base*) and at every back edge (*step*); the function is PROVED-UNBOUNDED
when, with all survivors assumed, no property of the cut program is
violated. `LoopCut.lean` proves that argument, together with the encoder's
per-statement `assume` guard (soundness fix S9, `docs/CONFORMANCE.md`).

**Programs.** Structured programs over an abstract state `S`: blocks of
statements (`upd R` — an assignment or a havoc, any relation; `check c`;
`assume c`), sequencing, branches, and loops `loop L body` with any number of
exits (`brk`) and latches (falling off the body, `cont`), plus `ret`. Loops
nest and follow each other freely; invariants `Inv L e s` (entry state `e`,
header state `s`) and havoc relations `Hav L e s` are per loop. The
*sequential semantics* `Exec` is the uncut program: a failed check ends the
run with `err`, a false `assume` blocks it.

**Part 1 — an `assume` constrains only later statements.** `encode.cpp`
gives each statement of a block the guard `r` = the node's reach,
strengthened by every `assume` already passed; a check's instance is
`r ∧ ¬cond`; the block's exit guard (`exit_reach`) is the final `r`.

| Theorem | Statement |
|---|---|
| `encViol_iff` | Some property instance of a block is violated under its own running guard **iff** the block, run sequentially, fails a check |
| `encExit_iff` | The exit guard holds with no violated instance, ending in `t`, **iff** the block runs sequentially to `t` |
| `old_viol_imp` | The pre-fix encoding (every `assume` an axiom of the whole node) only lost alarms: each alarm it gave, the fixed encoding gives |
| `s9_old_encoding_misses` | The program `100 / x; assume(x != 0)` with `x = 0`: the sequential semantics and the fixed encoding report the division, the old encoding reports nothing |

**Part 2 — the loop cut.** `CExec` is the cut program run sequentially: at
the header of `L` with entry state `e`, the base goal `Inv L e e`, then the
havoc to any `s` with `Hav L e s` and the assumption `Inv L e s`, then the
body once; a latch is a cut whose step goal is `Inv L e t`. `GRun` is the Z3
encoding as a path semantics: a path does not stop at a violated instance
(`v` records one), a base or step goal that fails where no property was
violated before sets `q`, and a header's invariant assumption is active only
while neither flag is set — the guard `reach(header) ∧ noviol(props_before)`
of `houdini.inc`, with each query assuming only the loops before its point.
"Every query UNSAT at the fixpoint" is the premise of:

| Theorem | Statement |
|---|---|
| `exec_cut` | Every execution of the uncut program is matched by a run of the cut program with the same outcome, or the cut program fails (a property, a base or a step goal). Needs `FrameOK Hav P` |
| `cexec_grun` | A failing run of the cut program is a complete encoding path with `v ∨ q`; any other run is an unflagged path with the same outcome (uses `grun_complete`) |
| `grun_complete` | After a violation or a failed goal the encoding path can always be completed: nothing after it is assumed |
| **`loopcut_sound`** | `FrameOK Hav P → Total P → (∀ o v q, GRun Inv Hav P s0 False False o v q → ¬v ∧ ¬q) → ¬ Exec P s0 .err`: if no complete path of the cut encoding violates a property or fails a base/step goal, **no execution of the uncut program**, through any number of iterations of any of its (nested, sequential, multi-exit) loops, violates a property |
| `loopcut_sound_seq` | The same from the sequential cut program alone (`¬ CExec … .err`), no totality needed |
| `houdini_loopcut_sound` | `loopcut_sound` with `Inv` the conjunction of per-loop survivor lists (`survivorsInv`) |

Premises: `FrameOK` — each loop's havoc covers its header states (for PIR:
the header phis are havocked and every byte a violation-free iteration
writes is in the loop's footprint; the loop allocates and frees nothing);
`Total` — every `upd` is total (a Z3 assignment defines its value; a havoc is
a fresh constant).

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
| `harness_assumption_discharged` | A function proved `PROVED-ASSUMING H`, called only at call sites that prove `H` (`WP` with `req := H`), gives `∃ r, run impl p = some r ∧ Q r`. |
| `caller_safe` | Worked example: a guarded division caller. |

## 4. Bit-blaster (`PrismTechniques/Bitblast*.lean`)

Files: `Bitblast.lean` (bit-level facts, gates, Tseitin clauses, the
builder specification), `BitblastOps.lean` (operator circuits),
`BitblastDiv.lean` (divider), `BitblastEncode.lean` (expression language,
encoder, `toCNF`, the main theorems, sharing), `BitblastSexp.lean` (the text
format the executables read; not part of any proof).

**Fragment** (everything the PIR verification conditions use).
`BVExpr : Nat → Type`, widths per variable:

| Group | Constructors | Circuit |
|---|---|---|
| Leaves | `var base` (a block of input bits), `const` | wires |
| Bitwise | `not`, `and`, `or`, `xor` | one gate per bit |
| Arithmetic | `add`, `sub`, `neg`, `mul` | ripple-carry adder; `x + ~~~y + 1`; `0 - x`; shift-and-add |
| Division | `udiv`, `urem`, `sdiv`, `srem` | restoring divider, SMT-LIB division by zero, SMT-LIB sign cases |
| Shifts | `shl`, `lshr`, `ashr` by a variable; `shlC`, `lshrC`, `ashrC` by a constant | barrel shifters; wires |
| Width | `zext n`, `sext n`, `extract lo len`, `concat` | wires |
| Choice | `ite` (1-bit condition) | multiplexer |
| Predicates (1 bit) | `eq`, `ult`, `ule`, `slt`, `sle` | xor + and-chain; carry chains |
| Overflow (1 bit) | `uaddo`, `saddo`, `usubo`, `ssubo`, `umulo`, `smulHi`, `smulLo` | carry out; sign tests; double-width products |

Semantics (`BVExpr.denote ρ`) is core Lean's `BitVec` operation for every
constructor, with two exceptions: division is SMT-LIB's (`smtUdiv x 0 =
allOnes`, `x % 0 = x`, `smtSdiv`/`smtSrem` by the standard's four sign
cases), because Z3 and the PIR encoder use SMT-LIB semantics; and signed
multiplication overflow is split into `smulHi` and `smulLo`, because Z3's
`bvmul_no_overflow` / `bvmul_no_underflow` are separate predicates
(`smulOverflow_hi_lo` proves `BitVec.smulOverflow = smulHi || smulLo`).
`FSat φ := ∃ ρ, φ.denote ρ = 1#1`.

**Encoding.** The encoder builds an and/or/xor circuit and Tseitin-encodes
each gate into `Std.Sat.CNF Nat`, the CNF type the verified LRAT checker
takes. Input bit `j` is variable `2j`, variable `1` is the constant `true`,
and gate `k` is variable `2k+3`. `toCNF φ` contains every gate's defining
clauses, the unit clause `[1]`, and a unit clause asserting the output bit.
The circuit caches its gate count (`Circuit.len`, invariant `Circuit.WF`)
and the clauses are emitted by a tail-recursive loop (`clausesAcc`, proved
equal to the specification `clausesL`), so the compiled encoder is linear in
the number of gates.

**Proof structure.** `Consistent α c` means that `α` satisfies every gate
definition; `clausesL_all_iff` proves the clauses hold exactly when it does.
Every circuit builder is proved against `Rep c ls X` ("under every
assignment consistent with `c`, the literals `ls` are the bits of `X α`")
and `Spec`. `extend ρ c` extends any input assignment to a consistent one
by evaluating the gates in order (`extendL_consistent`, `extendL_input`).
The arithmetic facts are core lemmas from `Init.Data.BitVec.Bitblast` and
`Init.Data.BitVec.Lemmas`; the circuits, the Tseitin clauses, the builder
specification and the equisatisfiability and sharing proofs are PRISM's own.

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

The per-operator circuit lemmas (`subOp_spec`, `udivOp_spec`,
`smulHiOp_spec`, …) are in the audit list.

**Sharing (`Dag`).** Z3 terms are DAGs; writing one as a tree can be
exponentially larger. A `Dag` is a list of definitions `(w, base, e)` ("input
bits `base … base+w-1` name the value of `e`") and a top formula. Its meaning,
`Dag.eval ρ`, evaluates the definitions in order and then the top formula.
`defsOK` (checked at run time by both executables) requires each definition
to read only bits below its own base and the definitions to be laid out
upwards; `BVExpr.denote_congr` (an expression depends only on the bits below
`reads`) then gives `dag_sat_imp`: a satisfying input of the DAG extends to
one of the flat formula `(v₁ = e₁) ∧ … ∧ (vₙ = eₙ) ∧ top`.

**Executables.** Both are `lean_exe` targets of this project, built by
`lake build` (and in CI):

- `prism-bitblast` reads a formula in the S-expression format of
  `BitblastSexp.lean` on stdin (`(dag (def W BASE e)* e)`) and writes
  `c prism-bitblast 1`, one `c var BASE WIDTH V0 … V(W-1)` line per free
  input variable, and then `Std.Sat.CNF.dimacs (dagCNF g)`: exactly `toCNF`
  of the flattened DAG. The executed code is the proved code, modulo the
  Lean compiler. `CNF.dimacs` numbers CNF variable `v` as DIMACS variable
  `v+1`, so input bit `j` is DIMACS variable `2j+1`. `--eval` instead
  evaluates `Dag.eval` under `(rho (BASE WIDTH VALUE)*)` assignments; PRISM's
  C++ serializer is tested against it. (The compiled `denote` is
  `denoteExec` through the proved `@[csimp]` lemma `denote_eq_denoteExec`.)
- `prism-lrat-check CNF LRAT` parses DIMACS (strictly) and runs
  `Std.Tactic.BVDecide.LRAT.check`, the checker `certified_unsat` relies on.
  `prism-lrat-check --dag FORMULA CNF LRAT` rebuilds the CNF from the formula
  with the proved bit-blaster, insists that the CNF file is byte for byte
  that CNF, and runs `checkDag g cert` — the exact premise of
  `checkDag_sound`, with no DIMACS parser in between. Both print
  `s VERIFIED UNSAT` (exit 0) or `s NOT VERIFIED` (exit 1).

PRISM's certified mode uses both (`src/prism/solver/leanbb.cpp`;
`docs/TRUSTED_BASE.md`).

**End to end.** `lake exe certified-demo <cadical>` bit-blasts concrete
formulas (one or more per operator group) with `toCNF`, writes DIMACS, runs
CaDiCaL (shipped inside the Lean toolchain) with `--lrat`, parses the
certificate with `Std`'s LRAT parser and runs the verified checker. CI runs
it, plus `prism-bitblast` → CaDiCaL → `prism-lrat-check` in both modes and a
tampered proof that must be rejected. This is an *executed* check of
`certified_unsat`'s premise, not a kernel proof.

## 5. Lazy sequentialisation, two threads (`PrismTechniques/LazySeq.lean`)

A small model: two threads, each a straight-line list of atomic actions
`G → G → Prop` over a shared state `G`; the real interleaving semantics
`Step`/`Star` counts context switches; `SeqLR K` is the Lal–Reps round-robin
guess-and-check reduction (thread 1 runs all `K` rounds first on
round-indexed copies of the shared state, each round after the first from a
guessed state; thread 2 then runs its `K` rounds; a run is kept only if each
guess matches).

| Theorem | Statement |
|---|---|
| `lazy_seq_covers` | Every interleaving with at most `2K−1` switches, finished or not, is covered by `SeqLR K`. |
| `lazy_seq_sound` | `0 < K →` every run of `SeqLR K` is a real interleaving with at most `2K−1` switches: no spurious runs. |
| `lazy_seq_reach_iff` | For `0 < K`: a state is reachable within the switch bound **iff** the sequentialised program reaches it. |

## 5b. Lazy sequentialisation: `N` threads, `K` rounds (`PrismTechniques/LazySeqN.lean`)

This is the schedule the `conc` stage runs (`src/prism/conc/lazy.cpp`,
[CONCURRENCY.md](CONCURRENCY.md)): `K` rounds in which threads `0, 1, …, N-1`
each get one slot, then one final slot for the harness `T0`.
`tests/cpp/test_repolint.cpp` locks `prismSched` to the loops in `lazy.cpp`
and to its `context_switch_bound = rounds * N`.

**Model (sequential consistency).** A thread `t` has a local state `L` and
takes atomic steps `S t l g l' g'` on the shared state `G`; a step is any
relation, so nondeterminism, blocking `assume`s (locks, joins), branches and
loops are all allowed. `Step`/`Star` is the interleaving semantics (the
current thread steps, or control switches to any thread `t < N`, counted).
`Slots π` is the sequentialised program for a slot pattern `π`.
`prismSched N K = rr N K ++ [0]`, with
`rr N K = (List.replicate K (List.range N)).flatten`.

| Theorem | Statement |
|---|---|
| `slots_of_star`, `star_of_slots` | Normal form: an interleaving with `sw` switches is exactly a run of `Slots σ` for its schedule `σ`. |
| `slots_mono` | `σ` a subsequence of `π` → every run along `σ` is a run along `π`. |
| `length_prismSched` | `(prismSched N K).length = K * N + 1`, so `K·N` switches. |
| `lazy_sound` | `0 < N →` every run of the sequentialised program is a real SC interleaving: no spurious counterexample. |
| `rr_covers_runs`, `lazy_covers_runs` | An interleaving whose schedule splits into at most `K` strictly increasing runs of thread ids `< N` is covered. |
| `lazy_covers` | `0 < N →` every interleaving from `T0` with at most `K − 1` switches **in total** is covered, for any `N`. |
| `lazy_covers_two` | `N = 2`: every interleaving with at most `2K` switches is covered (the full `K·N` bound). |
| `lazy_between` | The reachable states of the sequentialised program lie between the interleavings with `≤ K − 1` switches and those with `≤ K·N` switches. |
| `per_thread_bound_not_enough` | With `N = 3`, `K = 1`, the schedule `0, 2, 1` reaches a state that no run of `prismSched 3 1` reaches: a bound on switches **per thread** does not imply coverage for `N ≥ 3`. |

A `BOUNDED` verdict from the `conc` stage is only about schedules of
round-robin shape; no finite `K` covers every schedule.

## 6. Floating point: the rounding kernel (`PrismTechniques/FloatRound.lean`)

Round-to-nearest, ties-to-even, of a magnitude `n` to a multiple of the ulp
`P`, the shared kernel of every IEEE operation:

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

The IEEE formats and operations are in `proofs/refinement/PrismRefine/Float.lean`
and `FloatOps.lean`. Other rounding modes, `sqrt`, `fma` and libm are not
modelled.

## Gap to the C++ implementation (what these proofs do and do not cover)

These are proofs of **algorithms and designs** over abstract models. The C++
engine is **not** extracted from them, and nothing here proves that the C++
code implements them, except the bit-blaster, whose proved code runs as
`prism-bitblast`.

### k-induction

`kinduction_step` in `src/prism/pir/encode.cpp` answers the step for a
single loop: from an arbitrary header state (header phis and the bytes of
the loop's write footprint havocked), `k` violation-free iterations that
loop back imply that iteration `k` is violation-free. The theorems need, and
do not prove of the C++:

1. The step query starts from a state in which every loop-modified value is
   unconstrained (`HavocQuerySat`), or, for memory, unconstrained up to the
   frame `J` (`kinduction_frame_sound`). `J`'s preservation by the loop body
   rests on the write-footprint analysis, which is not proved (it needs a
   frame lemma over `PrismSem/Memory.lean`, which has no initialised flags).
2. The base case covers at least `k` iterations (`Base I T P k` with
   `k ≤ unwind`).
3. The encoded `T` and `P` match the program semantics: the encoder gap of
   [PROOFS_SEMANTICS.md](PROOFS_SEMANTICS.md#the-gap-between-this-model-and-the-c-encoder).

### Houdini and the loop cut

`src/prism/pir/houdini.inc` keeps a candidate only when every query that
checks it came back UNSAT with the final survivor set assumed
(`PresSound`/`InitSound`), and drops it on SAT, a timeout or "unknown".
Modelled, not proved of the C++:

- The encoder's DAG and Z3 formula are represented by the path semantics
  `GRun`; PIR is a CFG, the model a structured program with multi-exit,
  multi-latch loops. A `goto` into a loop body or other irreducible control
  flow is outside the model.
- `FrameOK` rests on the provenance analysis of `SymMem`, not proved here
  (the same gap as `kinduction_frame_sound`).
- That each Z3 query is exactly the `GRun` condition for its point is the
  encoder correspondence; `tests/cpp/test_repolint.cpp` locks the three
  encoder facts the model relies on (the running `assume` guard and
  `exit_reach`, the `noviol(props_before)` guard of the havoc assumptions,
  the order of the queries' assumed loops) to the C++ text.

### Contracts and `PROVED-ASSUMING`

The model is first-order and non-recursive, with UB as `Option.none`.
Recursive functions would need a termination measure or a
partial-correctness semantics; pointer and heap effects need a memory model
and frame conditions in `WP`. The report lists the assumptions of every
`PROVED-ASSUMING` verdict (`proved_assuming_is_implication`); upgrading
such a verdict to `PROVED` requires a proof of the assumptions at every
call site (`discharge`, `harness_assumption_discharged`).

### Bit-blaster

The certified path runs the compiled `toCNF` (`prism-bitblast`) and Lean's
verified LRAT checker (`prism-lrat-check --dag`) next to cake_lpr
(`src/prism/solver/leanbb.cpp`, `portfolio.cpp`). Between the proofs and a
`PROVED-CERTIFIED` verdict remain:

- **The Z3 → S-expression serializer** (`to_lean_dag`, C++). It maps each Z3
  operator to one constructor (a few by definition) and is tested on random
  assignments against Z3's evaluator and against `Dag.eval` itself
  (`prism-bitblast --eval`) in `tests/cpp/test_leanbb.cpp`. Formulas using
  anything else (`bvsmod`, arrays, UF, …) fall back to Z3's tactics and say
  so.
- **The formula itself**: the PIR encoder and property instrumentation that
  produce the VC (`proofs/semantics` models them; nothing is extracted).
- **The Lean compiler** that compiles `toCNF`, `checkDag` and the
  S-expression parser, and the parser (`BitblastSexp.lean`, unproved). A
  parser bug can only change *which* formula is checked.

### Lazy sequentialisation

`LazySeqN.lean` proves the `conc` stage's schedule sound and covering under
SC. Outside the proof: the Z3 formula `lazy.cpp` builds for a slot and the
static-locals encoding; context-switch points only before visible
operations; the data-race, deadlock and unlock checks as properties; relaxed
memory (non-SC orders are `NEEDS-HARNESS` in the stage); loop unrolling
inside a thread (the same bounded unwinding as the rest of the pir stage).

### Floating point

This project proves the rounding kernel; `proofs/refinement` builds the
IEEE operations and PRISM's floating-point checks on it. Z3's floating-point
theory stays trusted.
