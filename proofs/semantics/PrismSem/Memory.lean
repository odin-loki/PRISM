/-
PRISM PIR — memory model, memory-safety instrumentation, and encoder
soundness for straight-line code with memory (roadmap Part 2.5, Part 5.3
layer "memory", Part 8.2 row "Memory model").

Model (following roadmap 2.5, simplified):
* a pointer is a pair (object id, byte offset); object ids are `Nat`, the
  offset is a `BitVec 64`; object `0` is the null object and is never live;
* every object has a size (`BitVec 64`), a liveness bit and byte contents
  (`BitVec 64 → BitVec 8`); fresh objects are zero-filled;
* statements: `alloc p n`, `free p`, `gep q p e` (q := p + e),
  `load x p` (x : i8), `store p v` (v : i8), plus assign/assert/assume/seq.

Memory UB (the outcome `ub`):
* `load`/`store` through a pointer whose object is not live (null
  dereference, use after free) or whose offset is not `< size` (out of
  bounds, including negative offsets, which wrap to large unsigned values);
* `free` of a pointer whose object is not live (double free, free of null
  or of a never-allocated object) or whose offset is not 0 (invalid free).

The encoder layer here covers straight-line code only: object ids are then
static at encode time, sizes, offsets and contents are symbolic, and object
contents are encoded as read-over-write `select` chains (the array theory
eliminated, as in an Ackermannised / fixed-size array encoding).
-/
import PrismSem.Encode

namespace PrismSem.Mem

/-! ### Concrete memory -/

structure Ptr where
  obj : Nat
  off : BitVec 64

structure Obj where
  size : BitVec 64
  live : Bool
  data : BitVec 64 → BitVec 8

structure MState where
  ρ : Env
  π : String → Ptr
  heap : Nat → Obj
  next : Nat

def upd {α : Type} {β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) : α → β :=
  fun a' => if a' = a then b else f a'

def deadObj : Obj := ⟨0, false, fun _ => 0⟩

/-- Initial state for inputs `ρ₀`: no object is allocated, all pointer
variables are null. -/
def MState.init (ρ₀ : Env) : MState :=
  { ρ := ρ₀, π := fun _ => ⟨0, 0⟩, heap := fun _ => deadObj, next := 1 }

/-- Conditions that may be asserted. -/
inductive MCond where
  | bv (c : Expr 1)
  /-- the object `p` points into is live -/
  | live (p : String)
  /-- `p`'s offset is within its object's size -/
  | inBounds (p : String)
  /-- `p` points to the start of its object -/
  | atBase (p : String)

inductive MStmt where
  | skip
  | assign {w : Nat} (x : String) (e : Expr w)
  | assert (t : Tag) (c : MCond)
  | assume (c : Expr 1)
  | alloc (p : String) (size : Expr 64)
  | free (p : String)
  | gep (q p : String) (off : Expr 64)
  | load (x : String) (p : String)
  | store (p : String) (v : Expr 8)
  | seq (s t : MStmt)

inductive MOutcome where
  | normal (st : MState)
  | fail (t : Tag)
  | blocked
  | ub

def MOutcome.bind : MOutcome → (MState → MOutcome) → MOutcome
  | .normal st, k => k st
  | o, _ => o

def MOutcome.isNormal : MOutcome → Bool
  | .normal _ => true
  | _ => false

def MOutcome.isFail : MOutcome → Bool
  | .fail _ => true
  | _ => false

def MOutcome.isUb : MOutcome → Bool
  | .ub => true
  | _ => false

def liveC (st : MState) (p : String) : Bool := (st.heap (st.π p).obj).live
def inBoundsC (st : MState) (p : String) : Bool := (st.π p).off.ult (st.heap (st.π p).obj).size
def atBaseC (st : MState) (p : String) : Bool := (st.π p).off == 0#64

def evalC (st : MState) : MCond → Bool
  | .bv c => truth (evalE st.ρ c)
  | .live p => liveC st p
  | .inBounds p => inBoundsC st p
  | .atBase p => atBaseC st p

def ubC (st : MState) : MCond → Bool
  | .bv c => ubE st.ρ c
  | _ => false

/-- Mark the object `p` points into as freed. -/
def MState.freeObj (st : MState) (p : String) : MState :=
  let n := (st.π p).obj
  let o : Obj := { st.heap n with live := false }
  { st with heap := upd st.heap n o }

/-- Write byte `b` at the address `p`. -/
def MState.storeByte (st : MState) (p : String) (b : BitVec 8) : MState :=
  let n := (st.π p).obj
  let o : Obj := { st.heap n with data := upd (st.heap n).data (st.π p).off b }
  { st with heap := upd st.heap n o }

/-- Concrete semantics of straight-line memory programs. -/
def mrun : MStmt → MState → MOutcome
  | .skip, st => .normal st
  | .assign (w := w) x e, st =>
    if ubE st.ρ e then .ub else .normal { st with ρ := st.ρ.set w x (evalE st.ρ e) }
  | .assert t c, st =>
    if ubC st c then .ub else if evalC st c then .normal st else .fail t
  | .assume c, st =>
    if ubE st.ρ c then .ub else if truth (evalE st.ρ c) then .normal st else .blocked
  | .alloc p sz, st =>
    if ubE st.ρ sz then .ub else
    let o : Obj := ⟨evalE st.ρ sz, true, fun _ => 0⟩
    .normal { st with heap := upd st.heap st.next o, π := upd st.π p ⟨st.next, 0⟩, next := st.next + 1 }
  | .free p, st =>
    if liveC st p && atBaseC st p then .normal (st.freeObj p) else .ub
  | .gep q p e, st =>
    if ubE st.ρ e then .ub else
    .normal { st with π := upd st.π q ⟨(st.π p).obj, (st.π p).off + evalE st.ρ e⟩ }
  | .load x p, st =>
    if liveC st p && inBoundsC st p then
      .normal { st with ρ := st.ρ.set 8 x ((st.heap (st.π p).obj).data (st.π p).off) }
    else .ub
  | .store p v, st =>
    if ubE st.ρ v then .ub else
    if liveC st p && inBoundsC st p then .normal (st.storeByte p (evalE st.ρ v)) else .ub
  | .seq s t, st => (mrun s st).bind (mrun t)

/-! ### Memory-safety instrumentation -/

def mchk {w : Nat} (e : Expr w) : MStmt := .assert .ub (.bv (Expr.not' (ubExpr e)))

def minstr : MStmt → MStmt
  | .skip => .skip
  | .assign x e => .seq (mchk e) (.assign x e)
  | .assert t (.bv c) => .seq (mchk c) (.assert t (.bv c))
  | .assert t c => .assert t c
  | .assume c => .seq (mchk c) (.assume c)
  | .alloc p sz => .seq (mchk sz) (.alloc p sz)
  | .free p => .seq (.assert .ub (.live p)) (.seq (.assert .ub (.atBase p)) (.free p))
  | .gep q p e => .seq (mchk e) (.gep q p e)
  | .load x p => .seq (.assert .ub (.live p)) (.seq (.assert .ub (.inBounds p)) (.load x p))
  | .store p v =>
    .seq (mchk v) (.seq (.assert .ub (.live p)) (.seq (.assert .ub (.inBounds p)) (.store p v)))
  | .seq s t => .seq (minstr s) (minstr t)

def mapUbM : MOutcome → MOutcome
  | .ub => .fail .ub
  | o => o

@[simp] theorem mapUbM_normal (st : MState) : mapUbM (.normal st) = .normal st := rfl
@[simp] theorem mapUbM_fail (t : Tag) : mapUbM (.fail t) = .fail t := rfl
@[simp] theorem mapUbM_blocked : mapUbM .blocked = .blocked := rfl
@[simp] theorem mapUbM_ub : mapUbM .ub = .fail .ub := rfl
@[simp] theorem MOutcome.bind_normal (st : MState) (k : MState → MOutcome) :
    (MOutcome.normal st).bind k = k st := rfl
@[simp] theorem MOutcome.bind_fail (t : Tag) (k : MState → MOutcome) :
    (MOutcome.fail t).bind k = .fail t := rfl
@[simp] theorem MOutcome.bind_blocked (k : MState → MOutcome) : MOutcome.blocked.bind k = .blocked := rfl
@[simp] theorem MOutcome.bind_ub (k : MState → MOutcome) : MOutcome.ub.bind k = .ub := rfl

theorem mrun_mchk {w : Nat} (e : Expr w) (st : MState) :
    mrun (mchk e) st = if ubE st.ρ e then .fail .ub else .normal st := by
  have h1 : ubE st.ρ (Expr.not' (ubExpr e)) = false := by
    simp [Expr.not', Expr.tt, ubE, ubBin, ubE_ubExpr]
  have h2 : truth (evalE st.ρ (Expr.not' (ubExpr e))) = !ubE st.ρ e := by
    simp [Expr.not', Expr.tt, evalE, evalBin, ubExpr_correct]
  simp only [mchk, mrun, ubC, evalC, h1, h2]
  cases ubE st.ρ e <;> simp

/-- **Memory instrumentation theorem (exact form).**  The instrumented
program behaves as the original, except that every UB — arithmetic or
memory (null dereference, use after free, out of bounds, double/invalid
free) — becomes a failure of an inserted `.ub` assertion. -/
theorem mrun_minstr (s : MStmt) : ∀ st, mrun (minstr s) st = mapUbM (mrun s st) := by
  induction s with
  | skip => intro st; rfl
  | assign x e =>
    intro st; simp only [minstr, mrun, mrun_mchk]
    by_cases hu : ubE st.ρ e <;> simp [hu]
  | assert t c =>
    intro st
    cases c with
    | bv c =>
      simp only [minstr, mrun, mrun_mchk, ubC, evalC]
      by_cases hu : ubE st.ρ c <;> by_cases hc : truth (evalE st.ρ c) <;> simp [hu, hc]
    | _ =>
      have k : ∀ (b : Bool), (if b = true then MOutcome.normal st else MOutcome.fail t) =
          mapUbM (if b = true then MOutcome.normal st else MOutcome.fail t) := by
        intro b; cases b <;> rfl
      simp only [minstr, mrun, ubC, evalC, Bool.false_eq_true, ↓reduceIte]
      exact k _
  | assume c =>
    intro st; simp only [minstr, mrun, mrun_mchk]
    by_cases hu : ubE st.ρ c <;> by_cases hc : truth (evalE st.ρ c) <;> simp [hu, hc]
  | alloc p sz =>
    intro st; simp only [minstr, mrun, mrun_mchk]
    by_cases hu : ubE st.ρ sz <;> simp [hu]
  | free p =>
    intro st; simp only [minstr, mrun, ubC, evalC]
    by_cases hl : liveC st p <;> by_cases hb : atBaseC st p <;> simp [hl, hb]
  | gep q p e =>
    intro st; simp only [minstr, mrun, mrun_mchk]
    by_cases hu : ubE st.ρ e <;> simp [hu]
  | load x p =>
    intro st; simp only [minstr, mrun, ubC, evalC]
    by_cases hl : liveC st p <;> by_cases hb : inBoundsC st p <;> simp [hl, hb]
  | store p v =>
    intro st; simp only [minstr, mrun, mrun_mchk, ubC, evalC]
    by_cases hu : ubE st.ρ v <;> by_cases hl : liveC st p <;> by_cases hb : inBoundsC st p <;>
      simp [hu, hl, hb]
  | seq s t ihs iht =>
    intro st
    show (mrun (minstr s) st).bind (mrun (minstr t)) = mapUbM ((mrun s st).bind (mrun t))
    rw [ihs]
    cases mrun s st <;> simp [iht]

def MStmt.NoUbTags : MStmt → Prop
  | .assert t _ => t = .user
  | .seq s t => s.NoUbTags ∧ t.NoUbTags
  | _ => True

theorem mrun_ne_fail_ub (s : MStmt) (hs : s.NoUbTags) : ∀ st, mrun s st ≠ .fail .ub := by
  induction s with
  | assert t c =>
    intro st
    simp only [MStmt.NoUbTags] at hs; subst hs
    simp only [mrun]
    split
    · simp
    · split <;> simp
  | seq s t ihs iht =>
    intro st
    simp only [mrun]
    cases h : mrun s st with
    | normal st' => exact iht hs.2 st'
    | fail t => have := ihs hs.1 st; rw [h] at this; simpa using this
    | _ => simp
  | _ => intro st; simp only [mrun]; repeat' split
         all_goals simp

/-- **Memory-safety instrumentation (8.2 "Property instrumentation",
memory part).**  An inserted assertion fails iff the original program
executes an operation with UB (including every memory error). -/
theorem minstr_fail_ub_iff (s : MStmt) (hs : s.NoUbTags) (st : MState) :
    mrun (minstr s) st = .fail .ub ↔ mrun s st = .ub := by
  rw [mrun_minstr]
  have := mrun_ne_fail_ub s hs st
  cases h : mrun s st <;> simp_all

/-! ### Symbolic memory and the encoder (straight-line) -/

/-- Symbolic object: static liveness, symbolic size, contents as a
read-over-write function from a symbolic offset to a symbolic byte. -/
structure SObj where
  size : Expr 64
  live : Bool
  data : Expr 64 → Expr 8

structure MSym where
  σ : Subst
  π : String → Nat × Expr 64
  heap : Nat → SObj
  next : Nat
  g : Expr 1
  fl : Expr 1
  ub : Expr 1

def SObj.toObj (ρ₀ : Env) (o : SObj) : Obj :=
  ⟨evalE ρ₀ o.size, o.live, fun i => evalE ρ₀ (o.data (.const i))⟩

/-- The concrete state a symbolic state denotes at inputs `ρ₀`. -/
def MSym.toState (S : MSym) (ρ₀ : Env) : MState :=
  { ρ := S.σ.eval ρ₀,
    π := fun p => ⟨(S.π p).1, evalE ρ₀ (S.π p).2⟩,
    heap := fun n => (S.heap n).toObj ρ₀,
    next := S.next }

/-- Symbolic contents respect evaluation (true of every read-over-write
chain the encoder builds). -/
def DataWF (d : Expr 64 → Expr 8) : Prop :=
  ∀ ρ₀ o₁ o₂, evalE ρ₀ o₁ = evalE ρ₀ o₂ → evalE ρ₀ (d o₁) = evalE ρ₀ (d o₂)

def MSym.WF (S : MSym) : Prop := ∀ n, DataWF (S.heap n).data

open Expr in
def okExpr (S : MSym) (p : String) : Expr 1 :=
  and' (if (S.heap (S.π p).1).live then tt else ff) (.icmp .ult (S.π p).2 (S.heap (S.π p).1).size)

open Expr in
def freeOkExpr (S : MSym) (p : String) : Expr 1 :=
  and' (if (S.heap (S.π p).1).live then tt else ff) (.icmp .eq (S.π p).2 (.const 0#64))

open Expr in
def condExpr (S : MSym) : MCond → Expr 1
  | .bv c => c.subst S.σ
  | .live p => if (S.heap (S.π p).1).live then tt else ff
  | .inBounds p => .icmp .ult (S.π p).2 (S.heap (S.π p).1).size
  | .atBase p => .icmp .eq (S.π p).2 (.const 0#64)

def ubCond : MCond → Expr 1
  | .bv c => ubExpr c
  | _ => Expr.ff

open Expr in
/-- Record UB condition `u` (already over the inputs) and continue where it
is false. -/
def MSym.guardUb (S : MSym) (u : Expr 1) : MSym :=
  { S with ub := or' S.ub (and' S.g u), g := and' S.g (not' u) }

open Expr in
def menc : MStmt → MSym → MSym
  | .skip, S => S
  | .assign (w := w) x e, S =>
    let S₁ := S.guardUb ((ubExpr e).subst S.σ)
    { S₁ with σ := S₁.σ.set w x (e.subst S₁.σ) }
  | .assert _ c, S =>
    let S₁ := S.guardUb ((ubCond c).subst S.σ)
    let c' := condExpr S₁ c
    { S₁ with fl := or' S₁.fl (and' S₁.g (not' c')), g := and' S₁.g c' }
  | .assume c, S =>
    let S₁ := S.guardUb ((ubExpr c).subst S.σ)
    { S₁ with g := and' S₁.g (c.subst S₁.σ) }
  | .alloc p sz, S =>
    let S₁ := S.guardUb ((ubExpr sz).subst S.σ)
    let ob : SObj := ⟨sz.subst S₁.σ, true, fun _ => .const 0#8⟩
    { S₁ with heap := upd S₁.heap S₁.next ob, π := upd S₁.π p (S₁.next, .const 0#64), next := S₁.next + 1 }
  | .free p, S =>
    let S₁ := S.guardUb (not' (freeOkExpr S p))
    let ob' : SObj := { S₁.heap (S₁.π p).1 with live := false }
    { S₁ with heap := upd S₁.heap (S₁.π p).1 ob' }
  | .gep q p e, S =>
    let S₁ := S.guardUb ((ubExpr e).subst S.σ)
    { S₁ with π := upd S₁.π q ((S₁.π p).1, .bin .add {} (S₁.π p).2 (e.subst S₁.σ)) }
  | .load x p, S =>
    let S₁ := S.guardUb (not' (okExpr S p))
    { S₁ with σ := S₁.σ.set 8 x ((S₁.heap (S₁.π p).1).data (S₁.π p).2) }
  | .store p v, S =>
    let S₀ := S.guardUb ((ubExpr v).subst S.σ)
    let S₁ := S₀.guardUb (not' (okExpr S₀ p))
    let o := (S₁.π p).2
    let ob := S₁.heap (S₁.π p).1
    let ob' : SObj := { ob with data := fun o' => .select (.icmp .eq o' o) (v.subst S₁.σ) (ob.data o') }
    { S₁ with heap := upd S₁.heap (S₁.π p).1 ob' }
  | .seq s t, S => menc t (menc s S)

def MSym.init : MSym :=
  { σ := Subst.id, π := fun _ => (0, .const 0#64),
    heap := fun _ => ⟨.const 0#64, false, fun _ => .const 0#8⟩, next := 1,
    g := Expr.tt, fl := Expr.ff, ub := Expr.ff }

def mencode (p : MStmt) : MSym := menc p MSym.init

/-! ### Soundness of the memory encoding -/

structure MSpec (ρ₀ : Env) (S S' : MSym) (o : MOutcome) : Prop where
  g : truth (evalE ρ₀ S'.g) = o.isNormal
  fl : truth (evalE ρ₀ S'.fl) = (truth (evalE ρ₀ S.fl) || o.isFail)
  ub : truth (evalE ρ₀ S'.ub) = (truth (evalE ρ₀ S.ub) || o.isUb)
  st : ∀ st', o = .normal st' → S'.toState ρ₀ = st'
  wf : S'.WF

def mtarget (ρ₀ : Env) (S : MSym) (s : MStmt) : MOutcome :=
  if truth (evalE ρ₀ S.g) then mrun s (S.toState ρ₀) else .blocked

theorem upd_apply {α β : Type} [DecidableEq α] (f : α → β) (a a' : α) (b : β) :
    upd f a b a' = if a' = a then b else f a' := rfl

theorem guardUb_eval (ρ₀ : Env) (S : MSym) (u : Expr 1) :
    (S.guardUb u).σ = S.σ ∧ (S.guardUb u).π = S.π ∧ (S.guardUb u).heap = S.heap ∧
    (S.guardUb u).next = S.next ∧ (S.guardUb u).fl = S.fl ∧
    truth (evalE ρ₀ (S.guardUb u).g) = (truth (evalE ρ₀ S.g) && !truth (evalE ρ₀ u)) ∧
    truth (evalE ρ₀ (S.guardUb u).ub) = (truth (evalE ρ₀ S.ub) || (truth (evalE ρ₀ S.g) && truth (evalE ρ₀ u))) := by
  simp [MSym.guardUb]

theorem guardUb_toState (ρ₀ : Env) (S : MSym) (u : Expr 1) :
    (S.guardUb u).toState ρ₀ = S.toState ρ₀ := rfl

theorem guardUb_wf (S : MSym) (u : Expr 1) (h : S.WF) : (S.guardUb u).WF := h

theorem okExpr_eval (ρ₀ : Env) (S : MSym) (p : String) :
    truth (evalE ρ₀ (okExpr S p)) = (liveC (S.toState ρ₀) p && inBoundsC (S.toState ρ₀) p) := by
  unfold okExpr liveC inBoundsC MSym.toState SObj.toObj
  by_cases h : (S.heap (S.π p).1).live <;> simp [h, evalE, evalPred]

theorem freeOkExpr_eval (ρ₀ : Env) (S : MSym) (p : String) :
    truth (evalE ρ₀ (freeOkExpr S p)) = (liveC (S.toState ρ₀) p && atBaseC (S.toState ρ₀) p) := by
  unfold freeOkExpr liveC atBaseC MSym.toState SObj.toObj
  by_cases h : (S.heap (S.π p).1).live <;> simp [h, evalE, evalPred]

theorem condExpr_eval (ρ₀ : Env) (S : MSym) (c : MCond) :
    truth (evalE ρ₀ (condExpr S c)) = evalC (S.toState ρ₀) c := by
  cases c with
  | bv c => simp [condExpr, evalC, evalE_subst, MSym.toState]
  | live p =>
    unfold condExpr evalC liveC MSym.toState SObj.toObj
    by_cases h : (S.heap (S.π p).1).live <;> simp [h]
  | inBounds p => simp [condExpr, evalC, inBoundsC, MSym.toState, SObj.toObj, evalE, evalPred]
  | atBase p => simp [condExpr, evalC, atBaseC, MSym.toState, evalE, evalPred]

theorem ubCond_eval (ρ₀ : Env) (S : MSym) (c : MCond) :
    truth (evalE ρ₀ ((ubCond c).subst S.σ)) = ubC (S.toState ρ₀) c := by
  cases c with
  | bv c => simp [ubCond, ubC, truth_ub_subst, MSym.toState]
  | _ => simp [ubCond, ubC, Expr.subst, Expr.ff, evalE]

theorem truth_ub_subst' (ρ₀ : Env) (S : MSym) {w : Nat} (e : Expr w) :
    truth (evalE ρ₀ ((ubExpr e).subst S.σ)) = ubE (S.toState ρ₀).ρ e := by
  simp [truth_ub_subst, MSym.toState]

theorem evalE_subst' (ρ₀ : Env) (S : MSym) {w : Nat} (e : Expr w) :
    evalE ρ₀ (e.subst S.σ) = evalE (S.toState ρ₀).ρ e := by
  simp [evalE_subst, MSym.toState]

/-- A generic lemma for statements that first guard on a UB condition `u`
and then either stop (`ub`) or make a normal step. -/
theorem mspec_guarded (ρ₀ : Env) (S S' : MSym) (u : Expr 1) (bad : Bool) (next : MState)
    (hu : truth (evalE ρ₀ u) = bad)
    (hwf : S'.WF)
    (hg : S'.g = (S.guardUb u).g) (hfl : S'.fl = S.fl) (hub : S'.ub = (S.guardUb u).ub)
    (hst : S'.toState ρ₀ = next) :
    MSpec ρ₀ S S' (if truth (evalE ρ₀ S.g) then (if bad then .ub else .normal next) else .blocked) := by
  obtain ⟨-, -, -, -, -, eg, eub⟩ := guardUb_eval ρ₀ S u
  refine ⟨?_, ?_, ?_, ?_, hwf⟩
  · rw [hg, eg, hu]; by_cases g : truth (evalE ρ₀ S.g) <;> cases bad <;> simp [g, MOutcome.isNormal]
  · rw [hfl]; by_cases g : truth (evalE ρ₀ S.g) <;> cases bad <;> simp [g, MOutcome.isFail]
  · rw [hub, eub, hu]; by_cases g : truth (evalE ρ₀ S.g) <;> cases bad <;> simp [g, MOutcome.isUb]
  · intro st' h
    by_cases g : truth (evalE ρ₀ S.g) <;> cases bad <;> simp [g] at h
    subst h; exact hst

theorem MSpec.bind {ρ₀ : Env} {S S₁ S₂ : MSym} {o₁ : MOutcome} {t : MStmt}
    (h₁ : MSpec ρ₀ S S₁ o₁) (h₂ : MSpec ρ₀ S₁ S₂ (mtarget ρ₀ S₁ t)) :
    MSpec ρ₀ S S₂ (o₁.bind (mrun t)) := by
  cases o₁ with
  | normal st' =>
    have hg : truth (evalE ρ₀ S₁.g) = true := by simpa [MOutcome.isNormal] using h₁.g
    have he := h₁.st st' rfl
    simp only [mtarget, hg, he, ↓reduceIte] at h₂
    refine ⟨h₂.g, ?_, ?_, h₂.st, h₂.wf⟩
    · rw [h₂.fl, h₁.fl]; simp [MOutcome.isFail]
    · rw [h₂.ub, h₁.ub]; simp [MOutcome.isUb]
  | _ =>
    have hg : truth (evalE ρ₀ S₁.g) = false := by simpa [MOutcome.isNormal] using h₁.g
    simp only [mtarget, hg, Bool.false_eq_true, ↓reduceIte] at h₂
    refine ⟨?_, ?_, ?_, ?_, h₂.wf⟩
    · rw [h₂.g]; rfl
    · rw [h₂.fl, h₁.fl]; simp [MOutcome.isFail]
    · rw [h₂.ub, h₁.ub]; simp [MOutcome.isUb]
    · intro st' h; cases h

theorem heap_toObj_upd (ρ₀ : Env) (h : Nat → SObj) (n : Nat) (o : SObj) :
    (fun m => (upd h n o m).toObj ρ₀) = upd (fun m => (h m).toObj ρ₀) n (o.toObj ρ₀) := by
  funext m; simp only [upd_apply]; split <;> rfl

theorem wf_upd (h : Nat → SObj) (n : Nat) (o : SObj)
    (hh : ∀ m, DataWF (h m).data) (ho : DataWF o.data) : ∀ m, DataWF (upd h n o m).data := by
  intro m; simp only [upd_apply]; split
  · exact ho
  · exact hh m

theorem menc_spec (ρ₀ : Env) (s : MStmt) :
    ∀ S, S.WF → MSpec ρ₀ S (menc s S) (mtarget ρ₀ S s) := by
  induction s with
  | skip =>
    intro S hwf
    refine ⟨?_, ?_, ?_, ?_, hwf⟩ <;>
      by_cases g : truth (evalE ρ₀ S.g) <;>
      simp [menc, mtarget, mrun, g, MOutcome.isNormal, MOutcome.isFail, MOutcome.isUb]
  | assign x e =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    rw [← truth_ub_subst' ρ₀ S e]
    refine mspec_guarded ρ₀ S (menc (.assign x e) S) ((ubExpr e).subst S.σ) _ _ rfl hwf rfl rfl rfl ?_
    simp [menc, MSym.toState, MSym.guardUb, Subst.eval_set, evalE_subst]
  | assume c =>
    intro S hwf
    obtain ⟨hσ, hπ, hh, hn, hfl, hg, hub⟩ := guardUb_eval ρ₀ S ((ubExpr c).subst S.σ)
    have htoS : (menc (.assume c) S).toState ρ₀ = S.toState ρ₀ := rfl
    refine ⟨?_, ?_, ?_, ?_, hwf⟩
    rotate_left 3
    · intro st' h
      rw [htoS]
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : ubE (S.toState ρ₀).ρ c <;>
      by_cases cv : truth (evalE (S.toState ρ₀).ρ c) <;>
      simp_all [mtarget, mrun]
    all_goals
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : ubE (S.toState ρ₀).ρ c <;>
      by_cases cv : truth (evalE (S.toState ρ₀).ρ c) <;>
      simp_all [menc, mtarget, mrun, truth_ub_subst', evalE_subst', ubExpr_correct,
        MOutcome.isNormal, MOutcome.isFail, MOutcome.isUb]
  | assert t c =>
    intro S hwf
    obtain ⟨hσ, hπ, hh, hn, hfl, hg, hub⟩ := guardUb_eval ρ₀ S ((ubCond c).subst S.σ)
    have hc := condExpr_eval ρ₀ (S.guardUb ((ubCond c).subst S.σ)) c
    rw [guardUb_toState] at hc
    have hu := ubCond_eval ρ₀ S c
    have htoS : (menc (.assert t c) S).toState ρ₀ = S.toState ρ₀ := rfl
    refine ⟨?_, ?_, ?_, ?_, hwf⟩
    rotate_left 3
    · intro st' h
      rw [htoS]
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : ubC (S.toState ρ₀) c <;>
      by_cases cv : evalC (S.toState ρ₀) c <;>
      simp_all [mtarget, mrun]
    all_goals
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : ubC (S.toState ρ₀) c <;>
      by_cases cv : evalC (S.toState ρ₀) c <;>
      simp_all [menc, mtarget, mrun, MOutcome.isNormal, MOutcome.isFail, MOutcome.isUb]
  | alloc p sz =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    rw [← truth_ub_subst' ρ₀ S sz]
    refine mspec_guarded ρ₀ S (menc (.alloc p sz) S) ((ubExpr sz).subst S.σ) _ _ rfl ?_ rfl rfl rfl ?_
    · exact wf_upd _ _ _ hwf (fun _ _ _ _ => rfl)
    · simp only [menc, MSym.toState, MSym.guardUb]
      rw [heap_toObj_upd]
      simp only [SObj.toObj, evalE, evalE_subst, MState.mk.injEq, true_and]
      constructor
      · funext q; simp only [upd_apply]; split <;> rfl
      · exact ⟨rfl, trivial⟩
  | free p =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    have e : (liveC (S.toState ρ₀) p && atBaseC (S.toState ρ₀) p) =
        !truth (evalE ρ₀ (Expr.not' (freeOkExpr S p))) := by
      rw [truth_eval_not, freeOkExpr_eval]; simp
    rw [e]
    have key : ∀ (b : Bool) (A : MOutcome), (if (!b) = true then A else MOutcome.ub) =
        (if b then .ub else A) := by intro b A; cases b <;> rfl
    rw [key]
    refine mspec_guarded ρ₀ S (menc (.free p) S) (Expr.not' (freeOkExpr S p)) _ _ rfl ?_ rfl rfl rfl ?_
    · exact wf_upd _ _ _ hwf (hwf _)
    · simp only [menc, MSym.toState, MSym.guardUb, MState.freeObj]
      rw [heap_toObj_upd]
      rfl
  | gep q p e =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    rw [← truth_ub_subst' ρ₀ S e]
    refine mspec_guarded ρ₀ S (menc (.gep q p e) S) ((ubExpr e).subst S.σ) _ _ rfl hwf rfl rfl rfl ?_
    simp only [menc, MSym.toState, MSym.guardUb, MState.mk.injEq, true_and]
    refine ⟨?_, trivial⟩
    funext r; simp only [upd_apply]; split <;> simp [evalE, evalBin, evalE_subst]
  | load x p =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    have e : (liveC (S.toState ρ₀) p && inBoundsC (S.toState ρ₀) p) =
        !truth (evalE ρ₀ (Expr.not' (okExpr S p))) := by
      rw [truth_eval_not, okExpr_eval]; simp
    rw [e]
    have key : ∀ (b : Bool) (A : MOutcome), (if (!b) = true then A else MOutcome.ub) =
        (if b then .ub else A) := by intro b A; cases b <;> rfl
    rw [key]
    refine mspec_guarded ρ₀ S (menc (.load x p) S) (Expr.not' (okExpr S p)) _ _ rfl hwf rfl rfl rfl ?_
    simp only [menc, MSym.toState, MSym.guardUb, SObj.toObj, MState.mk.injEq, and_true]
    rw [Subst.eval_set]
    congr 1
    apply hwf
    simp [evalE]
  | store p v =>
    intro S hwf
    unfold mtarget
    simp only [mrun]
    let S₀ := S.guardUb ((ubExpr v).subst S.σ)
    have hS₀ : S₀.toState ρ₀ = S.toState ρ₀ := rfl
    have e : (liveC (S.toState ρ₀) p && inBoundsC (S.toState ρ₀) p) =
        !truth (evalE ρ₀ (Expr.not' (okExpr S₀ p))) := by
      rw [truth_eval_not, okExpr_eval, hS₀]; simp
    rw [e, ← truth_ub_subst' ρ₀ S v]
    obtain ⟨-, -, -, -, -, eg0, eub0⟩ := guardUb_eval ρ₀ S ((ubExpr v).subst S.σ)
    obtain ⟨-, -, -, -, -, eg1, eub1⟩ := guardUb_eval ρ₀ S₀ (Expr.not' (okExpr S₀ p))
    have hdata : DataWF (fun o' => Expr.select (.icmp .eq o' (S.π p).2) (v.subst S.σ)
        ((S.heap (S.π p).1).data o')) := by
      intro ρ o₁ o₂ h
      simp only [evalE, evalPred, h, hwf _ ρ o₁ o₂ h]; rfl
    have hst : (menc (.store p v) S).toState ρ₀ =
        (S.toState ρ₀).storeByte p (evalE (S.toState ρ₀).ρ v) := by
      simp only [menc, MSym.toState, MSym.guardUb, MState.storeByte]
      rw [heap_toObj_upd]
      simp only [SObj.toObj, MState.mk.injEq, true_and, and_true]
      congr 2
      funext i
      simp only [upd_apply, evalE, evalPred, evalE_subst]
      by_cases hi : i = evalE ρ₀ (S.π p).2 <;> simp [hi]
    have hwf' : (menc (.store p v) S).WF := wf_upd _ _ _ hwf hdata
    refine ⟨?_, ?_, ?_, ?_, hwf'⟩
    · show truth (evalE ρ₀ (S₀.guardUb (Expr.not' (okExpr S₀ p))).g) = _
      rw [eg1, eg0]
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : truth (evalE ρ₀ ((ubExpr v).subst S.σ)) <;>
      by_cases k : truth (evalE ρ₀ (Expr.not' (okExpr S₀ p))) <;>
      simp [g, u, k, MOutcome.isNormal]
    · show truth (evalE ρ₀ S.fl) = _
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : truth (evalE ρ₀ ((ubExpr v).subst S.σ)) <;>
      by_cases k : truth (evalE ρ₀ (Expr.not' (okExpr S₀ p))) <;>
      simp [g, u, k, MOutcome.isFail]
    · show truth (evalE ρ₀ (S₀.guardUb (Expr.not' (okExpr S₀ p))).ub) = _
      rw [eub1, eg0, eub0]
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : truth (evalE ρ₀ ((ubExpr v).subst S.σ)) <;>
      by_cases k : truth (evalE ρ₀ (Expr.not' (okExpr S₀ p))) <;>
      simp [g, u, k, MOutcome.isUb]
    · intro st' h
      by_cases g : truth (evalE ρ₀ S.g) <;>
      by_cases u : truth (evalE ρ₀ ((ubExpr v).subst S.σ)) <;>
      by_cases k : truth (evalE ρ₀ (Expr.not' (okExpr S₀ p))) <;>
      simp [g, u, k] at h
      subst h; exact hst
  | seq s t ihs iht =>
    intro S hwf
    have h1 := ihs S hwf
    have h2 := iht (menc s S) h1.wf
    have h := MSpec.bind h1 h2
    have e : mtarget ρ₀ S (.seq s t) = (mtarget ρ₀ S s).bind (mrun t) := by
      unfold mtarget; split <;> rfl
    rw [e]; exact h

theorem MSym.init_toState (ρ₀ : Env) : MSym.init.toState ρ₀ = MState.init ρ₀ := rfl

theorem MSym.init_wf : MSym.init.WF := fun _ _ _ _ _ => rfl

theorem mencode_spec (p : MStmt) (ρ₀ : Env) :
    MSpec ρ₀ MSym.init (mencode p) (mrun p (MState.init ρ₀)) := by
  have h := menc_spec ρ₀ p MSym.init MSym.init_wf
  unfold mtarget at h
  unfold mencode
  simpa [MSym.init, truth_eval_tt, ← MSym.init_toState] using h

/-- **Exactness of the memory encoder: assertion failures.** -/
theorem mencode_fail_iff (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mencode p).fl) = true ↔ ∃ t, mrun p (MState.init ρ₀) = .fail t := by
  rw [(mencode_spec p ρ₀).fl]
  cases mrun p (MState.init ρ₀) <;> simp [MSym.init, MOutcome.isFail]

/-- **Exactness of the memory encoder: UB, including every memory error.** -/
theorem mencode_ub_iff (p : MStmt) (ρ₀ : Env) :
    truth (evalE ρ₀ (mencode p).ub) = true ↔ mrun p (MState.init ρ₀) = .ub := by
  rw [(mencode_spec p ρ₀).ub]
  cases mrun p (MState.init ρ₀) <;> simp [MSym.init, MOutcome.isUb]

/-- **Encoder soundness with memory (straight-line).**  If `fl ∨ ub` is
unsatisfiable, no input leads to an assertion failure, arithmetic UB, null
dereference, use after free, out-of-bounds access, double free or invalid
free. -/
theorem mbmc_sound (p : MStmt) (h : ¬ Sat (Expr.or' (mencode p).fl (mencode p).ub)) :
    ∀ ρ₀, (∀ t, mrun p (MState.init ρ₀) ≠ .fail t) ∧ mrun p (MState.init ρ₀) ≠ .ub := by
  intro ρ₀
  refine ⟨fun t ht => h ⟨ρ₀, ?_⟩, fun hu => h ⟨ρ₀, ?_⟩⟩
  · simp only [truth_eval_or, Bool.or_eq_true]; exact Or.inl ((mencode_fail_iff p ρ₀).2 ⟨t, ht⟩)
  · simp only [truth_eval_or, Bool.or_eq_true]; exact Or.inr ((mencode_ub_iff p ρ₀).2 hu)

/-- **Completeness with memory (straight-line):** every model is a real
counterexample. -/
theorem mbmc_complete (p : MStmt) (ρ₀ : Env)
    (h : truth (evalE ρ₀ (Expr.or' (mencode p).fl (mencode p).ub)) = true) :
    (∃ t, mrun p (MState.init ρ₀) = .fail t) ∨ mrun p (MState.init ρ₀) = .ub := by
  simp only [truth_eval_or, Bool.or_eq_true] at h
  rcases h with h | h
  · exact Or.inl ((mencode_fail_iff p ρ₀).1 h)
  · exact Or.inr ((mencode_ub_iff p ρ₀).1 h)

/-! ### The classic memory errors are UB in this model (sanity checks) -/

/-- Use after free. -/
theorem uaf_is_ub (ρ₀ : Env) :
    mrun (.seq (.alloc "p" (.const 4#64)) (.seq (.free "p") (.load "x" "p"))) (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, MState.freeObj, upd, liveC, atBaseC, inBoundsC, ubE, evalE, MOutcome.bind]

/-- Double free. -/
theorem double_free_is_ub (ρ₀ : Env) :
    mrun (.seq (.alloc "p" (.const 4#64)) (.seq (.free "p") (.free "p"))) (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, MState.freeObj, upd, liveC, atBaseC, ubE, evalE, MOutcome.bind]

/-- Out of bounds (one past the end). -/
theorem oob_is_ub (ρ₀ : Env) :
    mrun (.seq (.alloc "p" (.const 4#64)) (.seq (.gep "q" "p" (.const 4#64)) (.load "x" "q")))
      (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, upd, liveC, inBoundsC, ubE, evalE, MOutcome.bind]

/-- Null dereference. -/
theorem null_deref_is_ub (ρ₀ : Env) : mrun (.load "x" "p") (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, liveC, deadObj]

/-- In-bounds access to a live object is fine, and reads back what was
stored. -/
theorem store_load_ok (ρ₀ : Env) :
    ∃ st, mrun (.seq (.alloc "p" (.const 4#64)) (.seq (.gep "q" "p" (.const 3#64))
        (.seq (.store "q" (.const 7#8)) (.load "x" "q")))) (MState.init ρ₀) = .normal st ∧
      st.ρ 8 "x" = 7#8 := by
  refine ⟨_, rfl, ?_⟩
  simp [mrun, MState.init, MState.storeByte, upd, liveC, inBoundsC, ubE, evalE, MOutcome.bind, Env.set]

end PrismSem.Mem
