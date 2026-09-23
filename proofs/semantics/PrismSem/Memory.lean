/-
PRISM PIR — memory model, memory-safety instrumentation, and encoder
soundness for programs with memory, branches and bounded loops (roadmap
Part 2.5, Part 5.3 layer "memory", Part 8.2 rows "Memory model" and
"Property instrumentation").

Model (roadmap 2.5, simplified — see docs/PROOFS_SEMANTICS.md for the gap):
* a pointer is a pair (object id, byte offset), both `BitVec 64`; object `0`
  is the null object and is never allocated;
* every object has a size (`BitVec 64`), a liveness bit and byte contents
  (`BitVec 64 → BitVec 8`); fresh objects are zero-filled; allocation takes
  the next object id (`next`, starting at 1);
* statements: `alloc p n`, `free p`, `gep q p e` (q := p + e),
  `load x p` (x : i8), `store p v` (v : i8), plus assign/assert/assume/seq,
  if/else and while (bounded as in `PrismSem.run`).

Memory UB (the outcome `ub`):
* `load`/`store` through a pointer whose object is not live (null
  dereference, use after free, wild pointer) or whose offset is not `< size`
  (out of bounds, including negative offsets, which wrap to large unsigned);
* `free` of a pointer whose object is not live (double free, free of null
  or of a never-allocated object) or whose offset is not 0 (invalid free).

The symbolic memory is fully symbolic (object ids included): sizes,
liveness and contents are read-over-write `select` chains indexed by
symbolic object ids and offsets — the SMT array theory eliminated, as in an
Ackermannised fixed-size array encoding.
-/
import PrismSem.Encode

-- Proof scripts share simp sets across `<;>` branches; unused-argument noise is expected.
set_option linter.unusedSimpArgs false

namespace PrismSem.Mem

/-! ### Concrete memory -/

structure Ptr where
  obj : BitVec 64
  off : BitVec 64

structure Obj where
  size : BitVec 64
  live : Bool
  data : BitVec 64 → BitVec 8

structure MState where
  ρ : Env
  π : String → Ptr
  heap : BitVec 64 → Obj
  next : BitVec 64

def upd {α : Type} {β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) : α → β :=
  fun a' => if a' = a then b else f a'

theorem upd_apply {α β : Type} [DecidableEq α] (f : α → β) (a a' : α) (b : β) :
    upd f a b a' = if a' = a then b else f a' := rfl

def deadObj : Obj := ⟨0, false, fun _ => 0⟩

/-- Initial state for inputs `ρ₀`: nothing allocated, every pointer
variable null, next object id 1. -/
def MState.init (ρ₀ : Env) : MState :=
  { ρ := ρ₀, π := fun _ => ⟨0, 0⟩, heap := fun _ => deadObj, next := 1 }

def MState.setVar (st : MState) (w : Nat) (x : String) (v : BitVec w) : MState :=
  { st with ρ := st.ρ.set w x v }

def MState.setPtr (st : MState) (q : String) (v : Ptr) : MState :=
  { st with π := upd st.π q v }

def MState.allocObj (st : MState) (p : String) (sz : BitVec 64) : MState :=
  { st with heap := upd st.heap st.next ⟨sz, true, fun _ => 0⟩,
            π := upd st.π p ⟨st.next, 0⟩, next := st.next + 1 }

def MState.freeObj (st : MState) (p : String) : MState :=
  let n := (st.π p).obj
  let o : Obj := { st.heap n with live := false }
  { st with heap := upd st.heap n o }

def MState.storeByte (st : MState) (p : String) (b : BitVec 8) : MState :=
  let n := (st.π p).obj
  let o : Obj := { st.heap n with data := upd (st.heap n).data (st.π p).off b }
  { st with heap := upd st.heap n o }

def MState.loadByte (st : MState) (p : String) : BitVec 8 :=
  (st.heap (st.π p).obj).data (st.π p).off

/-- Conditions that may be asserted. -/
inductive MCond where
  | bv (c : Expr 1)
  /-- the object `p` points into is live -/
  | live (p : String)
  /-- `p`'s offset is below its object's size -/
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
  | ite (c : Expr 1) (s t : MStmt)
  | loop (c : Expr 1) (body : MStmt)

inductive MOutcome where
  | normal (st : MState)
  | fail (t : Tag)
  | blocked
  | unwind
  | ub

namespace MOutcome

def bind : MOutcome → (MState → MOutcome) → MOutcome
  | .normal st, k => k st
  | o, _ => o

def isNormal : MOutcome → Bool
  | .normal _ => true
  | _ => false

def isFail : MOutcome → Bool
  | .fail _ => true
  | _ => false

def isUnwind : MOutcome → Bool
  | .unwind => true
  | _ => false

def isUb : MOutcome → Bool
  | .ub => true
  | _ => false

@[simp] theorem bind_normal (st : MState) (k : MState → MOutcome) : (normal st).bind k = k st := rfl
@[simp] theorem bind_fail (t : Tag) (k : MState → MOutcome) : (fail t).bind k = .fail t := rfl
@[simp] theorem bind_blocked (k : MState → MOutcome) : blocked.bind k = .blocked := rfl
@[simp] theorem bind_unwind (k : MState → MOutcome) : unwind.bind k = .unwind := rfl
@[simp] theorem bind_ub (k : MState → MOutcome) : ub.bind k = .ub := rfl

end MOutcome

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

def mloopRun (c : Expr 1) (B : MState → MOutcome) : Nat → MState → MOutcome
  | 0, st =>
    if ubE st.ρ c then .ub else if truth (evalE st.ρ c) then .unwind else .normal st
  | n + 1, st =>
    if ubE st.ρ c then .ub else
    if truth (evalE st.ρ c) then (B st).bind (mloopRun c B n) else .normal st

/-- Bounded concrete semantics of PIR with memory (bound `k` per loop
entry, as `PrismSem.run`). -/
def mrun (k : Nat) : MStmt → MState → MOutcome
  | .skip, st => .normal st
  | .assign (w := w) x e, st =>
    if ubE st.ρ e then .ub else .normal (st.setVar w x (evalE st.ρ e))
  | .assert t c, st =>
    if ubC st c then .ub else if evalC st c then .normal st else .fail t
  | .assume c, st =>
    if ubE st.ρ c then .ub else if truth (evalE st.ρ c) then .normal st else .blocked
  | .alloc p sz, st =>
    if ubE st.ρ sz then .ub else .normal (st.allocObj p (evalE st.ρ sz))
  | .free p, st =>
    if liveC st p && atBaseC st p then .normal (st.freeObj p) else .ub
  | .gep q p e, st =>
    if ubE st.ρ e then .ub else .normal (st.setPtr q ⟨(st.π p).obj, (st.π p).off + evalE st.ρ e⟩)
  | .load x p, st =>
    if liveC st p && inBoundsC st p then .normal (st.setVar 8 x (st.loadByte p)) else .ub
  | .store p v, st =>
    if ubE st.ρ v then .ub else
    if liveC st p && inBoundsC st p then .normal (st.storeByte p (evalE st.ρ v)) else .ub
  | .seq s t, st => (mrun k s st).bind (mrun k t)
  | .ite c s t, st =>
    if ubE st.ρ c then .ub else if truth (evalE st.ρ c) then mrun k s st else mrun k t st
  | .loop c b, st => mloopRun c (mrun k b) k st

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
  | .ite c s t => .seq (mchk c) (.ite c (minstr s) (minstr t))
  | .loop c b => .seq (mchk c) (.loop c (.seq (minstr b) (mchk c)))

def mapUbM : MOutcome → MOutcome
  | .ub => .fail .ub
  | o => o

@[simp] theorem mapUbM_normal (st : MState) : mapUbM (.normal st) = .normal st := rfl
@[simp] theorem mapUbM_fail (t : Tag) : mapUbM (.fail t) = .fail t := rfl
@[simp] theorem mapUbM_blocked : mapUbM .blocked = .blocked := rfl
@[simp] theorem mapUbM_unwind : mapUbM .unwind = .unwind := rfl
@[simp] theorem mapUbM_ub : mapUbM .ub = .fail .ub := rfl

theorem mrun_mchk (k : Nat) {w : Nat} (e : Expr w) (st : MState) :
    mrun k (mchk e) st = if ubE st.ρ e then .fail .ub else .normal st := by
  have h1 : ubE st.ρ (Expr.not' (ubExpr e)) = false := by
    simp [Expr.not', Expr.tt, ubE, ubBin, ubE_ubExpr]
  have h2 : truth (evalE st.ρ (Expr.not' (ubExpr e))) = !ubE st.ρ e := by
    simp [Expr.not', Expr.tt, evalE, evalBin, ubExpr_correct]
  simp only [mchk, mrun, ubC, evalC, h1, h2]
  cases ubE st.ρ e <;> simp

theorem mloopRun_ub (c : Expr 1) (B : MState → MOutcome) (n : Nat) (st : MState)
    (h : ubE st.ρ c = true) : mloopRun c B n st = .ub := by
  cases n <;> simp [mloopRun, h]

theorem mloopRun_minstr (k : Nat) (c : Expr 1) (b : MStmt)
    (ihb : ∀ st, mrun k (minstr b) st = mapUbM (mrun k b st)) :
    ∀ n st, ubE st.ρ c = false →
      mloopRun c (mrun k (.seq (minstr b) (mchk c))) n st = mapUbM (mloopRun c (mrun k b) n st) := by
  intro n
  induction n with
  | zero =>
    intro st hu
    by_cases hc : truth (evalE st.ρ c) <;> simp [mloopRun, hu, hc]
  | succ n ih =>
    intro st hu
    by_cases hc : truth (evalE st.ρ c)
    · have e1 : mloopRun c (mrun k (.seq (minstr b) (mchk c))) (n + 1) st
          = (mrun k (.seq (minstr b) (mchk c)) st).bind
              (mloopRun c (mrun k (.seq (minstr b) (mchk c))) n) := by
        simp [mloopRun, hu, hc]
      have e2 : mloopRun c (mrun k b) (n + 1) st = (mrun k b st).bind (mloopRun c (mrun k b) n) := by
        simp [mloopRun, hu, hc]
      have e3 : mrun k (.seq (minstr b) (mchk c)) st = (mapUbM (mrun k b st)).bind (mrun k (mchk c)) := by
        show (mrun k (minstr b) st).bind (mrun k (mchk c)) = _
        rw [ihb]
      rw [e1, e2, e3]
      cases hb : mrun k b st with
      | normal st' =>
        simp only [mapUbM_normal, MOutcome.bind_normal]
        rw [mrun_mchk]
        by_cases hu' : ubE st'.ρ c
        · simp [hu', mloopRun_ub c _ n st' hu']
        · simp only [hu', Bool.false_eq_true, ↓reduceIte, MOutcome.bind_normal]
          exact ih st' (by simpa using hu')
      | _ => simp
    · simp [mloopRun, hu, hc]

/-- **Memory instrumentation theorem (exact form).**  The instrumented
program behaves as the original, except that every UB — arithmetic or
memory (null dereference, use after free, out of bounds, double/invalid
free) — becomes the failure of an inserted `.ub` assertion. -/
theorem mrun_minstr (k : Nat) (s : MStmt) : ∀ st, mrun k (minstr s) st = mapUbM (mrun k s st) := by
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
    show (mrun k (minstr s) st).bind (mrun k (minstr t)) = mapUbM ((mrun k s st).bind (mrun k t))
    rw [ihs]
    cases mrun k s st <;> simp [iht]
  | ite c s t ihs iht =>
    intro st
    show (mrun k (mchk c) st).bind (mrun k (.ite c (minstr s) (minstr t))) = mapUbM (mrun k (.ite c s t) st)
    rw [mrun_mchk]
    by_cases hu : ubE st.ρ c
    · simp [hu, mrun]
    · by_cases hc : truth (evalE st.ρ c) <;> simp [hu, hc, mrun, ihs, iht]
  | loop c b ihb =>
    intro st
    show (mrun k (mchk c) st).bind (mrun k (.loop c (.seq (minstr b) (mchk c)))) =
      mapUbM (mloopRun c (mrun k b) k st)
    rw [mrun_mchk]
    by_cases hu : ubE st.ρ c
    · simp [hu, mloopRun_ub c _ k st hu]
    · simp only [hu, Bool.false_eq_true, ↓reduceIte, MOutcome.bind_normal]
      exact mloopRun_minstr k c b ihb k st (by simpa using hu)

def MStmt.NoUbTags : MStmt → Prop
  | .assert t _ => t = .user
  | .seq s t => s.NoUbTags ∧ t.NoUbTags
  | .ite _ s t => s.NoUbTags ∧ t.NoUbTags
  | .loop _ b => b.NoUbTags
  | _ => True

theorem mrun_ne_fail_ub (k : Nat) (s : MStmt) (hs : s.NoUbTags) : ∀ st, mrun k s st ≠ .fail .ub := by
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
    cases h : mrun k s st with
    | normal st' => exact iht hs.2 st'
    | fail t => have := ihs hs.1 st; rw [h] at this; simpa using this
    | _ => simp
  | ite c s t ihs iht =>
    intro st
    simp only [mrun]
    cases ubE st.ρ c <;> cases truth (evalE st.ρ c) <;> simp [ihs hs.1, iht hs.2]
  | loop c b ihb =>
    intro st
    simp only [mrun]
    have key : ∀ n st, mloopRun c (mrun k b) n st ≠ .fail .ub := by
      intro n
      induction n with
      | zero => intro st; simp only [mloopRun]; cases ubE st.ρ c <;> cases truth (evalE st.ρ c) <;> simp
      | succ n ih =>
        intro st
        simp only [mloopRun]
        cases ubE st.ρ c <;> cases truth (evalE st.ρ c) <;> simp
        cases h : mrun k b st with
        | normal st' => exact ih st'
        | fail t => have := ihb hs st; rw [h] at this; simpa using this
        | _ => simp
    exact key k st
  | _ => intro st; simp only [mrun]; repeat' split
         all_goals simp

/-- **Memory-safety instrumentation (8.2 "Property instrumentation",
memory part).**  An inserted assertion fails iff the original program
executes an operation with UB (every memory error included), within the
bound `k`. -/
theorem minstr_fail_ub_iff (k : Nat) (s : MStmt) (hs : s.NoUbTags) (st : MState) :
    mrun k (minstr s) st = .fail .ub ↔ mrun k s st = .ub := by
  rw [mrun_minstr]
  have := mrun_ne_fail_ub k s hs st
  cases h : mrun k s st <;> simp_all

/-! ### Reference (unbounded) semantics with memory -/

/-- Statements without sub-statements. -/
def MStmt.atomic : MStmt → Bool
  | .seq .. | .ite .. | .loop .. => false
  | _ => true

/-- Big-step reference semantics.  Atomic statements take their meaning
from `mrun` (which does not depend on the bound for them, `mrun_atomic`);
loops iterate without bound. -/
inductive MBigStep : MStmt → MState → MOutcome → Prop where
  | atom {s st} : s.atomic = true → MBigStep s st (mrun 0 s st)
  | seq_normal {s t st st' o} :
      MBigStep s st (.normal st') → MBigStep t st' o → MBigStep (.seq s t) st o
  | seq_abrupt {s t st o} :
      MBigStep s st o → o.isNormal = false → MBigStep (.seq s t) st o
  | ite_ub {c s t st} : ubE st.ρ c = true → MBigStep (.ite c s t) st .ub
  | ite_true {c s t st o} :
      ubE st.ρ c = false → truth (evalE st.ρ c) = true → MBigStep s st o → MBigStep (.ite c s t) st o
  | ite_false {c s t st o} :
      ubE st.ρ c = false → truth (evalE st.ρ c) = false → MBigStep t st o → MBigStep (.ite c s t) st o
  | loop_ub {c b st} : ubE st.ρ c = true → MBigStep (.loop c b) st .ub
  | loop_exit {c b st} :
      ubE st.ρ c = false → truth (evalE st.ρ c) = false → MBigStep (.loop c b) st (.normal st)
  | loop_normal {c b st st' o} :
      ubE st.ρ c = false → truth (evalE st.ρ c) = true →
      MBigStep b st (.normal st') → MBigStep (.loop c b) st' o → MBigStep (.loop c b) st o
  | loop_abrupt {c b st o} :
      ubE st.ρ c = false → truth (evalE st.ρ c) = true →
      MBigStep b st o → o.isNormal = false → MBigStep (.loop c b) st o

theorem mrun_atomic (k : Nat) (s : MStmt) (h : s.atomic = true) (st : MState) :
    mrun k s st = mrun 0 s st := by
  cases s <;> simp_all [MStmt.atomic, mrun]

theorem mrun_atomic_ne_unwind (s : MStmt) (h : s.atomic = true) (st : MState) :
    mrun 0 s st ≠ .unwind := by
  cases s <;> simp only [MStmt.atomic, Bool.false_eq_true] at h <;> simp only [mrun] <;>
    repeat' split
  all_goals simp

theorem MBigStep.not_unwind {s st o} (h : MBigStep s st o) : o ≠ .unwind := by
  induction h with
  | atom ha => exact mrun_atomic_ne_unwind _ ha _
  | seq_normal _ _ _ ih2 => exact ih2
  | seq_abrupt _ _ ih => exact ih
  | ite_true _ _ _ ih => exact ih
  | ite_false _ _ _ ih => exact ih
  | loop_normal _ _ _ _ _ ih2 => exact ih2
  | loop_abrupt _ _ _ _ ih => exact ih
  | _ => simp

theorem MBigStep.det {s st o₁ o₂} (h₁ : MBigStep s st o₁) (h₂ : MBigStep s st o₂) : o₁ = o₂ := by
  induction h₁ generalizing o₂ with
  | atom ha =>
    cases h₂ <;> first | rfl | simp [MStmt.atomic] at ha
  | seq_normal _ _ ih1 ih2 =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | seq_normal ha hb => cases ih1 ha; exact ih2 hb
    | seq_abrupt ha hn => have := ih1 ha; subst this; simp [MOutcome.isNormal] at hn
  | seq_abrupt _ hn ih =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | seq_normal ha _ => have := ih ha; subst this; simp [MOutcome.isNormal] at hn
    | seq_abrupt ha _ => exact ih ha
  | loop_normal hu hc _ _ ih1 ih2 =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | loop_ub h => simp_all
    | loop_exit _ h => simp_all
    | loop_normal _ _ ha hb => cases ih1 ha; exact ih2 hb
    | loop_abrupt _ _ ha hn => have := ih1 ha; subst this; simp [MOutcome.isNormal] at hn
  | loop_abrupt hu hc _ hn ih =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | loop_ub h => simp_all
    | loop_exit _ h => simp_all
    | loop_normal _ _ ha _ => have := ih ha; subst this; simp [MOutcome.isNormal] at hn
    | loop_abrupt _ _ ha _ => exact ih ha
  | ite_true hu hc _ ih =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | ite_ub h => simp_all
    | ite_true _ _ h => exact ih h
    | ite_false _ h _ => simp_all
  | ite_false hu hc _ ih =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | ite_ub h => simp_all
    | ite_true _ h _ => simp_all
    | ite_false _ _ h => exact ih h
  | ite_ub hu =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | _ => simp_all
  | loop_ub hu =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | _ => simp_all
  | loop_exit hu hc =>
    cases h₂ with
    | atom ha => simp [MStmt.atomic] at ha
    | _ => simp_all

theorem mloopRun_sound (c : Expr 1) (b : MStmt) (B : MState → MOutcome)
    (hB : ∀ st o, B st = o → o ≠ .unwind → MBigStep b st o) :
    ∀ n st o, mloopRun c B n st = o → o ≠ .unwind → MBigStep (.loop c b) st o := by
  intro n
  induction n with
  | zero =>
    intro st o h hne
    simp only [mloopRun] at h
    by_cases hu : ubE st.ρ c
    · simp [hu] at h; subst h; exact .loop_ub hu
    · by_cases hc : truth (evalE st.ρ c)
      · simp [hu, hc] at h; subst h; exact absurd rfl hne
      · simp [hu, hc] at h; subst h
        exact .loop_exit (by simpa using hu) (by simpa using hc)
  | succ n ih =>
    intro st o h hne
    simp only [mloopRun] at h
    by_cases hu : ubE st.ρ c
    · simp [hu] at h; subst h; exact .loop_ub hu
    · have hu' : ubE st.ρ c = false := by simpa using hu
      by_cases hc : truth (evalE st.ρ c)
      · simp [hu, hc] at h
        cases hb : B st with
        | normal st' =>
          rw [hb] at h
          exact .loop_normal hu' hc (hB st _ hb (by simp)) (ih st' o h hne)
        | _ =>
          rw [hb] at h; simp [MOutcome.bind] at h; subst h
          exact .loop_abrupt hu' hc (hB st _ hb hne) (by simp [MOutcome.isNormal])
      · simp [hu, hc] at h; subst h
        exact .loop_exit hu' (by simpa using hc)

/-- Bounded executions with memory are real executions. -/
theorem mrun_sound (k : Nat) (s : MStmt) :
    ∀ st o, mrun k s st = o → o ≠ .unwind → MBigStep s st o := by
  induction s with
  | seq s t ihs iht =>
    intro st o h hne
    simp only [mrun] at h
    cases hs : mrun k s st with
    | normal st' =>
      rw [hs] at h
      exact .seq_normal (ihs st _ hs (by simp)) (iht st' o h hne)
    | _ =>
      rw [hs] at h; simp [MOutcome.bind] at h; subst h
      exact .seq_abrupt (ihs st _ hs hne) (by simp [MOutcome.isNormal])
  | ite c s t ihs iht =>
    intro st o h hne
    simp only [mrun] at h
    by_cases hu : ubE st.ρ c
    · simp [hu] at h; subst h; exact .ite_ub hu
    · by_cases hc : truth (evalE st.ρ c)
      · simp [hu, hc] at h; exact .ite_true (by simpa using hu) hc (ihs st o h hne)
      · simp [hu, hc] at h
        exact .ite_false (by simpa using hu) (by simpa using hc) (iht st o h hne)
  | loop c b ihb =>
    intro st o h hne
    exact mloopRun_sound c b (mrun k b) ihb k st o h hne
  | _ =>
    intro st o h _
    rw [mrun_atomic k _ rfl] at h
    subst h
    exact .atom rfl

theorem mloopRun_mono (c : Expr 1) (B B' : MState → MOutcome)
    (hB : ∀ st, B st ≠ .unwind → B' st = B st) :
    ∀ n st, mloopRun c B n st ≠ .unwind → ∀ m, n ≤ m → mloopRun c B' m st = mloopRun c B n st := by
  intro n
  induction n with
  | zero =>
    intro st h m _
    cases m with
    | zero => rfl
    | succ m =>
      simp only [mloopRun] at h ⊢
      by_cases hu : ubE st.ρ c <;> by_cases hc : truth (evalE st.ρ c) <;> simp_all
  | succ n ih =>
    intro st h m hm
    obtain ⟨m, rfl⟩ : ∃ m', m = m' + 1 := ⟨m - 1, by omega⟩
    simp only [mloopRun] at h ⊢
    by_cases hu : ubE st.ρ c
    · simp [hu]
    · by_cases hc : truth (evalE st.ρ c)
      · simp [hu, hc] at h ⊢
        cases hb : B st with
        | normal st' =>
          rw [hb] at h
          rw [hB st (by simp [hb]), hb]
          exact ih st' h m (by omega)
        | unwind => rw [hb] at h; simp at h
        | _ => rw [hB st (by simp [hb]), hb]; rfl
      · simp [hu, hc]

theorem mrun_mono (s : MStmt) :
    ∀ k k', k ≤ k' → ∀ st, mrun k s st ≠ .unwind → mrun k' s st = mrun k s st := by
  induction s with
  | seq s t ihs iht =>
    intro k k' hk st h
    simp only [mrun] at h ⊢
    cases hs : mrun k s st with
    | normal st' =>
      rw [hs] at h
      rw [ihs k k' hk st (by simp [hs]), hs]
      exact iht k k' hk st' h
    | unwind => rw [hs] at h; simp at h
    | _ => rw [ihs k k' hk st (by simp [hs]), hs]; rfl
  | ite c s t ihs iht =>
    intro k k' hk st h
    simp only [mrun] at h ⊢
    by_cases hu : ubE st.ρ c
    · simp [hu]
    · by_cases hc : truth (evalE st.ρ c)
      · simp [hu, hc] at h ⊢; exact ihs k k' hk st h
      · simp [hu, hc] at h ⊢; exact iht k k' hk st h
  | loop c b ihb =>
    intro k k' hk st h
    simp only [mrun] at h ⊢
    exact mloopRun_mono c (mrun k b) (mrun k' b) (fun st h => ihb k k' hk st h) k st h k' hk
  | _ =>
    intro k k' _ st _
    rw [mrun_atomic k' _ rfl, mrun_atomic k _ rfl]

theorem mrun_adequate {s st o} (h : MBigStep s st o) : ∃ k, mrun k s st = o := by
  induction h with
  | atom ha => exact ⟨0, rfl⟩
  | ite_ub hu => exact ⟨0, by simp [mrun, hu]⟩
  | loop_ub hu => exact ⟨0, by simp [mrun, mloopRun, hu]⟩
  | loop_exit hu hc => exact ⟨0, by simp [mrun, mloopRun, hu, hc]⟩
  | ite_true hu hc _ ih =>
    obtain ⟨k, hk⟩ := ih; exact ⟨k, by simp [mrun, hu, hc, hk]⟩
  | ite_false hu hc _ ih =>
    obtain ⟨k, hk⟩ := ih; exact ⟨k, by simp [mrun, hu, hc, hk]⟩
  | @seq_normal s t st st' o h1 h2 ih1 ih2 =>
    obtain ⟨k1, hk1⟩ := ih1
    obtain ⟨k2, hk2⟩ := ih2
    refine ⟨k1 + k2, ?_⟩
    simp only [mrun]
    rw [mrun_mono s k1 (k1 + k2) (by omega) st (by simp [hk1]), hk1]
    rw [MOutcome.bind_normal, mrun_mono t k2 (k1 + k2) (by omega) st' (by rw [hk2]; exact h2.not_unwind), hk2]
  | @seq_abrupt s t st o h1 hn ih =>
    obtain ⟨k, hk⟩ := ih
    refine ⟨k, ?_⟩
    simp only [mrun]; rw [hk]
    cases o <;> simp_all [MOutcome.isNormal]
  | @loop_normal c b st st' o hu hc h1 h2 ih1 ih2 =>
    obtain ⟨k1, hk1⟩ := ih1
    obtain ⟨k2, hk2⟩ := ih2
    refine ⟨k1 + k2 + 1, ?_⟩
    simp only [mrun, mloopRun, hu, hc]
    simp only [Bool.false_eq_true, ↓reduceIte]
    rw [mrun_mono b k1 (k1 + k2 + 1) (by omega) st (by simp [hk1]), hk1, MOutcome.bind_normal]
    simp only [mrun] at hk2
    rw [← hk2]
    exact mloopRun_mono c (mrun k2 b) (mrun (k1 + k2 + 1) b)
      (fun st h => mrun_mono b k2 _ (by omega) st h) k2 st'
      (by rw [hk2]; exact h2.not_unwind) (k1 + k2) (by omega)
  | @loop_abrupt c b st o hu hc h1 hn ih =>
    obtain ⟨k, hk⟩ := ih
    refine ⟨k + 1, ?_⟩
    simp only [mrun, mloopRun, hu, hc]
    simp only [Bool.false_eq_true, ↓reduceIte]
    rw [mrun_mono b k (k + 1) (by omega) st (by rw [hk]; exact h1.not_unwind), hk]
    cases o <;> simp_all [MOutcome.isNormal]

theorem mbigStep_iff_mrun (s : MStmt) (st : MState) (o : MOutcome) :
    MBigStep s st o ↔ o ≠ .unwind ∧ ∃ k, mrun k s st = o :=
  ⟨fun h => ⟨h.not_unwind, mrun_adequate h⟩,
   fun ⟨hne, k, hk⟩ => mrun_sound k s st o hk hne⟩

/-- Memory instrumentation in the reference semantics. -/
theorem mbigStep_minstr_fail_ub_iff (s : MStmt) (hs : s.NoUbTags) (st : MState) :
    MBigStep (minstr s) st (.fail .ub) ↔ MBigStep s st .ub := by
  simp only [mbigStep_iff_mrun, mrun_minstr]
  constructor
  · rintro ⟨-, k, hk⟩
    refine ⟨by simp, k, ?_⟩
    have := mrun_ne_fail_ub k s hs st
    cases h : mrun k s st <;> simp_all
  · rintro ⟨-, k, hk⟩
    exact ⟨by simp, k, by rw [hk]; rfl⟩

/-! ### Sanity checks: the classic memory errors are UB in this model -/

/-- Use after free. -/
theorem uaf_is_ub (k : Nat) (ρ₀ : Env) :
    mrun k (.seq (.alloc "p" (.const 4#64)) (.seq (.free "p") (.load "x" "p"))) (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, MState.allocObj, MState.freeObj, upd, liveC, atBaseC, inBoundsC, ubE, evalE]

/-- Double free. -/
theorem double_free_is_ub (k : Nat) (ρ₀ : Env) :
    mrun k (.seq (.alloc "p" (.const 4#64)) (.seq (.free "p") (.free "p"))) (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, MState.allocObj, MState.freeObj, upd, liveC, atBaseC, ubE, evalE]

/-- Out of bounds (one past the end). -/
theorem oob_is_ub (k : Nat) (ρ₀ : Env) :
    mrun k (.seq (.alloc "p" (.const 4#64)) (.seq (.gep "q" "p" (.const 4#64)) (.load "x" "q")))
      (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, MState.allocObj, MState.setPtr, upd, liveC, inBoundsC, ubE, evalE]

/-- Null dereference. -/
theorem null_deref_is_ub (k : Nat) (ρ₀ : Env) : mrun k (.load "x" "p") (MState.init ρ₀) = .ub := by
  simp [mrun, MState.init, liveC, deadObj]

/-- In-bounds access to a live object is fine and reads back the stored
byte. -/
theorem store_load_ok (k : Nat) (ρ₀ : Env) :
    ∃ st, mrun k (.seq (.alloc "p" (.const 4#64)) (.seq (.gep "q" "p" (.const 3#64))
        (.seq (.store "q" (.const 7#8)) (.load "x" "q")))) (MState.init ρ₀) = .normal st ∧
      st.ρ 8 "x" = 7#8 := by
  refine ⟨_, rfl, ?_⟩
  simp [mrun, MState.init, MState.allocObj, MState.setPtr, MState.storeByte, MState.loadByte,
    MState.setVar, upd, liveC, inBoundsC, ubE, evalE, Env.set]

end PrismSem.Mem
