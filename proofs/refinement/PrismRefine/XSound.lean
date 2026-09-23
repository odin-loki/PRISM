/-
PRISM refinement, extended fragment (`freeze`, `undef` under `freeze`,
direct calls, stack memory: `alloca`, `load`, `store`, `getelementptr`) —
headline theorems.  Both semantics run on the same memory model
(`XMem.lean`), starting from an empty memory.

For every module `M`, analysed function `F`, PIR function `P` and
certificate `C` such that `validB M F P C` holds (`pir_lean_check` evaluates
it for every function it reports `agree-ext`, after checking that `P` is
exactly the C++ translator's output), every argument vector, every oracle
`ω` (the arbitrary values of `freeze`; PIR `havoc` draws the same ones) and
every bound `n` on the executed block segments:

* `pir_sound_x`: if `F`, under the LangRef semantics (`lRunXF`), reaches
  undefined behaviour or creates poison, the PIR program fails a check;
* `pir_sound_all_x`: if no PIR run fails, for any oracle and any bound,
  no LangRef run has UB or creates poison, for any oracle and any bound;
* `pir_faithful_ret_x` / `_fail_x` / `_fuel_x`, `pir_no_stop_x`: every PIR
  outcome is a LangRef outcome for the same oracle (or `F` is ill-formed on
  that path, `stuck`).
-/
import PrismRefine.XRun
import PrismRefine.XLazy

namespace PrismRefine

open PrismSem

variable {M : XMod} {F : XFunc} {P : PFunc} {C : List IInfo}

theorem pir_sound_x (h : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat)
    (hbad : (lRunXF M F args ω n).bad = true) : xpRunF P args ω n = .fail := by
  have e := translateX_exact h args ω n
  have l := strict_lazyX M F args ω n
  cases hs : sRunXF M F args ω n with
  | ret v => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | fuel => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | stuck => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | ub => rw [hs] at e; exact e

theorem pir_sound_all_x (h : validB M F P C = true) (args : List Nat)
    (hsafe : ∀ ω n, xpRunF P args ω n ≠ .fail) : ∀ ω n, (lRunXF M F args ω n).bad = false := by
  intro ω n
  cases hb : (lRunXF M F args ω n).bad
  · rfl
  · exact absurd (pir_sound_x h args ω n hb) (hsafe ω n)

theorem pir_faithful_ret_x (h : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat)
    {v : Option Nat} (hp : xpRunF P args ω n = .ret v) :
    lRunXF M F args ω n = .ret v false ∨ lRunXF M F args ω n = .stuck := by
  have e := translateX_exact h args ω n
  have l := strict_lazyX M F args ω n
  cases hs : sRunXF M F args ω n with
  | ret v' =>
    rw [hs] at e l; simp only [OSim] at e; simp only [OL] at l
    rw [hp] at e; cases e; exact .inl l
  | ub => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | fuel => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_faithful_fail_x (h : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat)
    (hp : xpRunF P args ω n = .fail) :
    (lRunXF M F args ω n).bad = true ∨ lRunXF M F args ω n = .stuck := by
  have e := translateX_exact h args ω n
  have l := strict_lazyX M F args ω n
  cases hs : sRunXF M F args ω n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | ub => rw [hs] at l; simp only [OL] at l; exact l
  | fuel => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_faithful_fuel_x (h : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat)
    (hp : xpRunF P args ω n = .fuel) :
    lRunXF M F args ω n = .fuel false ∨ lRunXF M F args ω n = .stuck := by
  have e := translateX_exact h args ω n
  have l := strict_lazyX M F args ω n
  cases hs : sRunXF M F args ω n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | ub => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | fuel => rw [hs] at l; simp only [OL] at l; exact .inl l
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_no_stop_x (h : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat)
    (hp : xpRunF P args ω n = .stop ∨ xpRunF P args ω n = .blocked) :
    lRunXF M F args ω n = .stuck := by
  have e := translateX_exact h args ω n
  have l := strict_lazyX M F args ω n
  cases hs : sRunXF M F args ω n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | ub => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | fuel => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact l

end PrismRefine
