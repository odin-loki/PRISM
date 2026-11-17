import Prism.Verdict

/-!
Assume-guarantee over the call sites of a module (docs/HARNESS_GENERATOR.md,
`--pir-discharge`; src/prism/pir/stage.cpp `discharge_pass`, translate.cpp
`call_obligations`).

PRISM proves a function safe under a precondition `P`: the automatic pointer
harness (an invented assumption). `--pir-discharge` turns `P` into checks at
every call site of the unit. Two things have to be true for that to be sound,
and both are proved here:

1. `assume_guarantee`: the composition. If every function is safe under its
   precondition, and every call site that is reached with the caller's
   precondition establishes the callee's, then every invocation reachable from
   an entry function started under its precondition satisfies the precondition
   of the function invoked and is safe. Functions that can be called from
   outside the module (exported, address taken) are exactly the `entry`
   predicate; for them the precondition stays an assumption.

2. `obligation_iff`: the check the C++ emits at a call site (`MemTr::obligation_object`, the
   `ob-*` properties of class HARNESS-OBLIGATION) holds exactly when the
   harness's object exists in the caller's memory: a live, writable object aligned as
   the callee's accesses need (the proof used the alignment only in its access checks), of at least the harnessed size, the pointer at the harnessed offset into it, its
   bytes initialised (up to the cap). `obligation_props` is the list of property names;
   tests/cpp/test_pir_harness.cpp compares it with the names the code emits.

Core Lean only. No `sorry`; the axioms of the theorems are audited in `Prism/Axioms.lean`.
-/

namespace Prism.Discharge

/-! ## 1. Composition -/

/-- A module as the call structure the check needs: `own f s` says the checks
of function `f`'s own code pass when it is entered in state `s` (callees are not
looked into); `calls f s` lists the call sites that execution reaches, with the
state at each. Functions are numbered; `σ` is whatever state the checks see. -/
structure Prog (σ : Type) where
  own : Nat → σ → Prop
  calls : Nat → σ → List (Nat × σ)

/-- Invocations reachable from an entry function started under its precondition. -/
inductive Reach {σ : Type} (P : Prog σ) (pre : Nat → σ → Prop) (entry : Nat → Prop) : Nat → σ → Prop
  | start (f : Nat) (s : σ) : entry f → pre f s → Reach P pre entry f s
  | call (f : Nat) (s : σ) (g : Nat) (t : σ) :
      Reach P pre entry f s → (g, t) ∈ P.calls f s → Reach P pre entry g t

/-- The composition. `h_own`: each function is safe under its precondition (what the
solver proved, with the harness as `pre`). `h_site`: every call site reached under the
caller's precondition establishes the callee's (what the discharge checks). Then no
reachable invocation violates a check: its precondition holds (so the proof of the callee
applies) and its own checks pass. -/
theorem assume_guarantee {σ : Type} (P : Prog σ) (pre : Nat → σ → Prop) (entry : Nat → Prop)
    (h_own : ∀ f s, pre f s → P.own f s)
    (h_site : ∀ f s, pre f s → ∀ c ∈ P.calls f s, pre c.1 c.2) :
    ∀ f s, Reach P pre entry f s → pre f s ∧ P.own f s := by
  intro f s h
  induction h with
  | start f s _ hp => exact ⟨hp, h_own f s hp⟩
  | call f s g t _ hc ih =>
    have hp : pre g t := h_site f s ih.1 (g, t) hc
    exact ⟨hp, h_own g t hp⟩

/-- A function that is not an entry (static, address not taken, not exported) is reached only
through call sites, so its precondition needs no assumption of its own: it holds at every
invocation as soon as the entries' preconditions hold. -/
theorem internal_pre_holds {σ : Type} (P : Prog σ) (pre : Nat → σ → Prop) (entry : Nat → Prop)
    (h_own : ∀ f s, pre f s → P.own f s)
    (h_site : ∀ f s, pre f s → ∀ c ∈ P.calls f s, pre c.1 c.2) (g : Nat) (t : σ)
    (h : Reach P pre entry g t) : pre g t :=
  (assume_guarantee P pre entry h_own h_site g t h).1

/-- Dropping the call-site check is not sound: a module where a callee is safe under its
precondition, but a call site does not establish it, has a reachable unsafe invocation. -/
theorem site_check_needed :
    ∃ (P : Prog Nat) (pre : Nat → Nat → Prop) (entry : Nat → Prop),
      (∀ f s, pre f s → P.own f s) ∧ (∃ f s, Reach P pre entry f s ∧ ¬ P.own f s) := by
  refine ⟨⟨fun f s => f = 0 ∨ s = 1, fun f s => if f = 0 then [(1, 0)] else []⟩,
          fun f s => f = 0 ∨ s = 1, fun f => f = 0, ?_, 1, 0, ?_, ?_⟩
  · intro f s h
    rcases h with h | h
    · exact Or.inl h
    · exact Or.inr h
  · apply Reach.call 0 0 1 0
    · exact Reach.start 0 0 rfl (Or.inl rfl)
    · simp
  · intro h
    rcases h with h | h <;> simp at h

/-! ## 2. The call-site check is the harness precondition -/

/-- What the harness allocates, as the caller's memory shows it. -/
structure Obj where
  live : Bool
  size : Nat
  align : Nat
  readOnly : Bool
  init : Nat → Bool

/-- Memory: the object table (`none`: the pointer is null or not an object). A pointer is
(object, byte offset). -/
abbrev Mem := Nat → Option Obj

/-- The harness precondition of one pointer argument: it points `memberOff` bytes into a live,
writable object aligned to `align` of at least `bytes` bytes, whose first `bytes` bytes are
initialised (when `checkInit`; objects over the cap keep that as a stated assumption). -/
def Pre (m : Mem) (p : Nat × Nat) (bytes memberOff align : Nat) (checkInit : Bool) : Prop :=
  ∃ o, m p.1 = some o ∧ o.live = true ∧ p.2 = memberOff ∧ bytes ≤ o.size ∧ align ≤ o.align ∧
    o.readOnly = false ∧ (checkInit = true → ∀ i, i < bytes → o.init i = true)

/-- The names of the properties the code checks (`MemTr::obligation_object`,
`MemTr::obligation_distinct`; class HARNESS-OBLIGATION). -/
def obligationProps : List String :=
  ["ob-null", "ob-dead", "ob-offset", "ob-size", "ob-align", "ob-const", "ob-init", "ob-distinct"]

/-- The properties the code reports violated for one pointer argument, in the order it
checks them. -/
def violations (m : Mem) (p : Nat × Nat) (bytes memberOff align : Nat) (checkInit : Bool) : List String :=
  match m p.1 with
  | none => ["ob-null"]
  | some o =>
    (if o.live = false then ["ob-dead"] else []) ++
    (if p.2 ≠ memberOff then ["ob-offset"] else []) ++
    (if o.size < bytes then ["ob-size"] else []) ++
    (if o.align < align then ["ob-align"] else []) ++
    (if o.readOnly = true then ["ob-const"] else []) ++
    (if checkInit = true ∧ ∃ i, i < bytes ∧ o.init i = false then ["ob-init"] else [])

/-- The check is the precondition: no property is reported violated exactly when the harness's
object exists. Together with `assume_guarantee` this is what lets the call sites replace the
harness assumption. -/
theorem obligation_iff (m : Mem) (p : Nat × Nat) (bytes memberOff align : Nat) (checkInit : Bool) :
    violations m p bytes memberOff align checkInit = [] ↔ Pre m p bytes memberOff align checkInit := by
  classical
  have key : ∀ (c : Prop) [Decidable c] (s : String), (if c then [s] else ([] : List String)) = [] ↔ ¬ c := by
    intro c _ s
    by_cases h : c <;> simp [h]
  unfold violations Pre
  cases hm : m p.1 with
  | none => simp
  | some o =>
    simp only [List.append_eq_nil_iff, key, Option.some.injEq, exists_eq_left']
    constructor
    · rintro ⟨⟨⟨⟨⟨h1, h2⟩, h3⟩, h4⟩, h5⟩, h6⟩
      refine ⟨by simpa using h1, by simpa using h2, by omega, by omega, by simpa using h5, ?_⟩
      intro hc i hi
      cases hh : o.init i
      · exact absurd ⟨hc, i, hi, hh⟩ h6
      · rfl
    · rintro ⟨h1, h2, h3, h4, h5, h6⟩
      refine ⟨⟨⟨⟨⟨by simpa using h1, by simpa using h2⟩, by omega⟩, by omega⟩, by simpa using h5⟩, ?_⟩
      rintro ⟨hc, i, hi, hf⟩
      have := h6 hc i hi
      simp [hf] at this

end Prism.Discharge
