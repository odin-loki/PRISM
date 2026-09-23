/-
PRISM techniques: function contracts, modular reasoning and the
`PROVED-ASSUMING` verdict (roadmap 8.2, row "Contracts and PROVED-ASSUMING";
roadmap 4.2 "Harness generation").

Model.  A first-order call language over a signature of functions `Fn` with
argument types `Arg f` and result types `Ret f`.  A caller is a `Prog R`:
it returns, reaches undefined behaviour (`ub`), or calls a function and
continues with its result.  An implementation `impl f a : Option (Ret f)`
gives the real behaviour of each function (`none` = UB inside `f`).

A contract gives `requires`/`ensures`.  PRISM proves each function against its
own contract and each caller against the *contracts* of its callees (`WP`),
never looking into callee bodies.

Proved:
* `modular_sound`: all functions satisfy their contracts ∧ the caller's
  verification condition holds ⇒ the whole program runs without UB (in the
  caller and inside every called function) and meets its postcondition;
* `compose_layer`: a function whose body is verified against lower-level
  contracts satisfies its own contract — iterating this covers every acyclic
  call graph;
* `proved_assuming_is_implication`, `proved_assuming_not_proved`,
  `discharge`: a `PROVED-ASSUMING` verdict is only a proof of the implication
  `assumptions → safe`; it becomes `PROVED` exactly when the assumptions are
  discharged, and there are programs for which it cannot be upgraded;
* `contract_violation_breaks_modularity`: the caller's proof alone does not
  imply safety when a callee does not meet its contract.
-/

namespace PrismTechniques.Contracts

/-- Callers: straight-line code with calls (first-order, no recursion). -/
inductive Prog (Fn : Type) (Arg Ret : Fn → Type) (R : Type) : Type
  | ret (r : R)
  | ub
  | call (f : Fn) (a : Arg f) (k : Ret f → Prog Fn Arg Ret R)

variable {Fn : Type} {Arg Ret : Fn → Type}

/-- Concrete execution with the real function implementations.
`none` means undefined behaviour was reached somewhere. -/
def run (impl : (f : Fn) → Arg f → Option (Ret f)) {R : Type} :
    Prog Fn Arg Ret R → Option R
  | .ret r => some r
  | .ub => none
  | .call f a k =>
    match impl f a with
    | none => none
    | some b => run impl (k b)

/-- A contract for function `f`. -/
structure Contract (A B : Type) where
  req : A → Prop
  ens : A → B → Prop

/-- `impl` meets the contract of `f`: on every input satisfying `requires`
it has no UB and its result satisfies `ensures`. -/
def Satisfies (impl : (f : Fn) → Arg f → Option (Ret f))
    (spec : (f : Fn) → Contract (Arg f) (Ret f)) (f : Fn) : Prop :=
  ∀ a, (spec f).req a → ∃ b, impl f a = some b ∧ (spec f).ens a b

/-- The modular verification condition of a caller: every call site proves
the callee's `requires`, and the continuation is verified for *every* result
allowed by the callee's `ensures`; `ub` is never allowed. -/
def WP (spec : (f : Fn) → Contract (Arg f) (Ret f)) {R : Type} :
    Prog Fn Arg Ret R → (R → Prop) → Prop
  | .ret r, Q => Q r
  | .ub, _ => False
  | .call f a k, Q => (spec f).req a ∧ ∀ b, (spec f).ens a b → WP spec (k b) Q

/-- **Modular soundness.**  If every function satisfies its contract and the
caller's verification condition holds, the composed program has no UB (in
the caller or in any called function) and its result satisfies `Q`. -/
theorem modular_sound (impl : (f : Fn) → Arg f → Option (Ret f))
    (spec : (f : Fn) → Contract (Arg f) (Ret f)) (himpl : ∀ f, Satisfies impl spec f)
    {R : Type} (p : Prog Fn Arg Ret R) (Q : R → Prop) (hp : WP spec p Q) :
    ∃ r, run impl p = some r ∧ Q r := by
  induction p with
  | ret r => exact ⟨r, rfl, hp⟩
  | ub => exact hp.elim
  | call f a k ih =>
    obtain ⟨hreq, hk⟩ := hp
    obtain ⟨b, hb, hens⟩ := himpl f a hreq
    obtain ⟨r, hr, hQ⟩ := ih b (hk b hens)
    exact ⟨r, by simp [run, hb, hr], hQ⟩

/-- **Layered composition.**  Functions `g` of a higher layer are implemented
by bodies that call lower-layer functions.  If the lower layer meets its
contracts and each body is verified against them (`WP`) under `g`'s own
`requires`, with `g`'s `ensures` as postcondition, then the higher layer meets
its contracts.  Applying this layer by layer covers any acyclic call graph. -/
theorem compose_layer {Fn₂ : Type} {Arg₂ Ret₂ : Fn₂ → Type}
    (impl : (f : Fn) → Arg f → Option (Ret f))
    (spec : (f : Fn) → Contract (Arg f) (Ret f)) (himpl : ∀ f, Satisfies impl spec f)
    (spec₂ : (g : Fn₂) → Contract (Arg₂ g) (Ret₂ g))
    (body : (g : Fn₂) → Arg₂ g → Prog Fn Arg Ret (Ret₂ g))
    (hbody : ∀ g a, (spec₂ g).req a → WP spec (body g a) ((spec₂ g).ens a)) :
    ∀ g, Satisfies (fun g a => run impl (body g a)) spec₂ g := by
  intro g a hreq
  exact modular_sound impl spec himpl (body g a) _ (hbody g a hreq)

/-- If a callee does *not* meet its contract, a verified caller can still hit
UB: the caller's proof is conditional on the callee contracts. -/
theorem contract_violation_breaks_modularity :
    ∃ (impl : Unit → Nat → Option Nat) (spec : Unit → Contract Nat Nat)
      (p : Prog Unit (fun _ => Nat) (fun _ => Nat) Nat),
      WP spec p (fun _ => True) ∧ run impl p = none := by
  refine ⟨fun _ _ => none, fun _ => ⟨fun _ => True, fun _ _ => True⟩,
    .call () 0 (fun b => .ret b), ?_, rfl⟩
  exact ⟨trivial, fun _ _ => trivial⟩

/-! ### `PROVED-ASSUMING` -/

/-- A verdict `PROVED` for the safety predicate `Safe` over inputs `X`. -/
def Proved {X : Type} (Safe : X → Prop) : Prop := ∀ x, Safe x

/-- A verdict `PROVED-ASSUMING A`: safety was proved only for inputs that
satisfy the listed assumptions (e.g. harness assumptions drafted by the model:
non-null pointers, allocation sizes, value ranges). -/
def ProvedAssuming {X : Type} (A Safe : X → Prop) : Prop := ∀ x, A x → Safe x

/-- `PROVED-ASSUMING` is exactly a proof of the implication, nothing more. -/
theorem proved_assuming_is_implication {X : Type} (A Safe : X → Prop) :
    ProvedAssuming A Safe ↔ Proved (fun x => A x → Safe x) := Iff.rfl

/-- `PROVED-ASSUMING` never upgrades to `PROVED` on its own: integer division
`100 / x` is safe assuming `x ≠ 0` but not safe. -/
theorem proved_assuming_not_proved :
    ∃ (A Safe : Nat → Prop), ProvedAssuming A Safe ∧ ¬ Proved Safe := by
  let impl : Nat → Option Nat := fun x => if x = 0 then none else some (100 / x)
  refine ⟨fun x => x ≠ 0, fun x => impl x ≠ none, ?_, ?_⟩
  · intro x hx; simp [impl, hx]
  · intro h; exact h 0 (by simp [impl])

/-- Discharging the assumptions (proving them at every use) turns
`PROVED-ASSUMING` into `PROVED`, and `PROVED` implies `PROVED-ASSUMING`
anything. -/
theorem discharge {X : Type} (A Safe : X → Prop) (h : ProvedAssuming A Safe)
    (hA : Proved A) : Proved Safe := fun x => h x (hA x)

theorem proved_to_assuming {X : Type} (A Safe : X → Prop) (h : Proved Safe) :
    ProvedAssuming A Safe := fun x _ => h x

/-- A harness assumption is a `requires`: a function proved `PROVED-ASSUMING H`
(no UB on inputs satisfying `H`) together with callers that establish `H` at
every call site gives a whole program without UB in those calls. -/
theorem harness_assumption_discharged (impl : (f : Fn) → Arg f → Option (Ret f))
    (H : (f : Fn) → Arg f → Prop)
    (hproved : ∀ f, ProvedAssuming (H f) (fun a => impl f a ≠ none))
    {R : Type} (p : Prog Fn Arg Ret R) (Q : R → Prop)
    (hp : WP (fun f => ⟨H f, fun _ _ => True⟩) p Q) :
    ∃ r, run impl p = some r ∧ Q r := by
  apply modular_sound impl _ _ p Q hp
  intro f a ha
  cases h : impl f a with
  | none => exact absurd h (hproved f a ha)
  | some b => exact ⟨b, rfl, trivial⟩

/-! ### Worked example: a guarded division -/

/-- `div (a, b)` has UB when `b = 0`. -/
def divImpl : Unit → Nat × Nat → Option Nat := fun _ ab =>
  if ab.2 = 0 then none else some (ab.1 / ab.2)

def divSpec : Unit → Contract (Nat × Nat) Nat := fun _ =>
  ⟨fun ab => ab.2 ≠ 0, fun ab r => r * ab.2 ≤ ab.1⟩

theorem div_satisfies : ∀ f, Satisfies divImpl divSpec f := by
  intro f ab hreq
  refine ⟨ab.1 / ab.2, by simp [divImpl, show ab.2 ≠ 0 from hreq], ?_⟩
  exact Nat.div_mul_le_self ab.1 ab.2

/-- Caller: `r := div(n, x + 1); return r`. -/
def caller (n x : Nat) : Prog Unit (fun _ => Nat × Nat) (fun _ => Nat) Nat :=
  .call () (n, x + 1) (fun r => .ret r)

theorem caller_safe (n x : Nat) :
    ∃ r, run divImpl (caller n x) = some r ∧ r * (x + 1) ≤ n :=
  modular_sound divImpl divSpec div_satisfies (caller n x) (fun r => r * (x + 1) ≤ n)
    ⟨Nat.succ_ne_zero x, fun _ h => h⟩

end PrismTechniques.Contracts
