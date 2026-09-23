/-
PRISM refinement — headline theorems.

For every LLVM function `F` in the fragment that the translator accepts
(`translate F = .ok P`, the translator mirroring `src/prism/pir/translate.cpp`),
every argument vector and every bound `n` on the number of executed blocks:

* `pir_sound`: if `F`, under the LLVM LangRef semantics, reaches undefined
  behaviour or creates poison, then the PIR program `P` fails a check.  This
  is the direction that makes PIR proofs sound for the LLVM code: a proof
  that no PIR check can fail (for all inputs and all `n`) is a proof that `F`
  has no undefined behaviour and never creates poison.
* `pir_faithful_ret` / `pir_faithful_fail` / `pir_faithful_fuel`: every PIR
  outcome is an outcome of `F`: a returned value is `F`'s value (no UB, no
  poison on the way); a failed check means `F` has UB or creates poison; in
  each case unless `F` is ill-formed on that path (`stuck`: an undefined
  register, a missing phi entry — IR the LLVM verifier rejects).
* `pir_no_stop`: the PIR program never blocks or stops on its own.
* `checks_eq_ubBin`: on the flags the PIR semantics of proofs/semantics
  models (`nsw`, `nuw`, and the implicit division / shift UB), the checks the
  C++ translator inserts test exactly `PrismSem.ubBin`, the UB predicate the
  PIR instrumentation theorems (`PrismSem.instr_fail_ub_iff`) are proved for.
-/
import PrismRefine.Refine
import PrismRefine.Lazy

namespace PrismRefine

open PrismSem

theorem pir_sound {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat) (n : Nat)
    (hbad : (lRunF F args n).bad = true) : pRunF P args n = .fail := by
  have e := translate_exact h args n
  have l := strict_lazy F args n
  cases hs : sRunF F args n with
  | ret v => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | fuel => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | stuck => rw [hs] at l; simp only [OL] at l; rw [l] at hbad; simp [LOut.bad] at hbad
  | ub => rw [hs] at e; exact e

/-- Unbounded form: a PIR proof for all bounds is a proof about all LLVM runs. -/
theorem pir_sound_all {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (hsafe : ∀ n, pRunF P args n ≠ .fail) : ∀ n, (lRunF F args n).bad = false := by
  intro n
  cases hb : (lRunF F args n).bad
  · rfl
  · exact absurd (pir_sound h args n hb) (hsafe n)

theorem pir_faithful_ret {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (n : Nat) {v : Option Nat} (hp : pRunF P args n = .ret v) :
    lRunF F args n = .ret v false ∨ lRunF F args n = .stuck := by
  have e := translate_exact h args n
  have l := strict_lazy F args n
  cases hs : sRunF F args n with
  | ret v' =>
    rw [hs] at e l; simp only [OSim] at e; simp only [OL] at l
    rw [hp] at e; cases e; exact .inl l
  | ub => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | fuel => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_faithful_fail {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (n : Nat) (hp : pRunF P args n = .fail) :
    (lRunF F args n).bad = true ∨ lRunF F args n = .stuck := by
  have e := translate_exact h args n
  have l := strict_lazy F args n
  cases hs : sRunF F args n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | ub => rw [hs] at l; simp only [OL] at l; exact l
  | fuel => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_faithful_fuel {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (n : Nat) (hp : pRunF P args n = .fuel) :
    lRunF F args n = .fuel false ∨ lRunF F args n = .stuck := by
  have e := translate_exact h args n
  have l := strict_lazy F args n
  cases hs : sRunF F args n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | ub => rw [hs] at e; simp only [OSim] at e; rw [hp] at e; cases e
  | fuel => rw [hs] at l; simp only [OL] at l; exact .inl l
  | stuck => rw [hs] at l; simp only [OL] at l; exact .inr l

theorem pir_no_stop {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (n : Nat) (hp : pRunF P args n = .stop ∨ pRunF P args n = .blocked) :
    lRunF F args n = .stuck := by
  have e := translate_exact h args n
  have l := strict_lazy F args n
  cases hs : sRunF F args n with
  | ret v' => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | ub => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | fuel => rw [hs] at e; simp only [OSim] at e; rcases hp with hp | hp <;> rw [hp] at e <;> cases e
  | stuck => rw [hs] at l; simp only [OL] at l; exact l

/-! ## The C++ instrumentation list and `PrismSem.ubBin` -/

/-- On the flags `PrismSem.Flags` models (`nsw`/`nuw` on `add`/`sub`/`mul`,
and the division and shift-amount UB), the checks `translate.cpp` inserts
fire exactly when `PrismSem.ubBin` — the UB definition of the PIR semantics —
holds. -/
theorem checks_eq_ubBin (σ : Store) (op : BinOp) (fl : LFlags) (w : Nat) (A B : Arg)
    (hA : A.width = w) (hB : B.width = w) (hf : flagsOk op fl = true)
    (hx : fl.exact = false) (hd : fl.disjoint = false) (hc : fl.csigned = false)
    (hs : op = .shl → fl.nsw = false ∧ fl.nuw = false) :
    (checks op fl w A B).any (Chk.bad σ) =
      ubBin op { nsw := fl.nsw, nuw := fl.nuw, total := false } (bv w (A.get σ)) (bv w (B.get σ)) := by
  rw [checks_bad σ op fl w A B hA hB hf]
  generalize A.get σ = x
  generalize B.get σ = y
  obtain ⟨nsw, nuw, ex, dj, cs⟩ := fl
  simp only at hx hd hc hs
  subst hx hd hc
  cases op <;>
    simp only [binUB, cUB, binPoison, ubBin, sInRange, BitVec.saddOverflow, BitVec.ssubOverflow,
      BitVec.smulOverflow, BitVec.uaddOverflow, BitVec.usubOverflow, BitVec.umulOverflow,
      Bool.false_or, Bool.or_false, Bool.false_and] <;>
    (try rfl)
  all_goals first
    | (obtain ⟨rfl, rfl⟩ := hs rfl; simp)
    | (cases nsw <;> cases nuw <;> simp only [Bool.true_and, Bool.false_and, Bool.false_or,
        Bool.or_false] <;> (try rfl) <;> (rw [Bool.eq_iff_iff]; simp; omega))

/-- For every flag combination, the C++ checks cover `PrismSem.ubBin` (and
test more: `exact`, `disjoint`, `nneg`, `shl nsw/nuw`, C signed `<<`). -/
theorem checks_cover_ubBin (σ : Store) (op : BinOp) (fl : LFlags) (w : Nat) (A B : Arg)
    (hA : A.width = w) (hB : B.width = w) (hf : flagsOk op fl = true)
    (hub : ubBin op { nsw := fl.nsw, nuw := fl.nuw, total := false }
      (bv w (A.get σ)) (bv w (B.get σ)) = true) :
    (checks op fl w A B).any (Chk.bad σ) = true := by
  rw [checks_bad σ op fl w A B hA hB hf]
  revert hub
  generalize A.get σ = x
  generalize B.get σ = y
  obtain ⟨nsw, nuw, ex, dj, cs⟩ := fl
  intro hub
  cases op <;> cases nsw <;> cases nuw <;>
    simp [binUB, cUB, binPoison, ubBin, sInRange, BitVec.saddOverflow, BitVec.ssubOverflow,
      BitVec.smulOverflow, BitVec.uaddOverflow, BitVec.usubOverflow, BitVec.umulOverflow] at hub ⊢
  all_goals first | exact hub | grind

end PrismRefine
