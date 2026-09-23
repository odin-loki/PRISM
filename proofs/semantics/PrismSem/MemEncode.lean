/-
PRISM PIR — the bounded encoder for programs with memory, and its
exactness (roadmap Part 5.3 layers "memory" + branches + bounded loops;
Part 8.2 rows "Encoder (bounded)" and "Memory model").

The symbolic heap is fully symbolic: object ids, sizes, liveness and bytes
are expressions over the inputs.  Every heap component is a function from
symbolic indices to expressions, built as read-over-write `select` chains:

* `alloc`: `size o := select (o = next) sz (size o)`, `live`, `data` alike,
  `next := next + 1`;
* `free`: `live o := select (o = obj p) false (live o)`;
* `store`: `data o i := select (o = obj p ∧ i = off p) v (data o i)`;
* branches and loop exits: every component merged with `select c`.

Main results: `mencode_fail_iff`, `mencode_ub_iff`, `mencode_unwind_iff`
(each formula is exact), hence `mbmc_sound`, `mbmc_complete`,
`mbmc_sound_full`.
-/
import PrismSem.Memory

-- Proof scripts share simp sets across `<;>` branches; unused-argument noise is expected.
set_option linter.unusedSimpArgs false

namespace PrismSem.Mem

open Expr

/-! ### Symbolic states -/

structure MSym where
  σ : Subst
  π : String → Expr 64 × Expr 64
  size : Expr 64 → Expr 64
  live : Expr 64 → Expr 1
  data : Expr 64 → Expr 64 → Expr 8
  next : Expr 64
  g : Expr 1
  fl : Expr 1
  uw : Expr 1
  ub : Expr 1

def eqE (a b : Expr 64) : Expr 1 := .icmp .eq a b

/-- The concrete state a symbolic state denotes at inputs `ρ₀`. -/
def MSym.toState (S : MSym) (ρ₀ : Env) : MState where
  ρ := S.σ.eval ρ₀
  π := fun p => ⟨evalE ρ₀ (S.π p).1, evalE ρ₀ (S.π p).2⟩
  heap := fun n =>
    ⟨evalE ρ₀ (S.size (.const n)), truth (evalE ρ₀ (S.live (.const n))),
     fun i => evalE ρ₀ (S.data (.const n) (.const i))⟩
  next := evalE ρ₀ S.next

/-- `toState` only reads the memory and variable components. -/
@[simp] theorem toState_mk (S : MSym) (ρ₀ : Env) (g fl uw ub : Expr 1) :
    ({ σ := S.σ, π := S.π, size := S.size, live := S.live, data := S.data, next := S.next,
       g := g, fl := fl, uw := uw, ub := ub } : MSym).toState ρ₀ = S.toState ρ₀ := rfl

/-- Congruence: the symbolic function respects evaluation.  All functions
built by the encoder have this property (`menc_wf`). -/
def Cong1 {v : Nat} (f : Expr 64 → Expr v) : Prop :=
  ∀ ρ a b, evalE ρ a = evalE ρ b → evalE ρ (f a) = evalE ρ (f b)

def Cong2 (f : Expr 64 → Expr 64 → Expr 8) : Prop :=
  ∀ ρ a b c d, evalE ρ a = evalE ρ b → evalE ρ c = evalE ρ d → evalE ρ (f a c) = evalE ρ (f b d)

def MSym.WF (S : MSym) : Prop := Cong1 S.size ∧ Cong1 S.live ∧ Cong2 S.data

theorem MSym.WF.size {S : MSym} (h : S.WF) : Cong1 S.size := h.1
theorem MSym.WF.live {S : MSym} (h : S.WF) : Cong1 S.live := h.2.1
theorem MSym.WF.data {S : MSym} (h : S.WF) : Cong2 S.data := h.2.2

/-! Field lemmas for `toState` (used instead of unfolding it). -/
@[simp] theorem toState_ρ (S : MSym) (ρ₀ : Env) : (S.toState ρ₀).ρ = S.σ.eval ρ₀ := rfl
@[simp] theorem toState_obj (S : MSym) (ρ₀ : Env) (p : String) :
    ((S.toState ρ₀).π p).obj = evalE ρ₀ (S.π p).1 := rfl
@[simp] theorem toState_off (S : MSym) (ρ₀ : Env) (p : String) :
    ((S.toState ρ₀).π p).off = evalE ρ₀ (S.π p).2 := rfl
@[simp] theorem toState_next (S : MSym) (ρ₀ : Env) : (S.toState ρ₀).next = evalE ρ₀ S.next := rfl

/-! ### Encoder -/

def MSym.guardUb (S : MSym) (u : Expr 1) : MSym :=
  { S with ub := or' S.ub (and' S.g u), g := and' S.g (not' u) }

def objOf (S : MSym) (p : String) : Expr 64 := (S.π p).1
def offOf (S : MSym) (p : String) : Expr 64 := (S.π p).2

def okExpr (S : MSym) (p : String) : Expr 1 :=
  and' (S.live (objOf S p)) (.icmp .ult (offOf S p) (S.size (objOf S p)))

def freeOkExpr (S : MSym) (p : String) : Expr 1 :=
  and' (S.live (objOf S p)) (eqE (offOf S p) (.const 0#64))

def condExpr (S : MSym) : MCond → Expr 1
  | .bv c => c.subst S.σ
  | .live p => S.live (objOf S p)
  | .inBounds p => .icmp .ult (offOf S p) (S.size (objOf S p))
  | .atBase p => eqE (offOf S p) (.const 0#64)

def ubCond : MCond → Expr 1
  | .bv c => ubExpr c
  | _ => ff

def MSym.allocS (S : MSym) (p : String) (sz : Expr 64) : MSym :=
  { S with size := fun o => .select (eqE o S.next) sz (S.size o),
           live := fun o => .select (eqE o S.next) tt (S.live o),
           data := fun o i => .select (eqE o S.next) (.const 0#8) (S.data o i),
           π := upd S.π p (S.next, .const 0#64),
           next := .bin .add {} S.next (.const 1#64) }

def MSym.freeS (S : MSym) (p : String) : MSym :=
  { S with live := fun o => .select (eqE o (objOf S p)) ff (S.live o) }

def MSym.storeS (S : MSym) (p : String) (v : Expr 8) : MSym :=
  { S with data := fun o i =>
      .select (and' (eqE o (objOf S p)) (eqE i (offOf S p))) v (S.data o i) }

/-- φ-merge: memory, pointers and variables selected by `c`; violation
formulas taken from `S₂` (which already includes those of `S₁`). -/
def MSym.merge (c : Expr 1) (S₁ S₂ : MSym) : MSym where
  σ := Subst.merge c S₁.σ S₂.σ
  π := fun p => (.select c (S₁.π p).1 (S₂.π p).1, .select c (S₁.π p).2 (S₂.π p).2)
  size := fun o => .select c (S₁.size o) (S₂.size o)
  live := fun o => .select c (S₁.live o) (S₂.live o)
  data := fun o i => .select c (S₁.data o i) (S₂.data o i)
  next := .select c S₁.next S₂.next
  g := or' S₁.g S₂.g
  fl := S₂.fl
  uw := S₂.uw
  ub := S₂.ub

/-- Entering the `then` arm / a loop iteration. -/
def MSym.enter (S : MSym) (c : Expr 1) : MSym :=
  let S₀ := S.guardUb ((ubExpr c).subst S.σ)
  { S₀ with g := and' S₀.g (c.subst S.σ) }

/-- Entering the `else` arm / leaving a loop, carrying the violations
accumulated in `S₁`. -/
def MSym.leave (S S₁ : MSym) (c : Expr 1) : MSym :=
  let S₀ := S.guardUb ((ubExpr c).subst S.σ)
  { S₀ with g := and' S₀.g (not' (c.subst S.σ)), fl := S₁.fl, uw := S₁.uw, ub := S₁.ub }

def mencLoop (c : Expr 1) (E : MSym → MSym) : Nat → MSym → MSym
  | 0, S =>
    let S₀ := S.guardUb ((ubExpr c).subst S.σ)
    { S₀ with uw := or' S₀.uw (and' S₀.g (c.subst S.σ)), g := and' S₀.g (not' (c.subst S.σ)) }
  | n + 1, S =>
    let S₁ := mencLoop c E n (E (S.enter c))
    MSym.merge (c.subst S.σ) S₁ (S.leave S₁ c)

def menc (k : Nat) : MStmt → MSym → MSym
  | .skip, S => S
  | .assign (w := w) x e, S =>
    let S₁ := S.guardUb ((ubExpr e).subst S.σ)
    { S₁ with σ := S.σ.set w x (e.subst S.σ) }
  | .assert _ c, S =>
    let S₁ := S.guardUb ((ubCond c).subst S.σ)
    let c' := condExpr S c
    { S₁ with fl := or' S₁.fl (and' S₁.g (not' c')), g := and' S₁.g c' }
  | .assume c, S =>
    let S₁ := S.guardUb ((ubExpr c).subst S.σ)
    { S₁ with g := and' S₁.g (c.subst S.σ) }
  | .alloc p sz, S =>
    (S.guardUb ((ubExpr sz).subst S.σ)).allocS p (sz.subst S.σ)
  | .free p, S =>
    (S.guardUb (not' (freeOkExpr S p))).freeS p
  | .gep q p e, S =>
    let S₁ := S.guardUb ((ubExpr e).subst S.σ)
    { S₁ with π := upd S.π q (objOf S p, .bin .add {} (offOf S p) (e.subst S.σ)) }
  | .load x p, S =>
    let S₁ := S.guardUb (not' (okExpr S p))
    { S₁ with σ := S.σ.set 8 x (S.data (objOf S p) (offOf S p)) }
  | .store p v, S =>
    let S₀ := S.guardUb ((ubExpr v).subst S.σ)
    (S₀.guardUb (not' (okExpr S p))).storeS p (v.subst S.σ)
  | .seq s t, S => menc k t (menc k s S)
  | .ite c s t, S =>
    let S₁ := menc k s (S.enter c)
    MSym.merge (c.subst S.σ) S₁ (menc k t (S.leave S₁ c))
  | .loop c b, S => mencLoop c (menc k b) k S

def MSym.init : MSym where
  σ := Subst.id
  π := fun _ => (.const 0#64, .const 0#64)
  size := fun _ => .const 0#64
  live := fun _ => ff
  data := fun _ _ => .const 0#8
  next := .const 1#64
  g := tt
  fl := ff
  uw := ff
  ub := ff

def mencode (k : Nat) (p : MStmt) : MSym := menc k p MSym.init

/-! ### Well-formedness is preserved -/

theorem cong1_select {v : Nat} (c : Expr 64 → Expr 1) (f g : Expr 64 → Expr v)
    (hc : Cong1 c) (hf : Cong1 f) (hg : Cong1 g) : Cong1 (fun o => .select (c o) (f o) (g o)) := by
  intro ρ a b h
  simp only [evalE, hc ρ a b h, hf ρ a b h, hg ρ a b h]

theorem cong2_select (c : Expr 64 → Expr 64 → Expr 1) (f g : Expr 64 → Expr 64 → Expr 8)
    (hc : ∀ ρ a b x y, evalE ρ a = evalE ρ b → evalE ρ x = evalE ρ y → evalE ρ (c a x) = evalE ρ (c b y))
    (hf : Cong2 f) (hg : Cong2 g) : Cong2 (fun o i => .select (c o i) (f o i) (g o i)) := by
  intro ρ a b x y h h'
  simp only [evalE, hc ρ a b x y h h', hf ρ a b x y h h', hg ρ a b x y h h']

theorem cong1_const {v : Nat} (e : Expr v) : Cong1 (fun _ : Expr 64 => e) := fun _ _ _ _ => rfl

theorem cong1_eqE (n : Expr 64) : Cong1 (fun o => eqE o n) := by
  intro ρ a b h; simp only [eqE, evalE, h]

theorem MSym.WF.guardUb {S : MSym} (h : S.WF) (u : Expr 1) : (S.guardUb u).WF := h

theorem MSym.WF.allocS {S : MSym} (h : S.WF) (p : String) (sz : Expr 64) : (S.allocS p sz).WF :=
  ⟨cong1_select _ _ _ (cong1_eqE _) (cong1_const _) h.size,
   cong1_select _ _ _ (cong1_eqE _) (cong1_const _) h.live,
   cong2_select _ _ _ (fun ρ a b _ _ h _ => by simp only [eqE, evalE, h])
     (fun _ _ _ _ _ _ _ => rfl) h.data⟩

theorem MSym.WF.freeS {S : MSym} (h : S.WF) (p : String) : (S.freeS p).WF :=
  ⟨h.size, cong1_select _ _ _ (cong1_eqE _) (cong1_const _) h.live, h.data⟩

theorem MSym.WF.storeS {S : MSym} (h : S.WF) (p : String) (v : Expr 8) : (S.storeS p v).WF :=
  ⟨h.size, h.live,
   cong2_select _ _ _ (fun ρ a b x y h h' => by simp only [and', eqE, evalE, evalBin, h, h'])
     (fun _ _ _ _ _ _ _ => rfl) h.data⟩

theorem MSym.WF.merge {S₁ S₂ : MSym} (h₁ : S₁.WF) (h₂ : S₂.WF) (c : Expr 1) : (MSym.merge c S₁ S₂).WF :=
  ⟨cong1_select _ _ _ (cong1_const c) h₁.size h₂.size,
   cong1_select _ _ _ (cong1_const c) h₁.live h₂.live,
   cong2_select _ _ _ (fun _ _ _ _ _ _ _ => rfl) h₁.data h₂.data⟩

theorem mencLoop_wf (c : Expr 1) (E : MSym → MSym) (hE : ∀ S, S.WF → (E S).WF) :
    ∀ n S, S.WF → (mencLoop c E n S).WF := by
  intro n
  induction n with
  | zero => intro S h; exact h
  | succ n ih =>
    intro S h
    have h₁ := ih (E (S.enter c)) (hE _ (show (S.enter c).WF from h))
    exact MSym.WF.merge h₁ (show (S.leave _ c).WF from h) (c.subst S.σ)

theorem menc_wf (k : Nat) (s : MStmt) : ∀ S, S.WF → (menc k s S).WF := by
  induction s with
  | alloc p sz => intro S h; exact MSym.WF.allocS (S := S.guardUb _) h p (sz.subst S.σ)
  | free p => intro S h; exact MSym.WF.freeS (S := S.guardUb (not' (freeOkExpr S p))) h p
  | store p v =>
    intro S h
    exact MSym.WF.storeS (S := (S.guardUb ((ubExpr v).subst S.σ)).guardUb (not' (okExpr S p))) h p
      (v.subst S.σ)
  | seq s t ihs iht => intro S h; exact iht _ (ihs _ h)
  | ite c s t ihs iht =>
    intro S h
    exact MSym.WF.merge (ihs (S.enter c) h) (iht (S.leave (menc k s (S.enter c)) c) h) (c.subst S.σ)
  | loop c b ihb => intro S h; exact mencLoop_wf c _ ihb k S h
  | _ => intro S h; exact h

theorem MSym.init_wf : MSym.init.WF :=
  ⟨cong1_const _, cong1_const _, fun _ _ _ _ _ _ _ => rfl⟩

/-! ### Evaluation lemmas -/

theorem truth_ub_subst' (ρ₀ : Env) (S : MSym) {w : Nat} (e : Expr w) :
    truth (evalE ρ₀ ((ubExpr e).subst S.σ)) = ubE (S.toState ρ₀).ρ e :=
  truth_ub_subst ρ₀ S.σ e

theorem evalE_subst' (ρ₀ : Env) (S : MSym) {w : Nat} (e : Expr w) :
    evalE ρ₀ (e.subst S.σ) = evalE (S.toState ρ₀).ρ e :=
  evalE_subst ρ₀ S.σ e

theorem size_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (e : Expr 64) :
    evalE ρ₀ (S.size e) = ((S.toState ρ₀).heap (evalE ρ₀ e)).size :=
  h.size ρ₀ e (.const (evalE ρ₀ e)) rfl

theorem live_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (e : Expr 64) :
    truth (evalE ρ₀ (S.live e)) = ((S.toState ρ₀).heap (evalE ρ₀ e)).live := by
  show _ = truth (evalE ρ₀ (S.live (.const (evalE ρ₀ e))))
  rw [h.live ρ₀ e (.const (evalE ρ₀ e)) rfl]

theorem data_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (e i : Expr 64) :
    evalE ρ₀ (S.data e i) = ((S.toState ρ₀).heap (evalE ρ₀ e)).data (evalE ρ₀ i) :=
  h.data ρ₀ e (.const (evalE ρ₀ e)) i (.const (evalE ρ₀ i)) rfl rfl

theorem okExpr_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (p : String) :
    truth (evalE ρ₀ (okExpr S p)) = (liveC (S.toState ρ₀) p && inBoundsC (S.toState ρ₀) p) := by
  simp only [okExpr, truth_eval_and, liveC, inBoundsC, toState_obj, toState_off, objOf, offOf]
  rw [← live_eval h, ← size_eval h]
  simp [evalE, evalPred]

theorem freeOkExpr_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (p : String) :
    truth (evalE ρ₀ (freeOkExpr S p)) = (liveC (S.toState ρ₀) p && atBaseC (S.toState ρ₀) p) := by
  simp only [freeOkExpr, truth_eval_and, liveC, atBaseC, toState_obj, toState_off, objOf, offOf]
  rw [← live_eval h]
  simp [eqE, evalE, evalPred]

theorem condExpr_eval {S : MSym} (h : S.WF) (ρ₀ : Env) (c : MCond) :
    truth (evalE ρ₀ (condExpr S c)) = evalC (S.toState ρ₀) c := by
  cases c with
  | bv c => simp only [condExpr, evalC, evalE_subst']
  | live p =>
    simp only [condExpr, evalC, liveC, toState_obj, objOf]
    rw [← live_eval h]
  | inBounds p =>
    simp only [condExpr, evalC, inBoundsC, toState_obj, toState_off, objOf, offOf]
    rw [← size_eval h]
    simp [evalE, evalPred]
  | atBase p => simp [condExpr, evalC, atBaseC, eqE, evalE, evalPred, offOf]

theorem ubCond_eval (ρ₀ : Env) (S : MSym) (c : MCond) :
    truth (evalE ρ₀ ((ubCond c).subst S.σ)) = ubC (S.toState ρ₀) c := by
  cases c with
  | bv c => simp [ubCond, ubC, truth_ub_subst']
  | _ => simp [ubCond, ubC, Expr.subst, ff, evalE]

theorem guardUb_toState (ρ₀ : Env) (S : MSym) (u : Expr 1) :
    (S.guardUb u).toState ρ₀ = S.toState ρ₀ := rfl

theorem toState_merge (ρ₀ : Env) (c : Expr 1) (S₁ S₂ : MSym) :
    (MSym.merge c S₁ S₂).toState ρ₀ = if truth (evalE ρ₀ c) then S₁.toState ρ₀ else S₂.toState ρ₀ := by
  by_cases h : truth (evalE ρ₀ c)
  · simp only [h, ↓reduceIte, MSym.toState, MSym.merge, evalE, Subst.eval_merge]
  · simp only [h, Bool.false_eq_true, ↓reduceIte, MSym.toState, MSym.merge, evalE, Subst.eval_merge]

theorem toState_allocS (ρ₀ : Env) (S : MSym) (p : String) (sz : Expr 64) :
    (S.allocS p sz).toState ρ₀ = (S.toState ρ₀).allocObj p (evalE ρ₀ sz) := by
  simp only [MSym.toState, MSym.allocS, MState.allocObj, MState.mk.injEq, true_and]
  refine ⟨?_, ?_, ?_⟩
  · funext q; simp only [upd_apply]; split <;> rfl
  · funext n; simp only [upd_apply, eqE, evalE, evalPred]
    by_cases hn : n = evalE ρ₀ S.next <;> simp [hn]
  · rfl

theorem toState_freeS (ρ₀ : Env) (S : MSym) (p : String) :
    (S.freeS p).toState ρ₀ = (S.toState ρ₀).freeObj p := by
  simp only [MSym.toState, MSym.freeS, MState.freeObj, MState.mk.injEq, true_and, and_true]
  funext n; simp only [upd_apply, eqE, evalE, evalPred, objOf]
  by_cases hn : n = evalE ρ₀ (S.π p).1 <;> simp [hn, ff, evalE]

theorem toState_storeS (ρ₀ : Env) (S : MSym) (p : String) (v : Expr 8) :
    (S.storeS p v).toState ρ₀ = (S.toState ρ₀).storeByte p (evalE ρ₀ v) := by
  simp only [MSym.toState, MSym.storeS, MState.storeByte, MState.mk.injEq, true_and, and_true]
  funext n; simp only [upd_apply, and', eqE, evalE, evalPred, evalBin, objOf, offOf]
  by_cases hn : n = evalE ρ₀ (S.π p).1
  · simp only [hn, ↓reduceIte]
    congr 1
    funext i
    by_cases hi : i = evalE ρ₀ (S.π p).2 <;> simp [hi, upd_apply]
  · simp [hn]

/-! ### The specification and its composition -/

structure MSpec (ρ₀ : Env) (S S' : MSym) (o : MOutcome) : Prop where
  g : truth (evalE ρ₀ S'.g) = o.isNormal
  fl : truth (evalE ρ₀ S'.fl) = (truth (evalE ρ₀ S.fl) || o.isFail)
  uw : truth (evalE ρ₀ S'.uw) = (truth (evalE ρ₀ S.uw) || o.isUnwind)
  ub : truth (evalE ρ₀ S'.ub) = (truth (evalE ρ₀ S.ub) || o.isUb)
  st : ∀ st', o = .normal st' → S'.toState ρ₀ = st'

def mtarget (ρ₀ : Env) (S : MSym) (f : MState → MOutcome) : MOutcome :=
  if truth (evalE ρ₀ S.g) then f (S.toState ρ₀) else .blocked

theorem MSpec.bind {ρ₀ : Env} {S S₁ S₂ : MSym} {o₁ : MOutcome} {f : MState → MOutcome}
    (h₁ : MSpec ρ₀ S S₁ o₁) (h₂ : MSpec ρ₀ S₁ S₂ (mtarget ρ₀ S₁ f)) :
    MSpec ρ₀ S S₂ (o₁.bind f) := by
  cases o₁ with
  | normal st' =>
    have hg : truth (evalE ρ₀ S₁.g) = true := by simpa [MOutcome.isNormal] using h₁.g
    have he := h₁.st st' rfl
    simp only [mtarget, hg, he, ↓reduceIte] at h₂
    refine ⟨h₂.g, ?_, ?_, ?_, h₂.st⟩
    · rw [h₂.fl, h₁.fl]; simp [MOutcome.isFail]
    · rw [h₂.uw, h₁.uw]; simp [MOutcome.isUnwind]
    · rw [h₂.ub, h₁.ub]; simp [MOutcome.isUb]
  | _ =>
    have hg : truth (evalE ρ₀ S₁.g) = false := by simpa [MOutcome.isNormal] using h₁.g
    simp only [mtarget, hg, Bool.false_eq_true, ↓reduceIte] at h₂
    refine ⟨?_, ?_, ?_, ?_, ?_⟩
    · rw [h₂.g]; rfl
    · rw [h₂.fl, h₁.fl]; simp [MOutcome.isFail]
    · rw [h₂.uw, h₁.uw]; simp [MOutcome.isUnwind]
    · rw [h₂.ub, h₁.ub]; simp [MOutcome.isUb]
    · intro st' h; cases h

theorem mtarget_bind (ρ₀ : Env) (S : MSym) (f g : MState → MOutcome) :
    mtarget ρ₀ S (fun st => (f st).bind g) = (mtarget ρ₀ S f).bind g := by
  unfold mtarget; split <;> rfl

/-- Statements that guard on a UB condition `u` and otherwise make one
normal step to `next`. -/
theorem mspec_guarded (ρ₀ : Env) (S S' : MSym) (u : Expr 1) (next : MState)
    (hg : S'.g = (S.guardUb u).g) (hfl : S'.fl = S.fl) (huw : S'.uw = S.uw)
    (hub : S'.ub = (S.guardUb u).ub) (hst : S'.toState ρ₀ = next) :
    MSpec ρ₀ S S' (mtarget ρ₀ S (fun _ => if truth (evalE ρ₀ u) then .ub else .normal next)) := by
  unfold mtarget
  refine ⟨?_, ?_, ?_, ?_, ?_⟩
  · rw [hg]; simp only [MSym.guardUb, truth_eval_and, truth_eval_not]
    by_cases g : truth (evalE ρ₀ S.g) <;> by_cases b : truth (evalE ρ₀ u) <;>
      simp [g, b, MOutcome.isNormal]
  · rw [hfl]; by_cases g : truth (evalE ρ₀ S.g) <;> by_cases b : truth (evalE ρ₀ u) <;>
      simp [g, b, MOutcome.isFail]
  · rw [huw]; by_cases g : truth (evalE ρ₀ S.g) <;> by_cases b : truth (evalE ρ₀ u) <;>
      simp [g, b, MOutcome.isUnwind]
  · rw [hub]; simp only [MSym.guardUb, truth_eval_or, truth_eval_and]
    by_cases g : truth (evalE ρ₀ S.g) <;> by_cases b : truth (evalE ρ₀ u) <;>
      simp [g, b, MOutcome.isUb]
  · intro st' h
    by_cases g : truth (evalE ρ₀ S.g) <;> by_cases b : truth (evalE ρ₀ u) <;> simp [g, b] at h
    subst h; exact hst

theorem enter_eval (ρ₀ : Env) (S : MSym) (c : Expr 1) :
    (S.enter c).toState ρ₀ = S.toState ρ₀ ∧
    truth (evalE ρ₀ (S.enter c).g) =
      (truth (evalE ρ₀ S.g) && !ubE (S.toState ρ₀).ρ c && truth (evalE (S.toState ρ₀).ρ c)) ∧
    truth (evalE ρ₀ (S.enter c).ub) =
      (truth (evalE ρ₀ S.ub) || (truth (evalE ρ₀ S.g) && ubE (S.toState ρ₀).ρ c)) ∧
    (S.enter c).fl = S.fl ∧ (S.enter c).uw = S.uw := by
  refine ⟨rfl, ?_, ?_, rfl, rfl⟩
  · simp [MSym.enter, MSym.guardUb, truth_ub_subst', evalE_subst', ubExpr_correct, Bool.and_assoc]
  · simp [MSym.enter, MSym.guardUb, truth_ub_subst', ubExpr_correct]

theorem leave_eval (ρ₀ : Env) (S S₁ : MSym) (c : Expr 1) :
    (S.leave S₁ c).toState ρ₀ = S.toState ρ₀ ∧
    truth (evalE ρ₀ (S.leave S₁ c).g) =
      (truth (evalE ρ₀ S.g) && !ubE (S.toState ρ₀).ρ c && !truth (evalE (S.toState ρ₀).ρ c)) ∧
    (S.leave S₁ c).fl = S₁.fl ∧ (S.leave S₁ c).uw = S₁.uw ∧ (S.leave S₁ c).ub = S₁.ub := by
  refine ⟨rfl, ?_, rfl, rfl, rfl⟩
  simp [MSym.leave, MSym.guardUb, truth_ub_subst', evalE_subst', ubExpr_correct, Bool.and_assoc]

@[simp] theorem merge_g (c : Expr 1) (S₁ S₂ : MSym) : (MSym.merge c S₁ S₂).g = or' S₁.g S₂.g := rfl
@[simp] theorem merge_fl (c : Expr 1) (S₁ S₂ : MSym) : (MSym.merge c S₁ S₂).fl = S₂.fl := rfl
@[simp] theorem merge_uw (c : Expr 1) (S₁ S₂ : MSym) : (MSym.merge c S₁ S₂).uw = S₂.uw := rfl
@[simp] theorem merge_ub (c : Expr 1) (S₁ S₂ : MSym) : (MSym.merge c S₁ S₂).ub = S₂.ub := rfl

/-- Branch combination over arbitrary result states. -/
theorem mspec_ite (ρ₀ : Env) (S S₁ S₂ : MSym) (c : Expr 1) (f₁ f₂ : MState → MOutcome)
    (h₁ : MSpec ρ₀ (S.enter c) S₁ (mtarget ρ₀ (S.enter c) f₁))
    (h₂ : MSpec ρ₀ (S.leave S₁ c) S₂ (mtarget ρ₀ (S.leave S₁ c) f₂)) :
    MSpec ρ₀ S (MSym.merge (c.subst S.σ) S₁ S₂)
      (mtarget ρ₀ S (fun st => if ubE st.ρ c then .ub else if truth (evalE st.ρ c) then f₁ st else f₂ st)) := by
  obtain ⟨eS, eg, eub, efl, euw⟩ := enter_eval ρ₀ S c
  obtain ⟨lS, lg, lfl, luw, lub⟩ := leave_eval ρ₀ S S₁ c
  obtain ⟨Ag, Afl, Auw, Aub, Ast⟩ := h₁
  obtain ⟨Bg, Bfl, Buw, Bub, Bst⟩ := h₂
  simp only [mtarget, eS, eg, efl, euw, eub] at Ag Afl Auw Aub Ast
  simp only [mtarget, lS, lg, lfl, luw, lub] at Bg Bfl Buw Bub Bst
  have hc : truth (evalE ρ₀ (c.subst S.σ)) = truth (evalE (S.toState ρ₀).ρ c) := by
    rw [evalE_subst']
  unfold mtarget
  by_cases g : truth (evalE ρ₀ S.g) <;>
  by_cases u : ubE (S.toState ρ₀).ρ c <;>
  by_cases cv : truth (evalE (S.toState ρ₀).ρ c)
  all_goals
    simp only [g, u, cv, Bool.true_and, Bool.and_true, Bool.not_true, Bool.not_false,
      Bool.and_false, Bool.false_and, Bool.false_eq_true, Bool.true_or, Bool.or_true,
      Bool.false_or, Bool.or_false, ↓reduceIte] at Ag Afl Auw Aub Ast
    simp only [g, u, cv, Bool.true_and, Bool.and_true, Bool.not_true, Bool.not_false,
      Bool.and_false, Bool.false_and, Bool.false_eq_true, Bool.true_or, Bool.or_true,
      Bool.false_or, Bool.or_false, ↓reduceIte] at Bg Bfl Buw Bub Bst
    refine ⟨?_, ?_, ?_, ?_, ?_⟩
    all_goals
      simp_all [merge_g, merge_fl, merge_uw, merge_ub, toState_merge, MOutcome.isNormal,
        MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]

theorem mencLoop_spec (ρ₀ : Env) (c : Expr 1) (E : MSym → MSym) (B : MState → MOutcome)
    (hE : ∀ S, S.WF → MSpec ρ₀ S (E S) (mtarget ρ₀ S B)) (hEwf : ∀ S, S.WF → (E S).WF) :
    ∀ n S, S.WF → MSpec ρ₀ S (mencLoop c E n S) (mtarget ρ₀ S (mloopRun c B n)) := by
  intro n
  induction n with
  | zero =>
    intro S _
    unfold mtarget
    refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.toState ρ₀).ρ c <;>
    by_cases cv : truth (evalE (S.toState ρ₀).ρ c) <;>
    simp_all [mencLoop, mloopRun, MSym.guardUb, truth_ub_subst', evalE_subst', ubExpr_correct,
      MOutcome.isNormal, MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]
  | succ n ih =>
    intro S hwf
    have hEnt : (S.enter c).WF := hwf
    have hA : MSpec ρ₀ (S.enter c) (mencLoop c E n (E (S.enter c)))
        (mtarget ρ₀ (S.enter c) (fun st => (B st).bind (mloopRun c B n))) := by
      rw [mtarget_bind]
      exact MSpec.bind (hE _ hEnt) (ih _ (hEwf _ hEnt))
    let S₁ := mencLoop c E n (E (S.enter c))
    have hB : MSpec ρ₀ (S.leave S₁ c) (S.leave S₁ c)
        (mtarget ρ₀ (S.leave S₁ c) (fun st => .normal st)) := by
      unfold mtarget
      refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
      by_cases g : truth (evalE ρ₀ (S.leave S₁ c).g) <;>
      simp [g, MOutcome.isNormal, MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]
    have h := mspec_ite ρ₀ S S₁ (S.leave S₁ c) c _ _ hA hB
    have e : mtarget ρ₀ S (mloopRun c B (n + 1)) =
        mtarget ρ₀ S (fun st => if ubE st.ρ c then .ub else
          if truth (evalE st.ρ c) then (B st).bind (mloopRun c B n) else .normal st) := by
      rfl
    rw [e]
    exact h

theorem menc_spec (k : Nat) (ρ₀ : Env) (s : MStmt) :
    ∀ S, S.WF → MSpec ρ₀ S (menc k s S) (mtarget ρ₀ S (mrun k s)) := by
  induction s with
  | skip =>
    intro S _
    unfold mtarget
    refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
    by_cases g : truth (evalE ρ₀ S.g) <;>
    simp [menc, mrun, g, MOutcome.isNormal, MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]
  | assign x e =>
    intro S _
    have e1 : mtarget ρ₀ S (mrun k (.assign x e)) = mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ ((ubExpr e).subst S.σ)) then .ub else
        .normal ((S.toState ρ₀).setVar _ x (evalE (S.toState ρ₀).ρ e))) := by
      unfold mtarget; rw [truth_ub_subst']; rfl
    rw [e1]
    refine mspec_guarded ρ₀ S _ _ _ rfl rfl rfl rfl ?_
    simp [menc, MSym.toState, MSym.guardUb, MState.setVar, Subst.eval_set, evalE_subst]
  | assume c =>
    intro S _
    unfold mtarget
    refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubE (S.toState ρ₀).ρ c <;>
    by_cases cv : truth (evalE (S.toState ρ₀).ρ c) <;>
    simp_all [menc, mrun, MSym.guardUb, truth_ub_subst', evalE_subst', ubExpr_correct,
      MOutcome.isNormal, MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]
  | assert t c =>
    intro S hwf
    have hc := condExpr_eval hwf ρ₀ c
    have hu := ubCond_eval ρ₀ S c
    unfold mtarget
    refine ⟨?_, ?_, ?_, ?_, ?_⟩ <;>
    by_cases g : truth (evalE ρ₀ S.g) <;>
    by_cases u : ubC (S.toState ρ₀) c <;>
    by_cases cv : evalC (S.toState ρ₀) c <;>
    simp_all [menc, mrun, MSym.guardUb, guardUb_toState,
      MOutcome.isNormal, MOutcome.isFail, MOutcome.isUnwind, MOutcome.isUb]
  | alloc p sz =>
    intro S _
    have e1 : mtarget ρ₀ S (mrun k (.alloc p sz)) = mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ ((ubExpr sz).subst S.σ)) then .ub else
        .normal ((S.toState ρ₀).allocObj p (evalE (S.toState ρ₀).ρ sz))) := by
      unfold mtarget; rw [truth_ub_subst']; rfl
    rw [e1]
    refine mspec_guarded ρ₀ S _ _ _ rfl rfl rfl rfl ?_
    show ((S.guardUb _).allocS p (sz.subst S.σ)).toState ρ₀ = _
    rw [toState_allocS, guardUb_toState, evalE_subst']
  | free p =>
    intro S hwf
    have e1 : mtarget ρ₀ S (mrun k (.free p)) = mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ (not' (freeOkExpr S p))) then .ub else
        .normal ((S.toState ρ₀).freeObj p)) := by
      unfold mtarget
      rw [truth_eval_not, freeOkExpr_eval hwf]
      split
      · simp only [mrun]; split <;> simp_all
      · rfl
    rw [e1]
    refine mspec_guarded ρ₀ S _ _ _ rfl rfl rfl rfl ?_
    show ((S.guardUb _).freeS p).toState ρ₀ = _
    rw [toState_freeS, guardUb_toState]
  | gep q p e =>
    intro S _
    have e1 : mtarget ρ₀ S (mrun k (.gep q p e)) = mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ ((ubExpr e).subst S.σ)) then .ub else
        .normal ((S.toState ρ₀).setPtr q
          ⟨((S.toState ρ₀).π p).obj, ((S.toState ρ₀).π p).off + evalE (S.toState ρ₀).ρ e⟩)) := by
      unfold mtarget; rw [truth_ub_subst']; rfl
    rw [e1]
    refine mspec_guarded ρ₀ S _ _ _ rfl rfl rfl rfl ?_
    simp only [menc, MSym.toState, MSym.guardUb, MState.setPtr, MState.mk.injEq, true_and, and_true]
    funext r; simp only [upd_apply]; split <;> simp [evalE, evalBin, evalE_subst, objOf, offOf]
  | load x p =>
    intro S hwf
    have e1 : mtarget ρ₀ S (mrun k (.load x p)) = mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ (not' (okExpr S p))) then .ub else
        .normal ((S.toState ρ₀).setVar 8 x ((S.toState ρ₀).loadByte p))) := by
      unfold mtarget
      rw [truth_eval_not, okExpr_eval hwf]
      split
      · simp only [mrun]; split <;> simp_all
      · rfl
    rw [e1]
    refine mspec_guarded ρ₀ S _ _ _ rfl rfl rfl rfl ?_
    simp only [menc, MSym.toState, MSym.guardUb, MState.setVar, MState.loadByte, MState.mk.injEq,
      and_true]
    rw [Subst.eval_set]
    congr 1
    exact data_eval hwf ρ₀ _ _
  | store p v =>
    intro S hwf
    let S₀ := S.guardUb ((ubExpr v).subst S.σ)
    let f : MState → MOutcome := fun _ =>
      if truth (evalE ρ₀ (not' (okExpr S p))) then .ub else
      .normal ((S.toState ρ₀).storeByte p (evalE (S.toState ρ₀).ρ v))
    have h₁ : MSpec ρ₀ S S₀ (mtarget ρ₀ S (fun _ =>
        if truth (evalE ρ₀ ((ubExpr v).subst S.σ)) then .ub else .normal (S.toState ρ₀))) :=
      mspec_guarded ρ₀ S S₀ _ _ rfl rfl rfl rfl rfl
    have h₂ : MSpec ρ₀ S₀ (menc k (.store p v) S) (mtarget ρ₀ S₀ f) := by
      refine mspec_guarded ρ₀ S₀ _ _ _ rfl rfl rfl rfl ?_
      show ((S₀.guardUb _).storeS p (v.subst S.σ)).toState ρ₀ = _
      rw [toState_storeS, evalE_subst']; rfl
    have h := MSpec.bind h₁ h₂
    have e : mtarget ρ₀ S (mrun k (.store p v)) =
        (mtarget ρ₀ S (fun _ =>
          if truth (evalE ρ₀ ((ubExpr v).subst S.σ)) then .ub else .normal (S.toState ρ₀))).bind f := by
      unfold mtarget
      simp only [f, truth_eval_not, okExpr_eval hwf, truth_ub_subst']
      by_cases g : truth (evalE ρ₀ S.g) <;> by_cases u : ubE (S.toState ρ₀).ρ v <;>
        simp only [g, u, mrun, Bool.false_eq_true, ↓reduceIte, MOutcome.bind_ub,
          MOutcome.bind_normal, MOutcome.bind_blocked]
      split <;> simp_all
    rw [e]; exact h
  | seq s t ihs iht =>
    intro S hwf
    have h := MSpec.bind (ihs S hwf) (iht (menc k s S) (menc_wf k s S hwf))
    have e : mtarget ρ₀ S (mrun k (.seq s t)) = (mtarget ρ₀ S (mrun k s)).bind (mrun k t) := by
      rw [← mtarget_bind]; rfl
    rw [e]; exact h
  | ite c s t ihs iht =>
    intro S hwf
    have h₁ := ihs (S.enter c) hwf
    have h₂ := iht (S.leave (menc k s (S.enter c)) c) hwf
    exact mspec_ite ρ₀ S _ _ c (mrun k s) (mrun k t) h₁ h₂
  | loop c b ihb =>
    intro S hwf
    exact mencLoop_spec ρ₀ c (menc k b) (mrun k b) ihb (menc_wf k b) k S hwf

/-! ### Top-level theorems for programs with memory -/

theorem MSym.init_toState (ρ₀ : Env) : MSym.init.toState ρ₀ = MState.init ρ₀ := by
  simp only [MSym.toState, MSym.init, MState.init, deadObj, Subst.eval_id, evalE, ff, truth_zero]
  rfl

theorem mencode_spec (k : Nat) (p : MStmt) (ρ₀ : Env) :
    MSpec ρ₀ MSym.init (mencode k p) (mrun k p (MState.init ρ₀)) := by
  have h := menc_spec k ρ₀ p MSym.init MSym.init_wf
  have e : mtarget ρ₀ MSym.init (mrun k p) = mrun k p (MState.init ρ₀) := by
    unfold mtarget
    rw [MSym.init_toState]
    simp [MSym.init]
  rw [e] at h
  exact h

/-- **Exactness (memory): assertion failures.** -/
theorem mencode_fail_iff (k : Nat) (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mencode k p).fl) = true ↔ ∃ t, mrun k p (MState.init ρ₀) = .fail t := by
  rw [(mencode_spec k p ρ₀).fl]
  cases mrun k p (MState.init ρ₀) <;> simp [MSym.init, MOutcome.isFail]

/-- **Exactness (memory): UB, including every memory error.** -/
theorem mencode_ub_iff (k : Nat) (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mencode k p).ub) = true ↔ mrun k p (MState.init ρ₀) = .ub := by
  rw [(mencode_spec k p ρ₀).ub]
  cases mrun k p (MState.init ρ₀) <;> simp [MSym.init, MOutcome.isUb]

/-- **Exactness (memory): unwinding.** -/
theorem mencode_unwind_iff (k : Nat) (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mencode k p).uw) = true ↔ mrun k p (MState.init ρ₀) = .unwind := by
  rw [(mencode_spec k p ρ₀).uw]
  cases mrun k p (MState.init ρ₀) <;> simp [MSym.init, MOutcome.isUnwind]

def mvcBounded (k : Nat) (p : MStmt) : Expr 1 := or' (mencode k p).fl (mencode k p).ub
def mvcFull (k : Nat) (p : MStmt) : Expr 1 := or' (mvcBounded k p) (mencode k p).uw

theorem mvcBounded_iff (k : Nat) (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mvcBounded k p)) = true ↔
      (∃ t, mrun k p (MState.init ρ₀) = .fail t) ∨ mrun k p (MState.init ρ₀) = .ub := by
  simp only [mvcBounded, truth_eval_or, Bool.or_eq_true, mencode_fail_iff, mencode_ub_iff]

theorem mvcFull_iff (k : Nat) (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mvcFull k p)) = true ↔
      (∃ t, mrun k p (MState.init ρ₀) = .fail t) ∨ mrun k p (MState.init ρ₀) = .ub ∨
        mrun k p (MState.init ρ₀) = .unwind := by
  simp only [mvcFull, truth_eval_or, Bool.or_eq_true, mencode_unwind_iff]
  simp only [mvcBounded_iff, or_assoc]

/-- **Encoder soundness with memory, bounded (5.3, 8.2).**  If the bounded
VC is unsatisfiable, no input leads, within the bound, to an assertion
failure, arithmetic UB, null dereference, use after free, out-of-bounds
access, double free or invalid free. -/
theorem mbmc_sound (k : Nat) (p : MStmt) (h : ¬ Sat (mvcBounded k p)) :
    ∀ ρ₀, (∀ t, mrun k p (MState.init ρ₀) ≠ .fail t) ∧ mrun k p (MState.init ρ₀) ≠ .ub := by
  intro ρ₀
  have : ¬ ((∃ t, mrun k p (MState.init ρ₀) = .fail t) ∨ mrun k p (MState.init ρ₀) = .ub) :=
    fun hc => h ⟨ρ₀, (mvcBounded_iff k p ρ₀).2 hc⟩
  exact ⟨fun t ht => this (Or.inl ⟨t, ht⟩), fun hu => this (Or.inr hu)⟩

/-- **Completeness with memory:** every model of the bounded VC is a real
counterexample of the reference semantics. -/
theorem mbmc_complete (k : Nat) (p : MStmt) (ρ₀ : Env)
    (h : truth (evalE ρ₀ (mvcBounded k p)) = true) :
    (∃ t, MBigStep p (MState.init ρ₀) (.fail t)) ∨ MBigStep p (MState.init ρ₀) .ub := by
  rcases (mvcBounded_iff k p ρ₀).1 h with ⟨t, ht⟩ | hu
  · exact Or.inl ⟨t, mrun_sound k p _ _ ht (by simp)⟩
  · exact Or.inr (mrun_sound k p _ _ hu (by simp))

/-- **Soundness with memory and the unwinding assertion.**  If the full VC
is unsatisfiable, every input terminates and no execution of the unbounded
reference semantics fails an assertion or has UB (memory errors included). -/
theorem mbmc_sound_unbounded (k : Nat) (p : MStmt) (h : ¬ Sat (mvcFull k p)) :
    ∀ ρ₀, (∃ o, MBigStep p (MState.init ρ₀) o) ∧
      ∀ o, MBigStep p (MState.init ρ₀) o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub := by
  intro ρ₀
  have hn : ¬ ((∃ t, mrun k p (MState.init ρ₀) = .fail t) ∨ mrun k p (MState.init ρ₀) = .ub ∨
      mrun k p (MState.init ρ₀) = .unwind) :=
    fun hc => h ⟨ρ₀, (mvcFull_iff k p ρ₀).2 hc⟩
  have hnu : mrun k p (MState.init ρ₀) ≠ .unwind := fun hu => hn (Or.inr (Or.inr hu))
  have hb : MBigStep p (MState.init ρ₀) (mrun k p (MState.init ρ₀)) := mrun_sound k p _ _ rfl hnu
  refine ⟨⟨_, hb⟩, fun o ho => ?_⟩
  have := MBigStep.det ho hb
  subst this
  exact ⟨fun t ht => hn (Or.inl ⟨t, ht⟩), fun hu => hn (Or.inr (Or.inl hu))⟩

/-- Loop-free programs with memory never exhaust the bound. -/
def MStmt.LoopFree : MStmt → Prop
  | .seq s t => s.LoopFree ∧ t.LoopFree
  | .ite _ s t => s.LoopFree ∧ t.LoopFree
  | .loop .. => False
  | _ => True

theorem mrun_loopFree_ne_unwind (k : Nat) (p : MStmt) (hp : p.LoopFree) :
    ∀ st, mrun k p st ≠ .unwind := by
  induction p with
  | seq s t ihs iht =>
    intro st
    simp only [mrun]
    cases h : mrun k s st with
    | normal st' => exact iht hp.2 st'
    | unwind => exact absurd h (ihs hp.1 st)
    | _ => simp
  | ite c s t ihs iht =>
    intro st
    simp only [mrun]
    cases ubE st.ρ c <;> cases truth (evalE st.ρ c) <;> simp [ihs hp.1, iht hp.2]
  | loop => exact absurd hp id
  | _ => intro st; rw [mrun_atomic k _ rfl]; exact mrun_atomic_ne_unwind _ rfl st

/-- **Roadmap Part 5 exit criterion "5.3 complete for loop-free code with
memory":** for loop-free PIR programs with memory, the bounded VC is
unsatisfiable iff no execution of the reference semantics fails an
assertion, has arithmetic UB, or commits a memory error. -/
theorem mloopFree_encode_exact (k : Nat) (p : MStmt) (hp : p.LoopFree) :
    ¬ Sat (mvcBounded k p) ↔
      ∀ ρ₀ o, MBigStep p (MState.init ρ₀) o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub := by
  constructor
  · intro h ρ₀ o ho
    have hs := mbmc_sound k p h ρ₀
    have hr := mrun_sound k p (MState.init ρ₀) _ rfl (mrun_loopFree_ne_unwind k p hp _)
    have := MBigStep.det ho hr; subst this
    exact hs
  · rintro h ⟨ρ₀, hρ⟩
    rcases mbmc_complete k p ρ₀ hρ with ⟨t, ht⟩ | hu
    · exact (h ρ₀ _ ht).1 t rfl
    · exact (h ρ₀ _ hu).2 rfl

/-- Checking the memory-instrumented program: if the assertion formula of
`mencode k (minstr p)` is unsatisfiable, the original program has no UB
(memory errors included) and no failing user assertion within the bound. -/
theorem minstr_encode_sound (k : Nat) (p : MStmt) (h : ¬ Sat (mencode k (minstr p)).fl) :
    ∀ ρ₀, mrun k p (MState.init ρ₀) ≠ .ub ∧ ∀ t, mrun k p (MState.init ρ₀) ≠ .fail t := by
  intro ρ₀
  have hf : ∀ t, mrun k (minstr p) (MState.init ρ₀) ≠ .fail t := fun t ht =>
    h ⟨ρ₀, (mencode_fail_iff k (minstr p) ρ₀).2 ⟨t, ht⟩⟩
  rw [mrun_minstr] at hf
  refine ⟨fun hu => hf .ub (by rw [hu]; rfl), fun t ht => hf t (by rw [ht]; rfl)⟩

end PrismSem.Mem
