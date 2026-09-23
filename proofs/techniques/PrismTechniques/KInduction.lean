/-
PRISM techniques: k-induction soundness (roadmap 8.2, row "k-induction").

An abstract transition system: states `S`, initial states `I`, transition
relation `T`, property `P`.  PRISM's k-induction stage issues two solver
queries per loop:

* base case: no violation of `P` in the first `k` states of any path from `I`;
* step case: *havoc* the loop state (start from an arbitrary state, not an
  initial one), assume `P` on `k` consecutive states linked by `T`, and ask
  whether `P` can fail on the next state.

`kinduction_sound` proves that the two premises imply `P` on every reachable
state.  `step_iff_havoc_query_unsat` shows that the step premise is exactly
the unsatisfiability of the havoc query the engine sends to the solver, and
`step_iff_havoc_split` shows the same for a state split into a frozen part and
a loop-modified part that is havocked.
-/

namespace PrismTechniques.KInduction

variable {S : Type}

/-- Reachable states of the transition system `(I, T)`. -/
inductive Reach (I : S → Prop) (T : S → S → Prop) : S → Prop
  | init {s} : I s → Reach I T s
  | step {s s'} : Reach I T s → T s s' → Reach I T s'

/-- `π` is a `T`-path of `n` steps: states `π 0, …, π n`. -/
def Path (T : S → S → Prop) (π : Nat → S) (n : Nat) : Prop :=
  ∀ i, i < n → T (π i) (π (i + 1))

/-- Base case of k-induction: every state reached from `I` within fewer than
`k` steps satisfies `P`. -/
def Base (I : S → Prop) (T : S → S → Prop) (P : S → Prop) (k : Nat) : Prop :=
  ∀ (π : Nat → S) (n : Nat), n < k → I (π 0) → Path T π n → P (π n)

/-- Step case of k-induction: any `k` consecutive `P`-states (starting from an
*arbitrary* state) followed by one more `T` step stay in `P`. -/
def Step (T : S → S → Prop) (P : S → Prop) (k : Nat) : Prop :=
  ∀ (π : Nat → S), Path T π k → (∀ i, i < k → P (π i)) → P (π k)

/-- Reachability is exactly "the end of some path from an initial state". -/
theorem reach_iff_path (I : S → Prop) (T : S → S → Prop) (s : S) :
    Reach I T s ↔ ∃ (π : Nat → S) (n : Nat), I (π 0) ∧ Path T π n ∧ π n = s := by
  constructor
  · intro h
    induction h with
    | @init s hs => exact ⟨fun _ => s, 0, hs, fun i hi => absurd hi (Nat.not_lt_zero _), rfl⟩
    | @step s s' _ hT ih =>
      obtain ⟨π, n, h0, hp, hn⟩ := ih
      refine ⟨fun i => if i ≤ n then π i else s', n + 1, ?_, ?_, ?_⟩
      · simpa using h0
      · intro i hi
        by_cases hin : i < n
        · have h1 : i ≤ n := Nat.le_of_lt hin
          have h2 : i + 1 ≤ n := hin
          simp only [h1, h2, ite_true]
          exact hp i hin
        · have hi' : i = n := by omega
          subst hi'
          simp only [Nat.le_refl, ite_true, Nat.not_succ_le_self, ite_false]
          rw [hn]; exact hT
      · have hn1 : ¬ (n + 1 ≤ n) := by omega
        simp [hn1]
  · rintro ⟨π, n, h0, hp, rfl⟩
    induction n with
    | zero => exact Reach.init h0
    | succ n ih =>
      exact Reach.step (ih (fun i hi => hp i (Nat.lt_succ_of_lt hi))) (hp n (Nat.lt_succ_self n))

/-- **k-induction soundness.**  If the base case and the step case hold for
some `k`, then `P` holds in every reachable state.  (No `k ≥ 1` side condition is
needed: for `k = 0` the step premise already says `P` holds everywhere.) -/
theorem kinduction_sound (I : S → Prop) (T : S → S → Prop) (P : S → Prop) (k : Nat)
    (hbase : Base I T P k) (hstep : Step T P k) :
    ∀ s, Reach I T s → P s := by
  intro s hs
  obtain ⟨π, n, h0, hp, rfl⟩ := (reach_iff_path I T s).1 hs
  -- strong induction on the path length, for this fixed path `π`
  have key : ∀ m, m ≤ n → P (π m) := by
    intro m
    refine Nat.strongRecOn (motive := fun m => m ≤ n → P (π m)) m ?_
    intro m ih hm
    by_cases hmk : m < k
    · exact hbase π m hmk h0 (fun i hi => hp i (by omega))
    · -- the window `π (m-k) … π m` is a k-step path whose first k states are in P
      let σ : Nat → S := fun i => π (m - k + i)
      have hσ : Path T σ k := by
        intro i hi
        have := hp (m - k + i) (by omega)
        simpa [σ, Nat.add_assoc] using this
      have hP : ∀ i, i < k → P (σ i) := fun i hi => ih (m - k + i) (by omega) (by omega)
      have := hstep σ hσ hP
      simpa [σ, show m - k + k = m by omega] using this
  exact key n (Nat.le_refl n)

/-! ### The step case is the havoc query -/

/-- The solver query of the step case: *havoc* the loop state (the first state
of `π` is unconstrained — in particular it need not be reachable), assume `k`
consecutive `P`-states, and look for a violation after the next step. -/
def HavocQuerySat (T : S → S → Prop) (P : S → Prop) (k : Nat) : Prop :=
  ∃ π : Nat → S, Path T π k ∧ (∀ i, i < k → P (π i)) ∧ ¬ P (π k)

/-- The step premise is exactly "the havoc query is unsatisfiable". -/
theorem step_iff_havoc_query_unsat (T : S → S → Prop) (P : S → Prop) (k : Nat) :
    Step T P k ↔ ¬ HavocQuerySat T P k := by
  constructor
  · rintro h ⟨π, hp, hP, hn⟩
    exact hn (h π hp hP)
  · intro h π hp hP
    exact Classical.byContradiction fun hn => h ⟨π, hp, hP, hn⟩

/-- A loop state split into a part `F` the loop does not modify and the loop
-modified part `L`.  The engine havocs `L`; since the step query also leaves
`F` unconstrained, quantifying over the havocked start `(f, l)` is the same as
the step premise over the product state. -/
theorem step_iff_havoc_split {F L : Type} (T : F × L → F × L → Prop) (P : F × L → Prop)
    (k : Nat) :
    Step T P k ↔
      ∀ (f : F) (l : L) (π : Nat → F × L), π 0 = (f, l) → Path T π k →
        (∀ i, i < k → P (π i)) → P (π k) := by
  constructor
  · intro h f l π _ hp hP; exact h π hp hP
  · intro h π hp hP; exact h (π 0).1 (π 0).2 π rfl hp hP

/-- 1-induction is ordinary inductive-invariant reasoning. -/
theorem step_one_iff (T : S → S → Prop) (P : S → Prop) :
    Step T P 1 ↔ ∀ s s', P s → T s s' → P s' := by
  constructor
  · intro h s s' hs hT
    let π : Nat → S := fun i => if i = 0 then s else s'
    have := h π (fun i hi => by
      have : i = 0 := by omega
      subst this; simpa [π] using hT) (fun i hi => by
      have : i = 0 := by omega
      subst this; simpa [π] using hs)
    simpa [π] using this
  · intro h π hp hP
    exact h (π 0) (π 1) (hP 0 (by omega)) (hp 0 (by omega))

/-- Raising `k` never loses a proof: a `k`-inductive property is also
`(k+1)`-inductive. -/
theorem step_mono (T : S → S → Prop) (P : S → Prop) (k : Nat) (h : Step T P k) :
    Step T P (k + 1) := by
  intro π hp hP
  have := h (fun i => π (i + 1)) (fun i hi => by
    have := hp (i + 1) (by omega); simpa [Nat.add_assoc] using this)
    (fun i hi => hP (i + 1) (by omega))
  simpa using this

/-! ### Strengthening with an auxiliary invariant -/

/-- **Strengthened k-induction.**  If `P ∧ J` passes the base and step cases
(`J` is an auxiliary invariant, e.g. proposed by an AI model and filtered by
Houdini), then `P` alone holds in every reachable state. -/
theorem kinduction_strengthened (I : S → Prop) (T : S → S → Prop) (P J : S → Prop) (k : Nat)
    (hbase : Base I T (fun s => P s ∧ J s) k)
    (hstep : Step T (fun s => P s ∧ J s) k) :
    ∀ s, Reach I T s → P s :=
  fun s hs => (kinduction_sound I T (fun s => P s ∧ J s) k hbase hstep s hs).1

/-- Step case *relative to* an invariant `J` that is already known to hold on
all reachable states (for instance the Houdini survivors): the step query may
assume `J` on every state of the window. -/
def StepRel (T : S → S → Prop) (J P : S → Prop) (k : Nat) : Prop :=
  ∀ (π : Nat → S), Path T π k → (∀ i, i ≤ k → J (π i)) → (∀ i, i < k → P (π i)) → P (π k)

/-- k-induction relative to a proved invariant `J` is sound. -/
theorem kinduction_rel_sound (I : S → Prop) (T : S → S → Prop) (J P : S → Prop) (k : Nat)
    (hJ : ∀ s, Reach I T s → J s)
    (hbase : Base I T P k) (hstep : StepRel T J P k) :
    ∀ s, Reach I T s → P s := by
  rcases Nat.eq_zero_or_pos k with rfl | hk
  · -- k = 0: the step premise says every J-state satisfies P
    intro s hs
    exact hstep (fun _ => s) (fun i hi => absurd hi (Nat.not_lt_zero _))
      (fun _ _ => hJ s hs) (fun i hi => absurd hi (Nat.not_lt_zero _))
  -- restrict the system to states satisfying J
  let T' : S → S → Prop := fun s s' => T s s' ∧ J s ∧ J s'
  have hreach : ∀ s, Reach I T s → Reach (fun s => I s ∧ J s) T' s := by
    intro s h
    induction h with
    | init hs => exact Reach.init ⟨hs, hJ _ (Reach.init hs)⟩
    | @step s s' hr hT ih => exact Reach.step ih ⟨hT, hJ s hr, hJ s' (Reach.step hr hT)⟩
  have hbase' : Base (fun s => I s ∧ J s) T' P k :=
    fun π n hn h0 hp => hbase π n hn h0.1 (fun i hi => (hp i hi).1)
  have hstep' : Step T' P k := by
    intro π hp hP
    apply hstep π (fun i hi => (hp i hi).1) _ hP
    intro i hi
    by_cases h : i < k
    · exact (hp i h).2.1
    · have : i = k := by omega
      subst this
      have h1 := (hp (i - 1) (by omega)).2.2
      rwa [show i - 1 + 1 = i by omega] at h1
  exact fun s hs => kinduction_sound _ T' P k hbase' hstep' s (hreach s hs)

end PrismTechniques.KInduction
