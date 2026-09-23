/-
PRISM techniques: the Houdini invariant filter (roadmap 8.2, row "Houdini
invariant filter"; roadmap 4.2 "Loop invariant synthesis").

An AI model proposes candidate loop invariants.  PRISM never trusts them: the
Houdini algorithm drops every candidate that fails on an initial state, then
repeatedly drops every candidate that is not preserved by one transition when
the conjunction of the *current* candidates is assumed, until nothing changes.

The solver is modelled by two Boolean oracles.  Soundness of the filter needs
only that a `true` answer is correct (`InitSound`, `PresSound`): a solver that
says "not preserved" wrongly, times out, or answers "unknown" only makes
Houdini drop more candidates.  Maximality additionally needs complete oracles.

Proved:
* `houdini` is a total Lean function (termination is checked by Lean: each
  round that continues strictly shortens the list) — `houdini_rounds_le`
  bounds the number of solver rounds by `length + 1`;
* `houdini_inductive`: the conjunction of the survivors holds initially and is
  preserved by `T`;
* `houdini_sound`: every survivor holds on every reachable state, whatever the
  source of the candidates;
* `houdini_maximal`: with complete oracles, every inductive subset of the
  candidates survives, so the result is the largest inductive subset;
* `houdini_then_kinduction`: survivors feed k-induction soundly.
-/
import PrismTechniques.KInduction

namespace PrismTechniques.Houdini

open PrismTechniques.KInduction

variable {S C : Type}

/-- One round: keep the candidates that the preservation oracle accepts under
the assumption of all current candidates. -/
def round (pres : List C → C → Bool) (cs : List C) : List C :=
  cs.filter (pres cs)

/-- The Houdini fixpoint loop. -/
def loop (pres : List C → C → Bool) (cs : List C) : List C :=
  if (round pres cs).length < cs.length then loop pres (round pres cs) else cs
termination_by cs.length

/-- Houdini: drop candidates that fail initially, then iterate to a fixpoint. -/
def houdini (init : C → Bool) (pres : List C → C → Bool) (cands : List C) : List C :=
  loop pres (cands.filter init)

/-- Number of solver rounds the loop performs (for the cost bound). -/
def rounds (pres : List C → C → Bool) (cs : List C) : Nat :=
  if (round pres cs).length < cs.length then rounds pres (round pres cs) + 1 else 1
termination_by cs.length

theorem rounds_le (pres : List C → C → Bool) (cs : List C) :
    rounds pres cs ≤ cs.length + 1 := by
  induction cs using WellFounded.induction (measure List.length).wf with
  | _ cs ih =>
    rw [rounds]
    split
    · rename_i h
      have := ih (round pres cs) h
      omega
    · omega

/-- Houdini makes at most `n + 1` preservation rounds on `n` candidates. -/
theorem houdini_rounds_le (init : C → Bool) (pres : List C → C → Bool) (cands : List C) :
    rounds pres (cands.filter init) ≤ cands.length + 1 :=
  Nat.le_trans (rounds_le pres _) (Nat.succ_le_succ (List.length_filter_le _ _))

theorem loop_sublist (pres : List C → C → Bool) (cs : List C) : (loop pres cs).Sublist cs := by
  induction cs using WellFounded.induction (measure List.length).wf with
  | _ cs ih =>
    rw [loop]
    split
    · rename_i h
      exact (ih _ h).trans (List.filter_sublist)
    · exact List.Sublist.refl _

/-- At the fixpoint every survivor is accepted by the oracle under the
assumption of all survivors. -/
theorem loop_fixpoint (pres : List C → C → Bool) (cs : List C) :
    ∀ c ∈ loop pres cs, pres (loop pres cs) c = true := by
  induction cs using WellFounded.induction (measure List.length).wf with
  | _ cs ih =>
    rw [loop]
    split
    · rename_i h
      exact ih _ h
    · rename_i h
      intro c hc
      have hlen : (cs.filter (pres cs)).length = cs.length :=
        Nat.le_antisymm (List.length_filter_le _ _) (Nat.le_of_not_lt h)
      have := (List.filter_eq_self.1 ((List.filter_sublist).eq_of_length hlen))
      exact this c hc

/-- Candidate semantics and oracle specifications. -/
structure Oracles (S C : Type) where
  holds : C → S → Prop
  init : C → Bool
  pres : List C → C → Bool

/-- The conjunction of a list of candidates. -/
def Conj (O : Oracles S C) (cs : List C) (s : S) : Prop := ∀ c ∈ cs, O.holds c s

/-- A `true` answer of the initiation oracle is correct. -/
def InitSound (O : Oracles S C) (I : S → Prop) : Prop :=
  ∀ c, O.init c = true → ∀ s, I s → O.holds c s

/-- A `true` answer of the preservation oracle is correct. -/
def PresSound (O : Oracles S C) (T : S → S → Prop) : Prop :=
  ∀ cs c, O.pres cs c = true → ∀ s s', Conj O cs s → T s s' → O.holds c s'

/-- **Houdini soundness (inductiveness).**  The conjunction of the survivors
holds on initial states and is preserved by every transition. -/
theorem houdini_inductive (O : Oracles S C) (I : S → Prop) (T : S → S → Prop)
    (hI : InitSound O I) (hT : PresSound O T) (cands : List C) :
    let J := Conj O (houdini O.init O.pres cands)
    (∀ s, I s → J s) ∧ (∀ s s', J s → T s s' → J s') := by
  intro J
  constructor
  · intro s hs c hc
    have hsub := (loop_sublist O.pres (cands.filter O.init)).subset hc
    exact hI c (List.mem_filter.1 hsub).2 s hs
  · intro s s' hJ hst c hc
    exact hT _ c (loop_fixpoint O.pres _ c hc) s s' hJ hst

/-- **Houdini soundness.**  Every surviving candidate holds on every reachable
state — for *any* candidate list, e.g. one proposed by an AI model. -/
theorem houdini_sound (O : Oracles S C) (I : S → Prop) (T : S → S → Prop)
    (hI : InitSound O I) (hT : PresSound O T) (cands : List C) :
    ∀ c ∈ houdini O.init O.pres cands, ∀ s, Reach I T s → O.holds c s := by
  intro c hc s hs
  obtain ⟨h0, hstep⟩ := houdini_inductive O I T hI hT cands
  have : Conj O (houdini O.init O.pres cands) s := by
    induction hs with
    | init h => exact h0 _ h
    | step _ hst ih => exact hstep _ _ ih hst
  exact this c hc

/-! ### Maximality -/

/-- Complete oracles: the answers are exactly right. -/
def InitComplete (O : Oracles S C) (I : S → Prop) : Prop :=
  ∀ c, (∀ s, I s → O.holds c s) → O.init c = true

def PresComplete (O : Oracles S C) (T : S → S → Prop) : Prop :=
  ∀ cs c, (∀ s s', Conj O cs s → T s s' → O.holds c s') → O.pres cs c = true

/-- `A` is an inductive set of candidates. -/
def Inductive (O : Oracles S C) (I : S → Prop) (T : S → S → Prop) (A : List C) : Prop :=
  (∀ c ∈ A, ∀ s, I s → O.holds c s) ∧
  (∀ c ∈ A, ∀ s s', Conj O A s → T s s' → O.holds c s')

theorem loop_keeps (O : Oracles S C) (I : S → Prop) (T : S → S → Prop)
    (hP : PresComplete O T) (A : List C) (hA : Inductive O I T A) :
    ∀ cs : List C, (∀ c ∈ A, c ∈ cs) → ∀ c ∈ A, c ∈ loop O.pres cs := by
  intro cs
  induction cs using WellFounded.induction (measure List.length).wf with
  | _ cs ih =>
    intro hsub
    rw [loop]
    split
    · rename_i h
      apply ih _ h
      intro c hc
      refine List.mem_filter.2 ⟨hsub c hc, ?_⟩
      apply hP
      intro s s' hs hst
      exact hA.2 c hc s s' (fun d hd => hs d (hsub d hd)) hst
    · exact hsub

/-- **Houdini maximality.**  With complete oracles, every inductive subset of
the candidates survives; hence the result is the largest inductive subset
(it is itself inductive by `houdini_inductive`). -/
theorem houdini_maximal (O : Oracles S C) (I : S → Prop) (T : S → S → Prop)
    (hIc : InitComplete O I) (hPc : PresComplete O T) (cands A : List C)
    (hAsub : ∀ c ∈ A, c ∈ cands) (hA : Inductive O I T A) :
    ∀ c ∈ A, c ∈ houdini O.init O.pres cands := by
  apply loop_keeps O I T hPc A hA
  intro c hc
  exact List.mem_filter.2 ⟨hAsub c hc, hIc c (hA.1 c hc)⟩

/-! ### Houdini feeding k-induction -/

/-- The survivors may be assumed on every state of the k-induction step
window: if `P` passes the base case and the step case relative to the
survivors, `P` holds on every reachable state. -/
theorem houdini_then_kinduction (O : Oracles S C) (I : S → Prop) (T : S → S → Prop)
    (hI : InitSound O I) (hT : PresSound O T) (cands : List C) (P : S → Prop) (k : Nat)
    (hbase : Base I T P k)
    (hstep : StepRel T (Conj O (houdini O.init O.pres cands)) P k) :
    ∀ s, Reach I T s → P s :=
  kinduction_rel_sound I T _ P k
    (fun s hs c hc => houdini_sound O I T hI hT cands c hc s hs) hbase hstep

end PrismTechniques.Houdini
