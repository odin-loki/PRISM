/-
PRISM PIR — property instrumentation (roadmap Part 8.2, row "Property
instrumentation": every inserted assertion fails exactly when the
corresponding undefined behaviour occurs).

`instr` inserts, before every evaluation of an expression `e`, the check
`assert[ub] ¬(ubExpr e)`.  Loop conditions are evaluated once on entry and
again after every iteration, so the check is inserted in both places.

Main results:
* `run_instr`      — `run k (instr s) ρ = mapUb (run k s ρ)`: the instrumented
                      program behaves exactly like the original, except that
                      the outcome `ub` becomes the assertion failure `fail .ub`.
* `instr_fail_ub_iff` — an inserted assertion fails iff the original program
                      executes an operation with UB (bounded semantics).
* `bigStep_instr_fail_ub_iff` — the same for the unbounded reference semantics.
-/
import PrismSem.Semantics

namespace PrismSem

/-- The inserted check for expression `e`. -/
def chk {w : Nat} (e : Expr w) : Stmt := .assert .ub (Expr.not' (ubExpr e))

/-- UB instrumentation. -/
def instr : Stmt → Stmt
  | .skip => .skip
  | .assign x e => .seq (chk e) (.assign x e)
  | .assert t c => .seq (chk c) (.assert t c)
  | .assume c => .seq (chk c) (.assume c)
  | .seq s t => .seq (instr s) (instr t)
  | .ite c s t => .seq (chk c) (.ite c (instr s) (instr t))
  | .loop c b => .seq (chk c) (.loop c (.seq (instr b) (chk c)))
  | .ret => .ret

/-- `ub` becomes an instrumentation assertion failure; everything else is
unchanged. -/
def mapUb : Outcome → Outcome
  | .ub => .fail .ub
  | o => o

theorem run_chk (k : Nat) {w : Nat} (e : Expr w) (ρ : Env) :
    run k (chk e) ρ = if ubE ρ e then .fail .ub else .normal ρ := by
  have h1 : ubE ρ (Expr.not' (ubExpr e)) = false := by
    simp [Expr.not', Expr.tt, ubE, ubBin, ubE_ubExpr]
  have h2 : truth (evalE ρ (Expr.not' (ubExpr e))) = !ubE ρ e := by
    simp [Expr.not', Expr.tt, evalE, evalBin, ubExpr_correct]
  simp only [chk, run, h1, h2]
  cases ubE ρ e <;> simp

theorem loopRun_ub (c : Expr 1) (B : Env → Outcome) (n : Nat) (ρ : Env)
    (h : ubE ρ c = true) : loopRun c B n ρ = .ub := by
  cases n <;> simp [loopRun, h]

@[simp] theorem mapUb_normal (ρ : Env) : mapUb (.normal ρ) = .normal ρ := rfl
@[simp] theorem mapUb_ret (ρ : Env) : mapUb (.ret ρ) = .ret ρ := rfl
@[simp] theorem mapUb_fail (t : Tag) : mapUb (.fail t) = .fail t := rfl
@[simp] theorem mapUb_blocked : mapUb .blocked = .blocked := rfl
@[simp] theorem mapUb_unwind' : mapUb .unwind = .unwind := rfl
@[simp] theorem mapUb_ub : mapUb .ub = .fail .ub := rfl

@[simp] theorem Outcome.bind_ret (ρ : Env) (k : Env → Outcome) : (Outcome.ret ρ).bind k = .ret ρ := rfl
@[simp] theorem Outcome.bind_fail (t : Tag) (k : Env → Outcome) : (Outcome.fail t).bind k = .fail t := rfl
@[simp] theorem Outcome.bind_blocked (k : Env → Outcome) : Outcome.blocked.bind k = .blocked := rfl
@[simp] theorem Outcome.bind_unwind (k : Env → Outcome) : Outcome.unwind.bind k = .unwind := rfl
@[simp] theorem Outcome.bind_ub (k : Env → Outcome) : Outcome.ub.bind k = .ub := rfl

theorem loopRun_instr (k : Nat) (c : Expr 1) (b : Stmt)
    (ihb : ∀ ρ, run k (instr b) ρ = mapUb (run k b ρ)) :
    ∀ n ρ, ubE ρ c = false →
      loopRun c (run k (.seq (instr b) (chk c))) n ρ = mapUb (loopRun c (run k b) n ρ) := by
  intro n
  induction n with
  | zero =>
    intro ρ hu
    by_cases hc : truth (evalE ρ c) <;> simp [loopRun, hu, hc]
  | succ n ih =>
    intro ρ hu
    by_cases hc : truth (evalE ρ c)
    · have e1 : loopRun c (run k (.seq (instr b) (chk c))) (n + 1) ρ
          = (run k (.seq (instr b) (chk c)) ρ).bind (loopRun c (run k (.seq (instr b) (chk c))) n) := by
        simp [loopRun, hu, hc]
      have e2 : loopRun c (run k b) (n + 1) ρ = (run k b ρ).bind (loopRun c (run k b) n) := by
        simp [loopRun, hu, hc]
      have e3 : run k (.seq (instr b) (chk c)) ρ = (mapUb (run k b ρ)).bind (run k (chk c)) := by
        show (run k (instr b) ρ).bind (run k (chk c)) = _
        rw [ihb]
      rw [e1, e2, e3]
      cases hb : run k b ρ with
      | normal ρ' =>
        simp only [mapUb_normal, Outcome.bind_normal]
        rw [run_chk]
        by_cases hu' : ubE ρ' c
        · simp [hu', loopRun_ub c _ n ρ' hu']
        · simp only [hu', Bool.false_eq_true, ↓reduceIte, Outcome.bind_normal]
          exact ih ρ' (by simpa using hu')
      | _ => simp
    · simp [loopRun, hu, hc]

/-- **Instrumentation theorem (exact form).** -/
theorem run_instr (k : Nat) (s : Stmt) : ∀ ρ, run k (instr s) ρ = mapUb (run k s ρ) := by
  induction s with
  | skip => intro ρ; rfl
  | ret => intro ρ; rfl
  | assign x e =>
    intro ρ
    simp only [instr, run, run_chk]
    by_cases hu : ubE ρ e <;> simp [hu]
  | assert t c =>
    intro ρ
    simp only [instr, run, run_chk]
    by_cases hu : ubE ρ c <;> by_cases hc : truth (evalE ρ c) <;> simp [hu, hc]
  | assume c =>
    intro ρ
    simp only [instr, run, run_chk]
    by_cases hu : ubE ρ c <;> by_cases hc : truth (evalE ρ c) <;> simp [hu, hc]
  | seq s t ihs iht =>
    intro ρ
    show (run k (instr s) ρ).bind (run k (instr t)) = mapUb ((run k s ρ).bind (run k t))
    rw [ihs]
    cases run k s ρ <;> simp [iht]
  | ite c s t ihs iht =>
    intro ρ
    show (run k (chk c) ρ).bind (run k (.ite c (instr s) (instr t))) = mapUb (run k (.ite c s t) ρ)
    rw [run_chk]
    by_cases hu : ubE ρ c
    · simp [hu, run]
    · by_cases hc : truth (evalE ρ c) <;> simp [hu, hc, run, ihs, iht]
  | loop c b ihb =>
    intro ρ
    show (run k (chk c) ρ).bind (run k (.loop c (.seq (instr b) (chk c)))) = mapUb (loopRun c (run k b) k ρ)
    rw [run_chk]
    by_cases hu : ubE ρ c
    · simp [hu, loopRun_ub c _ k ρ hu]
    · simp only [hu, Bool.false_eq_true, ↓reduceIte, Outcome.bind_normal]
      exact loopRun_instr k c b ihb k ρ (by simpa using hu)

theorem mapUb_eq_fail_ub (o : Outcome) : mapUb o = .fail .ub ↔ o = .ub ∨ o = .fail .ub := by
  cases o <;> simp [mapUb]

/-- A program without user-written `.ub`-tagged assertions never ends in
`fail .ub`. -/
theorem run_ne_fail_ub (k : Nat) (s : Stmt) (hs : s.NoUbTags) : ∀ ρ, run k s ρ ≠ .fail .ub := by
  induction s with
  | assert t c =>
    intro ρ
    simp only [Stmt.NoUbTags] at hs; subst hs
    simp only [run]
    cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
  | assign x e => intro ρ; simp only [run]; cases ubE ρ e <;> simp
  | assume c => intro ρ; simp only [run]; cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
  | skip => intro ρ; simp [run]
  | ret => intro ρ; simp [run]
  | seq s t ihs iht =>
    intro ρ
    simp only [run]
    cases h : run k s ρ with
    | normal ρ' => exact iht hs.2 ρ'
    | fail t => have := ihs hs.1 ρ; rw [h] at this; simpa [Outcome.bind] using this
    | _ => simp [Outcome.bind]
  | ite c s t ihs iht =>
    intro ρ
    simp only [run]
    cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp [ihs hs.1, iht hs.2]
  | loop c b ihb =>
    intro ρ
    simp only [run]
    have key : ∀ n ρ, loopRun c (run k b) n ρ ≠ .fail .ub := by
      intro n
      induction n with
      | zero => intro ρ; simp only [loopRun]; cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
      | succ n ih =>
        intro ρ
        simp only [loopRun]
        cases ubE ρ c <;> cases truth (evalE ρ c) <;> simp
        cases h : run k b ρ with
        | normal ρ' => exact ih ρ'
        | fail t => have := ihb hs ρ; rw [h] at this; simpa [Outcome.bind] using this
        | _ => simp [Outcome.bind]
    exact key k ρ

/-- **Property instrumentation (8.2).**  An inserted UB assertion of the
instrumented program fails iff the original program executes an operation
with undefined behaviour (bounded semantics, any bound `k`). -/
theorem instr_fail_ub_iff (k : Nat) (s : Stmt) (hs : s.NoUbTags) (ρ : Env) :
    run k (instr s) ρ = .fail .ub ↔ run k s ρ = .ub := by
  rw [run_instr, mapUb_eq_fail_ub]
  have := run_ne_fail_ub k s hs ρ
  constructor
  · rintro (h | h)
    · exact h
    · exact absurd h this
  · exact Or.inl

/-- The instrumented program never has UB itself. -/
theorem instr_no_ub (k : Nat) (s : Stmt) (ρ : Env) : run k (instr s) ρ ≠ .ub := by
  rw [run_instr]; cases run k s ρ <;> simp [mapUb]

/-- User assertions fail in the instrumented program exactly as before. -/
theorem instr_fail_user_iff (k : Nat) (s : Stmt) (ρ : Env) :
    run k (instr s) ρ = .fail .user ↔ run k s ρ = .fail .user := by
  rw [run_instr]; cases run k s ρ <;> simp [mapUb]

theorem mapUb_unwind (o : Outcome) : mapUb o = .unwind ↔ o = .unwind := by
  cases o <;> simp [mapUb]

/-- Instrumentation in the reference (unbounded) semantics. -/
theorem bigStep_instr (s : Stmt) (ρ : Env) (o' : Outcome) :
    BigStep (instr s) ρ o' ↔ ∃ o, BigStep s ρ o ∧ o' = mapUb o := by
  simp only [bigStep_iff_run, run_instr]
  constructor
  · rintro ⟨hne, k, hk⟩
    refine ⟨run k s ρ, ⟨?_, k, rfl⟩, hk.symm⟩
    intro h; rw [h] at hk; simp [mapUb] at hk; exact hne hk.symm
  · rintro ⟨o, ⟨hne, k, hk⟩, rfl⟩
    refine ⟨fun h => hne ((mapUb_unwind o).1 h), k, by rw [hk]⟩

/-- **Property instrumentation, reference semantics.** -/
theorem bigStep_instr_fail_ub_iff (s : Stmt) (hs : s.NoUbTags) (ρ : Env) :
    BigStep (instr s) ρ (.fail .ub) ↔ BigStep s ρ .ub := by
  rw [bigStep_instr]
  constructor
  · rintro ⟨o, ho, he⟩
    rcases (mapUb_eq_fail_ub o).1 he.symm with h | h
    · subst h; exact ho
    · subst h
      obtain ⟨k, hk⟩ := run_adequate ho
      exact absurd hk (run_ne_fail_ub k s hs ρ)
  · intro h; exact ⟨.ub, h, rfl⟩

end PrismSem
