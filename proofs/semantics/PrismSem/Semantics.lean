/-
PRISM PIR — operational semantics (roadmap Part 5.2).

Two semantics, proved to agree:

* `BigStep s ρ o` — the reference big-step relation.  Loops iterate as often
  as they need; there is no bound.  This is "the meaning of the program".
* `run k s ρ` — the bounded, executable semantics that a bounded model
  checker explores: every entry into a loop may run the body at most `k`
  times; needing a `(k+1)`-th iteration yields the outcome `unwind`.

Outcomes make every way an execution can end explicit:
`normal` (fell off the end), `ret` (executed `return`), `fail t` (an assertion
with tag `t` was false), `blocked` (an `assume` was false — not an error,
the path does not exist), `ub` (an operation with undefined behaviour was
executed — see `ubE`), and `unwind` (bounded semantics only: the loop bound
was exhausted).
-/
import PrismSem.Eval

-- Proof scripts share simp sets across `<;>` branches; unused-argument noise is expected.
set_option linter.unusedSimpArgs false

namespace PrismSem

inductive Outcome where
  | normal (ρ : Env)
  | ret (ρ : Env)
  | fail (t : Tag)
  | blocked
  | unwind
  | ub

namespace Outcome

/-- Sequential composition: continue only after a normal outcome. -/
def bind : Outcome → (Env → Outcome) → Outcome
  | .normal ρ, k => k ρ
  | o, _ => o

def isNormal : Outcome → Bool
  | .normal _ => true
  | _ => false

@[simp] theorem bind_normal (ρ : Env) (k : Env → Outcome) : (Outcome.normal ρ).bind k = k ρ := rfl

theorem bind_abrupt (o : Outcome) (k : Env → Outcome) (h : o.isNormal = false) : o.bind k = o := by
  cases o <;> simp_all [bind, isNormal]

end Outcome

/-- The bounded loop: `loopRun c B n ρ` runs `while c do B` with at most
`n` more iterations. -/
def loopRun (c : Expr 1) (B : Env → Outcome) : Nat → Env → Outcome
  | 0, ρ =>
    if ubE ρ c then .ub else if truth (evalE ρ c) then .unwind else .normal ρ
  | n + 1, ρ =>
    if ubE ρ c then .ub else if truth (evalE ρ c) then (B ρ).bind (loopRun c B n) else .normal ρ

/-- Bounded semantics with unwinding bound `k` (per loop entry). -/
def run (k : Nat) : Stmt → Env → Outcome
  | .skip, ρ => .normal ρ
  | .assign (w := w) x e, ρ => if ubE ρ e then .ub else .normal (ρ.set w x (evalE ρ e))
  | .assert t c, ρ =>
    if ubE ρ c then .ub else if truth (evalE ρ c) then .normal ρ else .fail t
  | .assume c, ρ =>
    if ubE ρ c then .ub else if truth (evalE ρ c) then .normal ρ else .blocked
  | .seq s t, ρ => (run k s ρ).bind (run k t)
  | .ite c s t, ρ =>
    if ubE ρ c then .ub else if truth (evalE ρ c) then run k s ρ else run k t ρ
  | .loop c b, ρ => loopRun c (run k b) k ρ
  | .ret, ρ => .ret ρ

/-- The reference (unbounded) big-step semantics. -/
inductive BigStep : Stmt → Env → Outcome → Prop where
  | skip {ρ} : BigStep .skip ρ (.normal ρ)
  | assign_ub {w ρ} {x : String} {e : Expr w} :
      ubE ρ e = true → BigStep (.assign x e) ρ .ub
  | assign {w ρ} {x : String} {e : Expr w} :
      ubE ρ e = false → BigStep (.assign x e) ρ (.normal (ρ.set w x (evalE ρ e)))
  | assert_ub {ρ t c} : ubE ρ c = true → BigStep (.assert t c) ρ .ub
  | assert_ok {ρ t c} :
      ubE ρ c = false → truth (evalE ρ c) = true → BigStep (.assert t c) ρ (.normal ρ)
  | assert_fail {ρ t c} :
      ubE ρ c = false → truth (evalE ρ c) = false → BigStep (.assert t c) ρ (.fail t)
  | assume_ub {ρ c} : ubE ρ c = true → BigStep (.assume c) ρ .ub
  | assume_ok {ρ c} :
      ubE ρ c = false → truth (evalE ρ c) = true → BigStep (.assume c) ρ (.normal ρ)
  | assume_block {ρ c} :
      ubE ρ c = false → truth (evalE ρ c) = false → BigStep (.assume c) ρ .blocked
  | seq_normal {s t ρ ρ' o} :
      BigStep s ρ (.normal ρ') → BigStep t ρ' o → BigStep (.seq s t) ρ o
  | seq_abrupt {s t ρ o} :
      BigStep s ρ o → o.isNormal = false → BigStep (.seq s t) ρ o
  | ite_ub {c s t ρ} : ubE ρ c = true → BigStep (.ite c s t) ρ .ub
  | ite_true {c s t ρ o} :
      ubE ρ c = false → truth (evalE ρ c) = true → BigStep s ρ o → BigStep (.ite c s t) ρ o
  | ite_false {c s t ρ o} :
      ubE ρ c = false → truth (evalE ρ c) = false → BigStep t ρ o → BigStep (.ite c s t) ρ o
  | loop_ub {c b ρ} : ubE ρ c = true → BigStep (.loop c b) ρ .ub
  | loop_exit {c b ρ} :
      ubE ρ c = false → truth (evalE ρ c) = false → BigStep (.loop c b) ρ (.normal ρ)
  | loop_normal {c b ρ ρ' o} :
      ubE ρ c = false → truth (evalE ρ c) = true →
      BigStep b ρ (.normal ρ') → BigStep (.loop c b) ρ' o → BigStep (.loop c b) ρ o
  | loop_abrupt {c b ρ o} :
      ubE ρ c = false → truth (evalE ρ c) = true →
      BigStep b ρ o → o.isNormal = false → BigStep (.loop c b) ρ o
  | ret {ρ} : BigStep .ret ρ (.ret ρ)

/-! ### Properties of the reference semantics -/

/-- The reference semantics never produces `unwind`. -/
theorem BigStep.not_unwind {s ρ o} (h : BigStep s ρ o) : o ≠ .unwind := by
  induction h <;> first | assumption | simp_all [Outcome.isNormal] | (intro h; cases h)

/-- The reference semantics is deterministic. -/
theorem BigStep.det {s ρ o₁ o₂} (h₁ : BigStep s ρ o₁) (h₂ : BigStep s ρ o₂) : o₁ = o₂ := by
  induction h₁ generalizing o₂ with
  | seq_normal _ _ ih1 ih2 =>
    cases h₂ with
    | seq_normal ha hb => cases ih1 ha; exact ih2 hb
    | seq_abrupt ha hn => have := ih1 ha; subst this; simp [Outcome.isNormal] at hn
  | seq_abrupt _ hn ih =>
    cases h₂ with
    | seq_normal ha _ => have := ih ha; subst this; simp [Outcome.isNormal] at hn
    | seq_abrupt ha _ => exact ih ha
  | loop_normal hu hc _ _ ih1 ih2 =>
    cases h₂ with
    | loop_ub h => simp_all
    | loop_exit _ h => simp_all
    | loop_normal _ _ ha hb => cases ih1 ha; exact ih2 hb
    | loop_abrupt _ _ ha hn => have := ih1 ha; subst this; simp [Outcome.isNormal] at hn
  | loop_abrupt hu hc _ hn ih =>
    cases h₂ with
    | loop_ub h => simp_all
    | loop_exit _ h => simp_all
    | loop_normal _ _ ha _ => have := ih ha; subst this; simp [Outcome.isNormal] at hn
    | loop_abrupt _ _ ha _ => exact ih ha
  | ite_true hu hc _ ih =>
    cases h₂ with
    | ite_ub h => simp_all
    | ite_true _ _ h => exact ih h
    | ite_false _ h _ => simp_all
  | ite_false hu hc _ ih =>
    cases h₂ with
    | ite_ub h => simp_all
    | ite_true _ h _ => simp_all
    | ite_false _ _ h => exact ih h
  | _ => cases h₂ <;> simp_all

/-! ### The bounded semantics agrees with the reference semantics -/

theorem loopRun_sound (c : Expr 1) (b : Stmt) (B : Env → Outcome)
    (hB : ∀ ρ o, B ρ = o → o ≠ .unwind → BigStep b ρ o) :
    ∀ n ρ o, loopRun c B n ρ = o → o ≠ .unwind → BigStep (.loop c b) ρ o := by
  intro n
  induction n with
  | zero =>
    intro ρ o h hne
    simp only [loopRun] at h
    by_cases hu : ubE ρ c
    · simp [hu] at h; subst h; exact .loop_ub hu
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h; subst h; exact absurd rfl hne
      · simp [hu, hc] at h; subst h
        exact .loop_exit (by simpa using hu) (by simpa using hc)
  | succ n ih =>
    intro ρ o h hne
    simp only [loopRun] at h
    by_cases hu : ubE ρ c
    · simp [hu] at h; subst h; exact .loop_ub hu
    · have hu' : ubE ρ c = false := by simpa using hu
      by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h
        cases hb : B ρ with
        | normal ρ' =>
          rw [hb] at h
          exact .loop_normal hu' hc (hB ρ _ hb (by simp)) (ih ρ' o h hne)
        | _ =>
          rw [hb] at h; simp [Outcome.bind] at h; subst h
          exact .loop_abrupt hu' hc (hB ρ _ hb hne) (by simp [Outcome.isNormal])
      · simp [hu, hc] at h; subst h
        exact .loop_exit hu' (by simpa using hc)

/-- **Bounded executions are real executions.**  Any outcome of the bounded
semantics other than `unwind` is an outcome of the reference semantics. -/
theorem run_sound (k : Nat) (s : Stmt) :
    ∀ ρ o, run k s ρ = o → o ≠ .unwind → BigStep s ρ o := by
  induction s with
  | skip => intro ρ o h _; subst h; exact .skip
  | ret => intro ρ o h _; subst h; exact .ret
  | assign x e =>
    intro ρ o h _
    simp only [run] at h
    by_cases hu : ubE ρ e
    · simp [hu] at h; subst h; exact .assign_ub hu
    · simp [hu] at h; subst h; exact .assign (by simpa using hu)
  | assert t c =>
    intro ρ o h _
    simp only [run] at h
    by_cases hu : ubE ρ c
    · simp [hu] at h; subst h; exact .assert_ub hu
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h; subst h; exact .assert_ok (by simpa using hu) hc
      · simp [hu, hc] at h; subst h; exact .assert_fail (by simpa using hu) (by simpa using hc)
  | assume c =>
    intro ρ o h _
    simp only [run] at h
    by_cases hu : ubE ρ c
    · simp [hu] at h; subst h; exact .assume_ub hu
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h; subst h; exact .assume_ok (by simpa using hu) hc
      · simp [hu, hc] at h; subst h; exact .assume_block (by simpa using hu) (by simpa using hc)
  | seq s t ihs iht =>
    intro ρ o h hne
    simp only [run] at h
    cases hs : run k s ρ with
    | normal ρ' =>
      rw [hs] at h
      exact .seq_normal (ihs ρ _ hs (by simp)) (iht ρ' o h hne)
    | _ =>
      rw [hs] at h; simp [Outcome.bind] at h; subst h
      exact .seq_abrupt (ihs ρ _ hs hne) (by simp [Outcome.isNormal])
  | ite c s t ihs iht =>
    intro ρ o h hne
    simp only [run] at h
    by_cases hu : ubE ρ c
    · simp [hu] at h; subst h; exact .ite_ub hu
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h; exact .ite_true (by simpa using hu) hc (ihs ρ o h hne)
      · simp [hu, hc] at h
        exact .ite_false (by simpa using hu) (by simpa using hc) (iht ρ o h hne)
  | loop c b ihb =>
    intro ρ o h hne
    exact loopRun_sound c b (run k b) ihb k ρ o h hne

theorem loopRun_mono (c : Expr 1) (B B' : Env → Outcome)
    (hB : ∀ ρ, B ρ ≠ .unwind → B' ρ = B ρ) :
    ∀ n ρ, loopRun c B n ρ ≠ .unwind → ∀ m, n ≤ m → loopRun c B' m ρ = loopRun c B n ρ := by
  intro n
  induction n with
  | zero =>
    intro ρ h m _
    cases m with
    | zero => rfl
    | succ m =>
      simp only [loopRun] at h ⊢
      by_cases hu : ubE ρ c <;> by_cases hc : truth (evalE ρ c) <;> simp_all
  | succ n ih =>
    intro ρ h m hm
    obtain ⟨m, rfl⟩ : ∃ m', m = m' + 1 := ⟨m - 1, by omega⟩
    simp only [loopRun] at h ⊢
    by_cases hu : ubE ρ c
    · simp [hu]
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h ⊢
        cases hb : B ρ with
        | normal ρ' =>
          rw [hb] at h
          rw [hB ρ (by simp [hb]), hb]
          exact ih ρ' h m (by omega)
        | unwind => rw [hb] at h; simp [Outcome.bind] at h
        | _ => rw [hB ρ (by simp [hb]), hb]; rfl
      · simp [hu, hc]

/-- **Monotonicity in the bound.**  Once an execution finishes within bound
`k`, every larger bound gives the same outcome. -/
theorem run_mono (s : Stmt) :
    ∀ k k', k ≤ k' → ∀ ρ, run k s ρ ≠ .unwind → run k' s ρ = run k s ρ := by
  induction s with
  | seq s t ihs iht =>
    intro k k' hk ρ h
    simp only [run] at h ⊢
    cases hs : run k s ρ with
    | normal ρ' =>
      rw [hs] at h
      rw [ihs k k' hk ρ (by simp [hs]), hs]
      exact iht k k' hk ρ' h
    | unwind => rw [hs] at h; simp [Outcome.bind] at h
    | _ => rw [ihs k k' hk ρ (by simp [hs]), hs]; rfl
  | ite c s t ihs iht =>
    intro k k' hk ρ h
    simp only [run] at h ⊢
    by_cases hu : ubE ρ c
    · simp [hu]
    · by_cases hc : truth (evalE ρ c)
      · simp [hu, hc] at h ⊢; exact ihs k k' hk ρ h
      · simp [hu, hc] at h ⊢; exact iht k k' hk ρ h
  | loop c b ihb =>
    intro k k' hk ρ h
    simp only [run] at h ⊢
    exact loopRun_mono c (run k b) (run k' b) (fun ρ h => ihb k k' hk ρ h) k ρ h k' hk
  | _ => intro k k' _ ρ _; rfl

/-- **Adequacy.**  Every terminating execution of the reference semantics is
found by the bounded semantics for some bound. -/
theorem run_adequate {s ρ o} (h : BigStep s ρ o) : ∃ k, run k s ρ = o := by
  induction h with
  | skip => exact ⟨0, rfl⟩
  | ret => exact ⟨0, rfl⟩
  | assign_ub hu => exact ⟨0, by simp [run, hu]⟩
  | assign hu => exact ⟨0, by simp [run, hu]⟩
  | assert_ub hu => exact ⟨0, by simp [run, hu]⟩
  | assert_ok hu hc => exact ⟨0, by simp [run, hu, hc]⟩
  | assert_fail hu hc => exact ⟨0, by simp [run, hu, hc]⟩
  | assume_ub hu => exact ⟨0, by simp [run, hu]⟩
  | assume_ok hu hc => exact ⟨0, by simp [run, hu, hc]⟩
  | assume_block hu hc => exact ⟨0, by simp [run, hu, hc]⟩
  | ite_ub hu => exact ⟨0, by simp [run, hu]⟩
  | loop_ub hu => exact ⟨0, by simp [run, loopRun, hu]⟩
  | loop_exit hu hc => exact ⟨0, by simp [run, loopRun, hu, hc]⟩
  | ite_true hu hc _ ih =>
    obtain ⟨k, hk⟩ := ih; exact ⟨k, by simp [run, hu, hc, hk]⟩
  | ite_false hu hc _ ih =>
    obtain ⟨k, hk⟩ := ih; exact ⟨k, by simp [run, hu, hc, hk]⟩
  | @seq_normal s t ρ ρ' o h1 h2 ih1 ih2 =>
    obtain ⟨k1, hk1⟩ := ih1
    obtain ⟨k2, hk2⟩ := ih2
    refine ⟨k1 + k2, ?_⟩
    simp only [run]
    rw [run_mono s k1 (k1 + k2) (by omega) ρ (by simp [hk1]), hk1]
    rw [Outcome.bind_normal, run_mono t k2 (k1 + k2) (by omega) ρ' (by rw [hk2]; exact h2.not_unwind), hk2]
  | @seq_abrupt s t ρ o h1 hn ih =>
    obtain ⟨k, hk⟩ := ih
    refine ⟨k, ?_⟩
    simp only [run]; rw [hk]; exact Outcome.bind_abrupt o _ hn
  | @loop_normal c b ρ ρ' o hu hc h1 h2 ih1 ih2 =>
    obtain ⟨k1, hk1⟩ := ih1
    obtain ⟨k2, hk2⟩ := ih2
    refine ⟨k1 + k2 + 1, ?_⟩
    simp only [run, loopRun, hu, hc]
    simp only [Bool.false_eq_true, ↓reduceIte]
    rw [run_mono b k1 (k1 + k2 + 1) (by omega) ρ (by simp [hk1]), hk1, Outcome.bind_normal]
    simp only [run] at hk2
    rw [← hk2]
    exact loopRun_mono c (run k2 b) (run (k1 + k2 + 1) b)
      (fun ρ h => run_mono b k2 _ (by omega) ρ h) k2 ρ'
      (by rw [hk2]; exact h2.not_unwind) (k1 + k2) (by omega)
  | @loop_abrupt c b ρ o hu hc h1 hn ih =>
    obtain ⟨k, hk⟩ := ih
    refine ⟨k + 1, ?_⟩
    simp only [run, loopRun, hu, hc]
    simp only [Bool.false_eq_true, ↓reduceIte]
    rw [run_mono b k (k + 1) (by omega) ρ (by rw [hk]; exact h1.not_unwind), hk]
    exact Outcome.bind_abrupt o _ hn

/-- The reference semantics is exactly the non-`unwind` part of the bounded
semantics, over all bounds. -/
theorem bigStep_iff_run (s : Stmt) (ρ : Env) (o : Outcome) :
    BigStep s ρ o ↔ o ≠ .unwind ∧ ∃ k, run k s ρ = o :=
  ⟨fun h => ⟨h.not_unwind, run_adequate h⟩,
   fun ⟨hne, k, hk⟩ => run_sound k s ρ o hk hne⟩

end PrismSem
