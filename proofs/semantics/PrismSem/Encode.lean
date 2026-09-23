/-
PRISM PIR — the bounded encoder and its soundness/completeness
(roadmap Part 5.3; Part 8.2 row "Encoder (bounded)").

The encoder is symbolic execution into a formula over the program inputs,
in the shape of a BMC encoder (CBMC/ESBMC style):

* straight-line code: SSA by substitution — `σ` maps each variable to the
  expression (over the inputs) that its current SSA version denotes;
* branches: both arms are encoded under guards `g ∧ c` / `g ∧ ¬c`, and the
  two substitutions are merged with `select` (φ-nodes as `ite`);
* loops: unrolled `k` times; after the `k`-th unrolling an *unwinding
  assertion* records "the loop condition still holds".

The symbolic state carries the path guard `g` (the current point is
reached normally) and three accumulated violation formulas: `fl` (a PIR
assertion failed), `ub` (an operation with UB was executed) and `uw` (a loop
needed more than `k` iterations).  Formulas are PIR `i1` expressions; their
meaning is `evalE` (the `nsw`/`nuw`/`total` flags do not affect values).

Main results (`ρ₀` ranges over all inputs = initial environments):
* `encode_fail_iff`, `encode_ub_iff`, `encode_unwind_iff` — each formula is
  *exact*: it is true at `ρ₀` iff the bounded execution from `ρ₀` ends with
  that violation.
* `bmc_sound` — if `fl ∨ ub` is unsatisfiable, no execution within the bound
  fails an assertion or has UB; `bmc_complete` — every model is a real
  counterexample in the reference semantics.
* `bmc_sound_unbounded` — if `fl ∨ ub ∨ uw` is unsatisfiable, then *every*
  execution of the reference semantics terminates and is free of assertion
  failures and UB (the unwinding assertion closes the bound).
* Layered corollaries for straight-line and loop-free programs.
-/
import PrismSem.Instrument

namespace PrismSem

/-! ### Substitutions (SSA) -/

/-- A substitution: the current SSA value of every variable, as an
expression over the inputs. -/
def Subst : Type := (w : Nat) → String → Expr w

def Subst.id : Subst := fun w x => .var w x

def Subst.set (σ : Subst) (w : Nat) (x : String) (e : Expr w) : Subst :=
  fun w' y => if h : w = w' then (if y = x then h ▸ e else σ w' y) else σ w' y

/-- φ-merge of two substitutions by an `i1` condition. -/
def Subst.merge (c : Expr 1) (σ₁ σ₂ : Subst) : Subst :=
  fun w x => .select c (σ₁ w x) (σ₂ w x)

def Expr.subst (σ : Subst) : {w : Nat} → Expr w → Expr w
  | _, .const v => .const v
  | _, .var w x => σ w x
  | _, .bin op fl a b => .bin op fl (a.subst σ) (b.subst σ)
  | _, .icmp p a b => .icmp p (a.subst σ) (b.subst σ)
  | _, .ovf op a b => .ovf op (a.subst σ) (b.subst σ)
  | _, .select c a b => .select (c.subst σ) (a.subst σ) (b.subst σ)
  | _, .zext v a => .zext v (a.subst σ)
  | _, .sext v a => .sext v (a.subst σ)
  | _, .trunc v a => .trunc v (a.subst σ)

/-- The environment a substitution denotes at inputs `ρ₀`. -/
def Subst.eval (σ : Subst) (ρ₀ : Env) : Env := fun w x => evalE ρ₀ (σ w x)

theorem evalE_subst (ρ₀ : Env) (σ : Subst) {w : Nat} (e : Expr w) :
    evalE ρ₀ (e.subst σ) = evalE (σ.eval ρ₀) e := by
  induction e <;> simp_all [Expr.subst, evalE, Subst.eval]

@[simp] theorem Subst.eval_id (ρ₀ : Env) : Subst.id.eval ρ₀ = ρ₀ := rfl

theorem Subst.eval_set (σ : Subst) (ρ₀ : Env) (w : Nat) (x : String) (e : Expr w) :
    (σ.set w x e).eval ρ₀ = (σ.eval ρ₀).set w x (evalE ρ₀ e) := by
  funext w' y
  simp only [Subst.eval, Subst.set, Env.set]
  by_cases hw : w = w'
  · subst hw; by_cases hy : y = x <;> simp [hy]
  · simp [hw]

theorem Subst.eval_merge (c : Expr 1) (σ₁ σ₂ : Subst) (ρ₀ : Env) :
    (Subst.merge c σ₁ σ₂).eval ρ₀ =
      if truth (evalE ρ₀ c) then σ₁.eval ρ₀ else σ₂.eval ρ₀ := by
  funext w x
  by_cases h : truth (evalE ρ₀ c) <;> simp [Subst.eval, Subst.merge, evalE, h]

/-! ### Symbolic states and the encoder -/

structure SymSt where
  σ : Subst
  /-- path guard: this program point is reached normally -/
  g : Expr 1
  /-- some assertion has failed -/
  fl : Expr 1
  /-- some loop needed more than `k` iterations -/
  uw : Expr 1
  /-- some operation with undefined behaviour was executed -/
  ub : Expr 1

open Expr in
/-- Evaluating `e` at the current point: record its UB condition and continue
only where it is UB-free. -/
def SymSt.chkUb {w : Nat} (S : SymSt) (e : Expr w) : SymSt :=
  let u := (ubExpr e).subst S.σ
  { S with ub := or' S.ub (and' S.g u), g := and' S.g (not' u) }

open Expr in
/-- Bounded loop unrolling; `E` is the encoder of the body. -/
def encLoop (c : Expr 1) (E : SymSt → SymSt) : Nat → SymSt → SymSt
  | 0, S =>
    let S₀ := S.chkUb c
    let c' := c.subst S₀.σ
    { S₀ with uw := or' S₀.uw (and' S₀.g c'), g := and' S₀.g (not' c') }
  | n + 1, S =>
    let S₀ := S.chkUb c
    let c' := c.subst S₀.σ
    let S₁ := encLoop c E n (E { S₀ with g := and' S₀.g c' })
    { S₁ with σ := Subst.merge c' S₁.σ S₀.σ, g := or' S₁.g (and' S₀.g (not' c')) }

open Expr in
/-- The encoder with unwinding bound `k`. -/
def enc (k : Nat) : Stmt → SymSt → SymSt
  | .skip, S => S
  | .assign (w := w) x e, S =>
    let S₁ := S.chkUb e
    { S₁ with σ := S₁.σ.set w x (e.subst S₁.σ) }
  | .assert _ c, S =>
    let S₁ := S.chkUb c
    let c' := c.subst S₁.σ
    { S₁ with fl := or' S₁.fl (and' S₁.g (not' c')), g := and' S₁.g c' }
  | .assume c, S =>
    let S₁ := S.chkUb c
    { S₁ with g := and' S₁.g (c.subst S₁.σ) }
  | .seq s t, S => enc k t (enc k s S)
  | .ite c s t, S =>
    let S₀ := S.chkUb c
    let c' := c.subst S₀.σ
    let S₁ := enc k s { S₀ with g := and' S₀.g c' }
    let S₂ := enc k t { S₁ with σ := S₀.σ, g := and' S₀.g (not' c') }
    { S₂ with σ := Subst.merge c' S₁.σ S₂.σ, g := or' S₁.g S₂.g }
  | .loop c b, S => encLoop c (enc k b) k S
  | .ret, S => { S with g := ff }

/-- Initial symbolic state: every variable is its input value. -/
def SymSt.init : SymSt :=
  { σ := Subst.id, g := Expr.tt, fl := Expr.ff, uw := Expr.ff, ub := Expr.ff }

/-- The encoding of program `p` with bound `k`. -/
def encode (k : Nat) (p : Stmt) : SymSt := enc k p SymSt.init

/-- Verification condition "an assertion fails or UB happens within the
bound" (bounded mode, unwinding *assumption*). -/
def vcBounded (k : Nat) (p : Stmt) : Expr 1 :=
  Expr.or' (encode k p).fl (encode k p).ub

/-- Verification condition with the unwinding *assertion* (CBMC default). -/
def vcFull (k : Nat) (p : Stmt) : Expr 1 :=
  Expr.or' (vcBounded k p) (encode k p).uw

/-- Satisfiability of an `i1` formula over the inputs. -/
def Sat (φ : Expr 1) : Prop := ∃ ρ₀ : Env, truth (evalE ρ₀ φ) = true

/-! ### The specification of one encoding step -/

def Outcome.isFail : Outcome → Bool
  | .fail _ => true
  | _ => false

def Outcome.isUnwind : Outcome → Bool
  | .unwind => true
  | _ => false

def Outcome.isUb : Outcome → Bool
  | .ub => true
  | _ => false

/-- `Spec ρ₀ S S' o`: encoding from `S` to `S'` agrees, at inputs `ρ₀`, with
the concrete outcome `o`: the guard of `S'` holds iff `o` is normal, each
violation formula gains exactly the violation `o` has, and on a normal
outcome `S'.σ` denotes the final environment. -/
structure Spec (ρ₀ : Env) (S S' : SymSt) (o : Outcome) : Prop where
  g : truth (evalE ρ₀ S'.g) = o.isNormal
  fl : truth (evalE ρ₀ S'.fl) = (truth (evalE ρ₀ S.fl) || o.isFail)
  uw : truth (evalE ρ₀ S'.uw) = (truth (evalE ρ₀ S.uw) || o.isUnwind)
  ub : truth (evalE ρ₀ S'.ub) = (truth (evalE ρ₀ S.ub) || o.isUb)
  env : ∀ ρ', o = .normal ρ' → S'.σ.eval ρ₀ = ρ'

/-- The outcome the encoder must track from state `S`: the concrete run
if `S` is active, otherwise nothing happens (`blocked`). -/
def target (ρ₀ : Env) (S : SymSt) (f : Env → Outcome) : Outcome :=
  if truth (evalE ρ₀ S.g) then f (S.σ.eval ρ₀) else .blocked

theorem Spec.bind {ρ₀ : Env} {S S₁ S₂ : SymSt} {o₁ : Outcome} {f : Env → Outcome}
    (h₁ : Spec ρ₀ S S₁ o₁) (h₂ : Spec ρ₀ S₁ S₂ (target ρ₀ S₁ f)) :
    Spec ρ₀ S S₂ (o₁.bind f) := by
  cases o₁ with
  | normal ρ' =>
    have hg : truth (evalE ρ₀ S₁.g) = true := by simpa [Outcome.isNormal] using h₁.g
    have he := h₁.env ρ' rfl
    simp only [target, hg, he, ↓reduceIte] at h₂
    refine ⟨h₂.g, ?_, ?_, ?_, h₂.env⟩
    · rw [h₂.fl, h₁.fl]; simp [Outcome.isFail]
    · rw [h₂.uw, h₁.uw]; simp [Outcome.isUnwind]
    · rw [h₂.ub, h₁.ub]; simp [Outcome.isUb]
  | _ =>
    have hg : truth (evalE ρ₀ S₁.g) = false := by simpa [Outcome.isNormal] using h₁.g
    simp only [target, hg, Bool.false_eq_true, ↓reduceIte] at h₂
    refine ⟨?_, ?_, ?_, ?_, ?_⟩
    · rw [h₂.g]; rfl
    · rw [h₂.fl, h₁.fl]; simp [Outcome.isFail]
    · rw [h₂.uw, h₁.uw]; simp [Outcome.isUnwind]
    · rw [h₂.ub, h₁.ub]; simp [Outcome.isUb]
    · intro ρ' h; cases h

theorem target_bind (ρ₀ : Env) (S : SymSt) (f : Env → Outcome) (g : Env → Outcome) :
    target ρ₀ S (fun ρ => (f ρ).bind g) = (target ρ₀ S f).bind g := by
  unfold target; split <;> rfl

@[simp] theorem truth_eval_tt (ρ₀ : Env) : truth (evalE ρ₀ Expr.tt) = true := by
  simp [Expr.tt, evalE]
@[simp] theorem truth_eval_ff (ρ₀ : Env) : truth (evalE ρ₀ Expr.ff) = false := by
  simp [Expr.ff, evalE]
@[simp] theorem truth_eval_or (ρ₀ : Env) (a b : Expr 1) :
    truth (evalE ρ₀ (Expr.or' a b)) = (truth (evalE ρ₀ a) || truth (evalE ρ₀ b)) := by
  simp [Expr.or', evalE, evalBin]
@[simp] theorem truth_eval_and (ρ₀ : Env) (a b : Expr 1) :
    truth (evalE ρ₀ (Expr.and' a b)) = (truth (evalE ρ₀ a) && truth (evalE ρ₀ b)) := by
  simp [Expr.and', evalE, evalBin]
@[simp] theorem truth_eval_not (ρ₀ : Env) (a : Expr 1) :
    truth (evalE ρ₀ (Expr.not' a)) = !truth (evalE ρ₀ a) := by
  simp [Expr.not', Expr.tt, evalE, evalBin]

theorem truth_ub_subst (ρ₀ : Env) (σ : Subst) {w : Nat} (e : Expr w) :
    truth (evalE ρ₀ ((ubExpr e).subst σ)) = ubE (σ.eval ρ₀) e := by
  rw [evalE_subst, ubExpr_correct]

/-- One step of `chkUb`, evaluated. -/
theorem chkUb_eval (ρ₀ : Env) (S : SymSt) {w : Nat} (e : Expr w) :
    (S.chkUb e).σ = S.σ ∧
    truth (evalE ρ₀ (S.chkUb e).g) = (truth (evalE ρ₀ S.g) && !ubE (S.σ.eval ρ₀) e) ∧
    truth (evalE ρ₀ (S.chkUb e).ub) =
      (truth (evalE ρ₀ S.ub) || (truth (evalE ρ₀ S.g) && ubE (S.σ.eval ρ₀) e)) ∧
    (S.chkUb e).fl = S.fl ∧ (S.chkUb e).uw = S.uw := by
  simp [SymSt.chkUb, truth_ub_subst]

/-! ### The encoder is exact, statement by statement -/

theorem encLoop_spec (ρ₀ : Env) (c : Expr 1) (E : SymSt → SymSt) (B : Env → Outcome)
    (hE : ∀ S, Spec ρ₀ S (E S) (target ρ₀ S B)) :
    ∀ n S, Spec ρ₀ S (encLoop c E n S) (target ρ₀ S (loopRun c B n)) := by
  intro n
  induction n with
  | zero =>
    intro S
    obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S c
    simp only [encLoop, target]
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.σ.eval ρ₀) c <;>
    by_cases cv : truth (evalE (S.σ.eval ρ₀) c) <;>
    · constructor <;>
        simp_all [evalE_subst, loopRun, Outcome.isNormal, Outcome.isFail, Outcome.isUnwind,
          Outcome.isUb]
  | succ n ih =>
    intro S
    obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S c
    let S₀ := S.chkUb c
    let Sa : SymSt := { S₀ with g := Expr.and' S₀.g (c.subst S₀.σ) }
    have hA : Spec ρ₀ Sa (encLoop c E n (E Sa)) ((target ρ₀ Sa B).bind (loopRun c B n)) :=
      Spec.bind (hE Sa) (ih (E Sa))
    obtain ⟨Ag, Afl, Auw, Aub, Aenv⟩ := hA
    have hSa_g : truth (evalE ρ₀ Sa.g) =
        (truth (evalE ρ₀ S.g) && !ubE (S.σ.eval ρ₀) c && truth (evalE (S.σ.eval ρ₀) c)) := by
      simp [Sa, S₀, hg, evalE_subst, hσ]
    have hSa_σ : Sa.σ = S.σ := hσ
    simp only [encLoop, target]
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.σ.eval ρ₀) c <;>
    by_cases cv : truth (evalE (S.σ.eval ρ₀) c)
    all_goals
      simp only [target, hSa_g, hSa_σ, g, u, cv, Bool.true_and, Bool.and_true, Bool.not_true,
        Bool.not_false, Bool.and_false, Bool.false_and, Bool.false_eq_true, ↓reduceIte,
        Outcome.bind_blocked] at Ag Afl Auw Aub Aenv
      constructor <;>
        simp_all [Sa, S₀, evalE_subst, Subst.eval_merge, loopRun, Outcome.isNormal,
          Outcome.isFail, Outcome.isUnwind, Outcome.isUb]

/-- Branch combination, stated over arbitrary result states `S₁`, `S₂`. -/
theorem spec_ite (ρ₀ : Env) (S S₁ S₂ : SymSt) (c : Expr 1) (f₁ f₂ : Env → Outcome)
    (h₁ : Spec ρ₀ { S.chkUb c with g := Expr.and' (S.chkUb c).g (c.subst (S.chkUb c).σ) } S₁
      (target ρ₀ { S.chkUb c with g := Expr.and' (S.chkUb c).g (c.subst (S.chkUb c).σ) } f₁))
    (h₂ : Spec ρ₀ { S₁ with σ := (S.chkUb c).σ, g := Expr.and' (S.chkUb c).g (Expr.not' (c.subst (S.chkUb c).σ)) } S₂
      (target ρ₀ { S₁ with σ := (S.chkUb c).σ, g := Expr.and' (S.chkUb c).g (Expr.not' (c.subst (S.chkUb c).σ)) } f₂)) :
    Spec ρ₀ S { S₂ with σ := Subst.merge (c.subst (S.chkUb c).σ) S₁.σ S₂.σ, g := Expr.or' S₁.g S₂.g }
      (target ρ₀ S (fun ρ => if ubE ρ c then .ub else if truth (evalE ρ c) then f₁ ρ else f₂ ρ)) := by
  obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S c
  obtain ⟨Ag, Afl, Auw, Aub, Aenv⟩ := h₁
  obtain ⟨Bg, Bfl, Buw, Bub, Benv⟩ := h₂
  simp only [target, hσ, evalE_subst, truth_eval_and, truth_eval_not, hg] at Ag Afl Auw Aub Aenv
  simp only [target, hσ, evalE_subst, truth_eval_and, truth_eval_not, hg] at Bg Bfl Buw Bub Benv
  simp only [hfl, huw, hub] at Afl Auw Aub
  simp only [target]
  by_cases g : truth (evalE ρ₀ S.g) <;>
  by_cases u : ubE (S.σ.eval ρ₀) c <;>
  by_cases cv : truth (evalE (S.σ.eval ρ₀) c)
  all_goals
    simp only [g, u, cv, Bool.true_and, Bool.and_true, Bool.not_true, Bool.not_false,
      Bool.and_false, Bool.false_and, Bool.false_eq_true, ↓reduceIte] at Ag Afl Auw Aub Aenv
    simp only [g, u, cv, Bool.true_and, Bool.and_true, Bool.not_true, Bool.not_false,
      Bool.and_false, Bool.false_and, Bool.false_eq_true, ↓reduceIte] at Bg Bfl Buw Bub Benv
    constructor <;>
      simp_all [hσ, evalE_subst, Subst.eval_merge, Outcome.isNormal,
        Outcome.isFail, Outcome.isUnwind, Outcome.isUb]

theorem enc_spec (k : Nat) (ρ₀ : Env) (s : Stmt) :
    ∀ S, Spec ρ₀ S (enc k s S) (target ρ₀ S (run k s)) := by
  induction s with
  | skip =>
    intro S
    unfold target
    by_cases g : truth (evalE ρ₀ S.g) <;>
      constructor <;> simp_all [enc, run, Outcome.isNormal, Outcome.isFail, Outcome.isUnwind, Outcome.isUb]
  | ret =>
    intro S
    unfold target
    by_cases g : truth (evalE ρ₀ S.g) <;>
      constructor <;> simp_all [enc, run, Outcome.isNormal, Outcome.isFail, Outcome.isUnwind, Outcome.isUb]
  | assign x e =>
    intro S
    obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S e
    unfold target
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.σ.eval ρ₀) e <;>
    · constructor <;>
        simp_all [enc, run, Subst.eval_set, evalE_subst, Outcome.isNormal, Outcome.isFail,
          Outcome.isUnwind, Outcome.isUb]
  | assert t c =>
    intro S
    obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S c
    unfold target
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.σ.eval ρ₀) c <;>
    by_cases cv : truth (evalE (S.σ.eval ρ₀) c) <;>
    · constructor <;>
        simp_all [enc, run, evalE_subst, Outcome.isNormal, Outcome.isFail,
          Outcome.isUnwind, Outcome.isUb]
  | assume c =>
    intro S
    obtain ⟨hσ, hg, hub, hfl, huw⟩ := chkUb_eval ρ₀ S c
    unfold target
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.σ.eval ρ₀) c <;>
    by_cases cv : truth (evalE (S.σ.eval ρ₀) c) <;>
    · constructor <;>
        simp_all [enc, run, evalE_subst, Outcome.isNormal, Outcome.isFail,
          Outcome.isUnwind, Outcome.isUb]
  | seq s t ihs iht =>
    intro S
    have h := Spec.bind (ihs S) (iht (enc k s S))
    simp only [enc]
    have e : target ρ₀ S (run k (.seq s t)) = (target ρ₀ S (run k s)).bind (run k t) := by
      rw [← target_bind]; rfl
    rw [e]; exact h
  | ite c s t ihs iht =>
    intro S
    exact spec_ite ρ₀ S _ _ c (run k s) (run k t) (ihs _) (iht _)
  | loop c b ihb =>
    intro S
    exact encLoop_spec ρ₀ c (enc k b) (run k b) ihb k S

/-! ### Top-level theorems -/

theorem encode_spec (k : Nat) (p : Stmt) (ρ₀ : Env) :
    Spec ρ₀ SymSt.init (encode k p) (run k p ρ₀) := by
  have h := enc_spec k ρ₀ p SymSt.init
  unfold encode
  simpa [target, SymSt.init] using h

/-- **Exactness of the failure formula.** -/
theorem encode_fail_iff (k : Nat) (p : Stmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (encode k p).fl) = true ↔ ∃ t, run k p ρ₀ = .fail t := by
  rw [(encode_spec k p ρ₀).fl]
  cases run k p ρ₀ <;> simp [SymSt.init, Outcome.isFail]

/-- **Exactness of the UB formula.** -/
theorem encode_ub_iff (k : Nat) (p : Stmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (encode k p).ub) = true ↔ run k p ρ₀ = .ub := by
  rw [(encode_spec k p ρ₀).ub]
  cases run k p ρ₀ <;> simp [SymSt.init, Outcome.isUb]

/-- **Exactness of the unwinding formula.** -/
theorem encode_unwind_iff (k : Nat) (p : Stmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (encode k p).uw) = true ↔ run k p ρ₀ = .unwind := by
  rw [(encode_spec k p ρ₀).uw]
  cases run k p ρ₀ <;> simp [SymSt.init, Outcome.isUnwind]

theorem vcBounded_iff (k : Nat) (p : Stmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (vcBounded k p)) = true ↔
      (∃ t, run k p ρ₀ = .fail t) ∨ run k p ρ₀ = .ub := by
  simp only [vcBounded, truth_eval_or, Bool.or_eq_true, encode_fail_iff, encode_ub_iff]

theorem vcFull_iff (k : Nat) (p : Stmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (vcFull k p)) = true ↔
      (∃ t, run k p ρ₀ = .fail t) ∨ run k p ρ₀ = .ub ∨ run k p ρ₀ = .unwind := by
  simp only [vcFull, truth_eval_or, Bool.or_eq_true, encode_unwind_iff]
  simp only [vcBounded_iff, or_assoc]

/-- **Encoder soundness, bounded (5.3 / 8.2).**  If the bounded VC is
unsatisfiable, no execution within the bound fails an assertion or executes
UB. -/
theorem bmc_sound (k : Nat) (p : Stmt) (h : ¬ Sat (vcBounded k p)) :
    ∀ ρ₀, (∀ t, run k p ρ₀ ≠ .fail t) ∧ run k p ρ₀ ≠ .ub := by
  intro ρ₀
  have : ¬ ((∃ t, run k p ρ₀ = .fail t) ∨ run k p ρ₀ = .ub) :=
    fun hc => h ⟨ρ₀, (vcBounded_iff k p ρ₀).2 hc⟩
  exact ⟨fun t ht => this (Or.inl ⟨t, ht⟩), fun hu => this (Or.inr hu)⟩

/-- **Encoder completeness, bounded.**  Every model of the bounded VC is a
real counterexample of the reference semantics (no spurious alarms). -/
theorem bmc_complete (k : Nat) (p : Stmt) (ρ₀ : Env)
    (h : truth (evalE ρ₀ (vcBounded k p)) = true) :
    (∃ t, BigStep p ρ₀ (.fail t)) ∨ BigStep p ρ₀ .ub := by
  rcases (vcBounded_iff k p ρ₀).1 h with ⟨t, ht⟩ | hu
  · exact Or.inl ⟨t, run_sound k p ρ₀ _ ht (by simp)⟩
  · exact Or.inr (run_sound k p ρ₀ _ hu (by simp))

/-- **Encoder soundness with the unwinding assertion.**  If the full VC is
unsatisfiable, then on every input the program terminates (within `k`
iterations per loop entry), and no execution of the *unbounded* reference
semantics fails an assertion or executes UB. -/
theorem bmc_sound_unbounded (k : Nat) (p : Stmt) (h : ¬ Sat (vcFull k p)) :
    ∀ ρ₀, (∃ o, BigStep p ρ₀ o) ∧
      ∀ o, BigStep p ρ₀ o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub := by
  intro ρ₀
  have hn : ¬ ((∃ t, run k p ρ₀ = .fail t) ∨ run k p ρ₀ = .ub ∨ run k p ρ₀ = .unwind) :=
    fun hc => h ⟨ρ₀, (vcFull_iff k p ρ₀).2 hc⟩
  have hnu : run k p ρ₀ ≠ .unwind := fun hu => hn (Or.inr (Or.inr hu))
  have hb : BigStep p ρ₀ (run k p ρ₀) := run_sound k p ρ₀ _ rfl hnu
  refine ⟨⟨_, hb⟩, fun o ho => ?_⟩
  have := BigStep.det ho hb
  subst this
  exact ⟨fun t ht => hn (Or.inl ⟨t, ht⟩), fun hu => hn (Or.inr (Or.inl hu))⟩

/-- **Completeness with the unwinding assertion.**  A model of the full VC is
either a real counterexample or a witness that the bound `k` is too small. -/
theorem bmc_complete_full (k : Nat) (p : Stmt) (ρ₀ : Env)
    (h : truth (evalE ρ₀ (vcFull k p)) = true) :
    (∃ t, BigStep p ρ₀ (.fail t)) ∨ BigStep p ρ₀ .ub ∨ run k p ρ₀ = .unwind := by
  rcases (vcFull_iff k p ρ₀).1 h with ⟨t, ht⟩ | hu | hw
  · exact Or.inl ⟨t, run_sound k p ρ₀ _ ht (by simp)⟩
  · exact Or.inr (Or.inl (run_sound k p ρ₀ _ hu (by simp)))
  · exact Or.inr (Or.inr hw)

/-- For every failing execution there is a bound at which the bounded VC
has a model (bounded model checking is complete in the limit). -/
theorem bmc_complete_limit (p : Stmt) (ρ₀ : Env)
    (h : (∃ t, BigStep p ρ₀ (.fail t)) ∨ BigStep p ρ₀ .ub) :
    ∃ k, truth (evalE ρ₀ (vcBounded k p)) = true := by
  rcases h with ⟨t, ht⟩ | hu
  · obtain ⟨k, hk⟩ := run_adequate ht
    exact ⟨k, (vcBounded_iff k p ρ₀).2 (Or.inl ⟨t, hk⟩)⟩
  · obtain ⟨k, hk⟩ := run_adequate hu
    exact ⟨k, (vcBounded_iff k p ρ₀).2 (Or.inr hk)⟩

/-! ### Layers: straight-line and loop-free programs

For loop-free programs the unwinding formula is identically false, so the
bounded VC decides safety of the reference semantics exactly, for any `k`. -/

theorem run_loopFree_ne_unwind (k : Nat) (p : Stmt) (hp : p.LoopFree) :
    ∀ ρ, run k p ρ ≠ .unwind := by
  induction p with
  | seq s t ihs iht =>
    intro ρ
    simp only [run]
    cases h : run k s ρ with
    | normal ρ' => exact iht hp.2 ρ'
    | unwind => exact absurd h (ihs hp.1 ρ)
    | _ => simp [Outcome.bind]
  | ite c s t ihs iht =>
    intro ρ
    simp only [run]
    cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp [ihs hp.1, iht hp.2]
  | loop => exact absurd hp id
  | assign x e => intro ρ; simp only [run]; cases ubE ρ e <;> simp
  | assert t c => intro ρ; simp only [run]; cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
  | assume c => intro ρ; simp only [run]; cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
  | skip => intro ρ; simp [run]
  | ret => intro ρ; simp [run]

theorem Stmt.StraightLine.loopFree {p : Stmt} (hp : p.StraightLine) : p.LoopFree := by
  induction p with
  | seq s t ihs iht => exact ⟨ihs hp.1, iht hp.2⟩
  | ite => exact absurd hp id
  | loop => exact absurd hp id
  | _ => trivial

/-- **Layer 2 (branches): exact decision for loop-free code.**  The bounded
VC of a loop-free program is unsatisfiable iff no execution of the reference
semantics fails an assertion or executes UB. -/
theorem loopFree_encode_exact (k : Nat) (p : Stmt) (hp : p.LoopFree) :
    ¬ Sat (vcBounded k p) ↔
      ∀ ρ₀ o, BigStep p ρ₀ o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub := by
  constructor
  · intro h ρ₀ o ho
    have hs := bmc_sound k p h ρ₀
    have hr := run_sound k p ρ₀ _ rfl (run_loopFree_ne_unwind k p hp ρ₀)
    have := BigStep.det ho hr; subst this
    exact hs
  · rintro h ⟨ρ₀, hρ⟩
    rcases bmc_complete k p ρ₀ hρ with ⟨t, ht⟩ | hu
    · exact (h ρ₀ _ ht).1 t rfl
    · exact (h ρ₀ _ hu).2 rfl

/-- **Layer 1 (straight-line code).** -/
theorem straightLine_encode_exact (k : Nat) (p : Stmt) (hp : p.StraightLine) :
    ¬ Sat (vcBounded k p) ↔
      ∀ ρ₀ o, BigStep p ρ₀ o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub :=
  loopFree_encode_exact k p hp.loopFree

/-! ### Instrumentation and encoding together -/

/-- Checking the instrumented program: if the assertion formula of
`encode k (instr p)` is unsatisfiable, the original program has no UB and no
failing user assertion within the bound. -/
theorem instr_encode_sound (k : Nat) (p : Stmt) (h : ¬ Sat (encode k (instr p)).fl) :
    ∀ ρ₀, run k p ρ₀ ≠ .ub ∧ ∀ t, run k p ρ₀ ≠ .fail t := by
  intro ρ₀
  have hf : ∀ t, run k (instr p) ρ₀ ≠ .fail t := fun t ht =>
    h ⟨ρ₀, (encode_fail_iff k (instr p) ρ₀).2 ⟨t, ht⟩⟩
  rw [run_instr] at hf
  refine ⟨fun hu => hf .ub (by rw [hu]; rfl), fun t ht => hf t (by rw [ht]; rfl)⟩

end PrismSem
