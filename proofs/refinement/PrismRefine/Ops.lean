/-
PRISM refinement — the C++ tests compute the LangRef conditions.

`translate.cpp` tests each flag with an i1 PIR operator whose value is
computed as `eval_op` does (`PrismRefine.testVal`: e.g. `sadd.ovf` is
"the wrapped sum, read as signed, differs from the exact sum";
`shl.nuw.ovf` is "`(a << b) >>u b != a`").  The LLVM semantics
(`Llvm.lean`) states the same conditions arithmetically (the exact result is
out of range; a set bit is shifted out; ...).  This file proves, for every
width and every operand value, that each test is exactly its condition.
-/
import PrismRefine.Translate

namespace PrismRefine

open PrismSem

private theorem toInt_range {w : Nat} (c : BitVec w) :
    -2 ^ (w - 1) ≤ c.toInt ∧ c.toInt < 2 ^ (w - 1) :=
  ⟨BitVec.le_toInt c, BitVec.toInt_lt⟩

theorem test_sadd (w x y : Nat) :
    ovfTest .sadd w x y = !sInRange w ((bv w x).toInt + (bv w y).toInt) := by
  simp only [ovfTest]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : -2 ^ (w - 1) ≤ a.toInt + b.toInt ∧ a.toInt + b.toInt < 2 ^ (w - 1)
  · have hn : ¬ a.saddOverflow b = true := by
      simp only [BitVec.saddOverflow]; simp; omega
    rw [BitVec.toInt_add_of_not_saddOverflow hn]
    simp [sInRange, h]
  · have := toInt_range (a + b)
    have hs : sInRange w (a.toInt + b.toInt) = false := by
      simp only [sInRange, decide_eq_false_iff_not]; exact h
    rw [hs, Bool.not_false, bne_iff_ne, ne_eq]
    omega

theorem test_ssub (w x y : Nat) :
    ovfTest .ssub w x y = !sInRange w ((bv w x).toInt - (bv w y).toInt) := by
  simp only [ovfTest]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : -2 ^ (w - 1) ≤ a.toInt - b.toInt ∧ a.toInt - b.toInt < 2 ^ (w - 1)
  · have hn : ¬ a.ssubOverflow b = true := by
      simp only [BitVec.ssubOverflow]; simp; omega
    rw [BitVec.toInt_sub_of_not_ssubOverflow hn]
    simp [sInRange, h]
  · have := toInt_range (a - b)
    have hs : sInRange w (a.toInt - b.toInt) = false := by
      simp only [sInRange, decide_eq_false_iff_not]; exact h
    rw [hs, Bool.not_false, bne_iff_ne, ne_eq]
    omega

theorem test_smul (w x y : Nat) :
    ovfTest .smul w x y = !sInRange w ((bv w x).toInt * (bv w y).toInt) := by
  simp only [ovfTest]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : -2 ^ (w - 1) ≤ a.toInt * b.toInt ∧ a.toInt * b.toInt < 2 ^ (w - 1)
  · have hn : ¬ a.smulOverflow b = true := by
      simp only [BitVec.smulOverflow]; simp; omega
    rw [BitVec.toInt_mul_of_not_smulOverflow hn]
    simp [sInRange, h]
  · have := toInt_range (a * b)
    have hs : sInRange w (a.toInt * b.toInt) = false := by
      simp only [sInRange, decide_eq_false_iff_not]; exact h
    rw [hs, Bool.not_false, bne_iff_ne, ne_eq]
    omega

theorem test_uadd (w x y : Nat) :
    ovfTest .uadd w x y = decide (2 ^ w ≤ (bv w x).toNat + (bv w y).toNat) := by
  simp only [ovfTest]
  generalize bv w x = a
  generalize bv w y = b
  have ha := a.isLt
  have hb := b.isLt
  rw [BitVec.toNat_add]
  by_cases h : a.toNat + b.toNat < 2 ^ w
  · rw [Nat.mod_eq_of_lt h]; simp; omega
  · have : (a.toNat + b.toNat) % 2 ^ w = a.toNat + b.toNat - 2 ^ w := by
      rw [Nat.mod_eq_sub_mod (by omega), Nat.mod_eq_of_lt (by omega)]
    rw [this]; simp; omega

theorem test_usub (w x y : Nat) :
    ovfTest .usub w x y = decide ((bv w x).toNat < (bv w y).toNat) := rfl

theorem test_umul (w x y : Nat) :
    ovfTest .umul w x y = decide (2 ^ w ≤ (bv w x).toNat * (bv w y).toNat) := by
  simp only [ovfTest]
  have : 0 < 2 ^ w := Nat.two_pow_pos w
  simp; omega

/-! ### Shifts -/

theorem pow_split (w y : Nat) (hy : y ≤ w) : 2 ^ w = 2 ^ (w - y) * 2 ^ y := by
  rw [← Nat.pow_add]; congr 1; omega

/-- `shl nuw`: `(a << y) >>u y != a` iff a set bit is shifted out. -/
theorem shlNuw_core {w : Nat} (a : BitVec w) (y : Nat) (hy : y < w) :
    ((a <<< y) >>> y != a) = decide (2 ^ w ≤ a.toNat * 2 ^ y) := by
  have hN := a.isLt
  have hsplit := pow_split w y (by omega)
  have hP : 0 < 2 ^ y := Nat.two_pow_pos y
  rw [Bool.eq_iff_iff, bne_iff_ne, ne_eq, BitVec.toNat_eq, BitVec.toNat_ushiftRight, BitVec.toNat_shiftLeft,
    Nat.shiftLeft_eq, Nat.shiftRight_eq_div_pow]
  by_cases h : a.toNat * 2 ^ y < 2 ^ w
  · rw [Nat.mod_eq_of_lt h, Nat.mul_div_cancel _ hP]; simp; omega
  · have h1 : a.toNat * 2 ^ y % 2 ^ w / 2 ^ y < 2 ^ (w - y) := by
      rw [Nat.div_lt_iff_lt_mul hP, ← hsplit]; exact Nat.mod_lt _ (Nat.two_pow_pos w)
    have h2 : 2 ^ (w - y) ≤ a.toNat := by
      apply Nat.le_of_mul_le_mul_right (c := 2 ^ y) _ hP
      rw [← hsplit]; omega
    simp; omega

/-- `lshr exact`: `(a >>u y) << y != a` iff a set bit is shifted out. -/
theorem lostL_core {w : Nat} (a : BitVec w) (y : Nat) :
    ((a >>> y) <<< y != a) = (a.toNat % 2 ^ y != 0) := by
  have hN := a.isLt
  have hP : 0 < 2 ^ y := Nat.two_pow_pos y
  rw [Bool.eq_iff_iff, bne_iff_ne, ne_eq, BitVec.toNat_eq]
  simp only [BitVec.toNat_shiftLeft, BitVec.toNat_ushiftRight, Nat.shiftLeft_eq,
    Nat.shiftRight_eq_div_pow]
  have hd := Nat.div_add_mod a.toNat (2 ^ y)
  have hle : a.toNat / 2 ^ y * 2 ^ y ≤ a.toNat := Nat.div_mul_le_self _ _
  rw [Nat.mod_eq_of_lt (by omega), bne_iff_ne]
  have hc := Nat.mul_comm (a.toNat / 2 ^ y) (2 ^ y)
  omega

/-- `ashr exact` tests the same low bits as `lshr exact` (for `y < w`). -/
theorem ashr_shl_eq {w : Nat} (a : BitVec w) (y : Nat) :
    (a.sshiftRight y) <<< y = (a >>> y) <<< y := by
  apply BitVec.eq_of_getLsbD_eq
  intro i hi
  simp only [BitVec.getLsbD_shiftLeft, BitVec.getLsbD_sshiftRight, BitVec.getLsbD_ushiftRight]
  by_cases h : i < y
  · simp [h]
  · have h1 : y + (i - y) = i := by omega
    simp [h, hi, h1]
    intro _; omega

theorem msb_iff_toInt_neg {w : Nat} (a : BitVec w) : a.msb = decide (a.toInt < 0) :=
  BitVec.msb_eq_toInt

/-- `shl` under C's rule (`csigned`): the C++ test is "negative, or a set bit
reaches the sign bit" (`a >>u (w-1-y) != 0`); the condition is "negative, or
`a * 2^y` is not representable". -/
theorem shlS_core {w : Nat} (a : BitVec w) (y : Nat) (hy : y < w) :
    (a.msb || (a >>> (w - 1 - y)) != 0#w) =
      (decide (a.toInt < 0) || !decide (a.toInt * 2 ^ y < 2 ^ (w - 1))) := by
  rw [BitVec.msb_eq_toInt]
  by_cases hneg : a.toInt < 0
  · simp [hneg]
  · simp only [hneg, decide_false, Bool.false_or]
    have hmsb : a.msb = false := by rw [BitVec.msb_eq_toInt]; simp; omega
    have hA := BitVec.toInt_eq_toNat_of_msb hmsb
    rw [hA]
    have hsplit : 2 ^ (w - 1) = 2 ^ (w - 1 - y) * 2 ^ y := pow_split (w - 1) y (by omega)
    have hP : 0 < 2 ^ y := Nat.two_pow_pos y
    have hQ : 0 < 2 ^ (w - 1 - y) := Nat.two_pow_pos _
    rw [Bool.eq_iff_iff, bne_iff_ne, ne_eq, BitVec.toNat_eq, BitVec.toNat_ushiftRight,
      Nat.shiftRight_eq_div_pow]
    simp only [BitVec.toNat_ofNat, Nat.zero_mod, Bool.not_eq_true', decide_eq_false_iff_not]
    have key : (a.toNat / 2 ^ (w - 1 - y) = 0) ↔ a.toNat * 2 ^ y < 2 ^ (w - 1) := by
      rw [Nat.div_eq_zero_iff_lt hQ, hsplit]
      exact (Nat.mul_lt_mul_right hP).symm
    have hc : ((a.toNat : Int) * 2 ^ y < 2 ^ (w - 1)) ↔ a.toNat * 2 ^ y < 2 ^ (w - 1) := by
      constructor
      · intro h; exact_mod_cast h
      · intro h; exact_mod_cast h
    rw [hc, ← key]

/-- `shl nsw`: `(a << y) >>s y != a` iff `a * 2^y` (signed) is out of range. -/
theorem shlNsw_core {w : Nat} (a : BitVec w) (y : Nat) (hy : y < w) :
    ((a <<< y).sshiftRight y != a) = !sInRange w (a.toInt * 2 ^ y) := by
  have hw : (2 : Int) ^ w = 2 * 2 ^ (w - 1) := by
    rw [← Int.pow_succ']; congr 1; omega
  have hPM : (2 : Int) ^ w = 2 ^ y * 2 ^ (w - y) := by
    rw [← Int.pow_add]; congr 1; omega
  have hP : (0 : Int) < 2 ^ y := Int.pow_pos (by decide)
  -- X = (A * P).bmod 2^w
  have hX : (a <<< y).toInt = (a.toInt * 2 ^ y).bmod (2 ^ w) := by
    rw [BitVec.toInt_shiftLeft, Nat.shiftLeft_eq]
    simp only [Int.bmod_def]
    have hm : ((a.toNat * 2 ^ y : Nat) : Int) % ((2 ^ w : Nat) : Int) =
        a.toInt * 2 ^ y % ((2 ^ w : Nat) : Int) := by
      rw [BitVec.toInt_eq_toNat_cond]
      split
      · push_cast; rfl
      · push_cast
        rw [show ((a.toNat : Int) - 2 ^ w) * 2 ^ y = (a.toNat : Int) * 2 ^ y + (-(2 ^ y)) * 2 ^ w by
          grind, Int.add_mul_emod_self_right]
    rw [hm]
  have hR := toInt_range (a <<< y)
  have hA := toInt_range a
  rw [Bool.eq_iff_iff, bne_iff_ne, ne_eq, ← BitVec.toInt_inj, BitVec.toInt_sshiftRight,
    Int.shiftRight_eq_div_pow]
  push_cast
  by_cases hin : -2 ^ (w - 1) ≤ a.toInt * 2 ^ y ∧ a.toInt * 2 ^ y < 2 ^ (w - 1)
  · have hb : (a.toInt * 2 ^ y).bmod (2 ^ w) = a.toInt * 2 ^ y := by
      apply Int.bmod_eq_of_le
      · push_cast; rw [hw]; omega
      · push_cast; rw [hw]; omega
    rw [hX, hb, Int.mul_ediv_cancel _ (by omega)]
    simp [sInRange, hin]
  · have hs : sInRange w (a.toInt * 2 ^ y) = false := by
      simp only [sInRange, decide_eq_false_iff_not]; exact hin
    rw [hs]
    simp only [Bool.not_false, iff_true]
    intro heq
    apply hin
    -- X % P = 0, so X = P * (X / P) = P * A
    have hXP : (a <<< y).toInt % 2 ^ y = 0 := by
      have h1 : (a <<< y).toInt % 2 ^ w % 2 ^ y = (a <<< y).toInt % 2 ^ y :=
        Int.emod_emod_of_dvd _ ⟨2 ^ (w - y), hPM⟩
      have h2 : (a <<< y).toInt % 2 ^ w = a.toInt * 2 ^ y % 2 ^ w := by
        rw [hX]; have := @Int.bmod_emod (a.toInt * 2 ^ y) (2 ^ w); push_cast at this; exact this
      rw [← h1, h2, Int.emod_emod_of_dvd _ ⟨2 ^ (w - y), hPM⟩, Int.mul_emod_left]
    have hd := Int.mul_ediv_add_emod (a <<< y).toInt (2 ^ y)
    rw [hXP, heq, Int.add_zero] at hd
    rw [Int.mul_comm, hd]; exact hR

/-! ### Test operators at the level of `testVal` -/

@[simp] theorem truthN_ofBool (b : Bool) : truthN (BitVec.ofBool b).toNat = b := by
  cases b <;> rfl

@[simp] theorem truthN_boolToNat (b : Bool) : truthN b.toNat = b := by
  cases b <;> rfl

@[simp] theorem truthN_mod2 (v : Nat) : truthN (v % 2) = truthN v := by
  simp [truthN]

theorem icmpVal_lt2 (p : Pred) (w x y : Nat) : icmpVal p w x y < 2 := by
  unfold icmpVal; exact (BitVec.ofBool _).isLt

@[simp] theorem truthN_icmpVal (p : Pred) (w x y : Nat) :
    truthN (icmpVal p w x y) = evalPred p (bv w x) (bv w y) := by
  simp only [icmpVal, truthN_ofBool]

theorem test_shlNuw (w x y : Nat) :
    testVal .shlNuwOvf w x y =
      (decide (w ≤ (bv w y).toNat) || decide (2 ^ w ≤ (bv w x).toNat * 2 ^ (bv w y).toNat)) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : w ≤ b.toNat
  · simp [h]
  · simp only [h, decide_false, Bool.false_or]
    exact shlNuw_core _ _ (by omega)

theorem test_shlNsw (w x y : Nat) :
    testVal .shlNswOvf w x y =
      (decide (w ≤ (bv w y).toNat) || !sInRange w ((bv w x).toInt * 2 ^ (bv w y).toNat)) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : w ≤ b.toNat
  · simp [h]
  · simp only [h, decide_false, Bool.false_or]
    exact shlNsw_core _ _ (by omega)

theorem test_shlS (w x y : Nat) :
    testVal .shlSOvf w x y =
      (decide (w ≤ (bv w y).toNat) || (decide ((bv w x).toInt < 0) ||
        !decide ((bv w x).toInt * 2 ^ (bv w y).toNat < 2 ^ (w - 1)))) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : w ≤ b.toNat
  · simp [h]
  · simp only [h, decide_false, Bool.false_or]
    exact shlS_core _ _ (by omega)

theorem test_lostL (w x y : Nat) :
    testVal .lostBitsL w x y =
      (decide (w ≤ (bv w y).toNat) || ((bv w x).toNat % 2 ^ (bv w y).toNat != 0)) := by
  simp only [testVal]
  rw [lostL_core]

theorem test_lostA (w x y : Nat) :
    testVal .lostBitsA w x y =
      (decide (w ≤ (bv w y).toNat) || ((bv w x).toNat % 2 ^ (bv w y).toNat != 0)) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : w ≤ b.toNat
  · simp [h]
  · simp only [h, decide_false, Bool.false_or]
    rw [ashr_shl_eq, lostL_core]

theorem test_inexactU (w x y : Nat) :
    (bv w y == 0#w || testVal .inexactU w x y) =
      (bv w y == 0#w || ((bv w x).toNat % (bv w y).toNat != 0)) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h : b = 0#w
  · simp [h]
  · have : (a % b != 0#w) = (a.toNat % b.toNat != 0) := by
      rw [Bool.eq_iff_iff, bne_iff_ne, bne_iff_ne, ne_eq, ne_eq, BitVec.toNat_eq,
        BitVec.toNat_umod]
      simp
    have hb : (b == 0#w) = false := by simpa using h
    have hb' : (b != 0#w) = true := by simpa using h
    rw [this, hb, hb', Bool.true_and]

theorem test_inexactS (w x y : Nat) :
    ((bv w y == 0#w || (bv w x == BitVec.intMin w && bv w y == BitVec.allOnes w)) ||
        testVal .inexactS w x y) =
      ((bv w y == 0#w || (bv w x == BitVec.intMin w && bv w y == BitVec.allOnes w)) ||
        !decide ((bv w y).toInt ∣ (bv w x).toInt)) := by
  simp only [testVal]
  generalize bv w x = a
  generalize bv w y = b
  by_cases h1 : b = 0#w
  · simp [h1]
  by_cases h2 : a = BitVec.intMin w ∧ b = BitVec.allOnes w
  · simp [h2]
  have e : (a.srem b != 0#w) = !decide (b.toInt ∣ a.toInt) := by
    rw [Bool.eq_iff_iff]
    simp only [bne_iff_ne, ne_eq, Bool.not_eq_true', decide_eq_false_iff_not]
    rw [Int.dvd_iff_tmod_eq_zero, ← BitVec.toInt_inj, BitVec.toInt_srem, BitVec.toInt_zero]
  have h2' : (a == BitVec.intMin w && b == BitVec.allOnes w) = false := by
    simp only [Bool.and_eq_false_iff, beq_eq_false_iff_ne]
    by_cases ha : a = BitVec.intMin w
    · right; exact fun hb => h2 ⟨ha, hb⟩
    · left; exact ha
  simp [h1, h2', e]

theorem slt_zero_msb {w : Nat} (a : BitVec w) : evalPred .slt a (bv w 0) = a.msb := by
  simp only [evalPred, BitVec.slt, BitVec.msb_eq_toInt]
  simp

/-! ### The checks of one instruction test exactly its UB / poison condition -/

/-- A check fires. -/
def Chk.bad (σ : Store) : Chk → Bool
  | .p op a b _ _ => truthN (evalOp σ op 1 [a, b])
  | .disj a b w => (bv w (a.get σ) &&& bv w (b.get σ)) != 0#w

theorem bad_test (σ : Store) (op : POp) (a b : Arg) (pr cl : String)
    (h1 : ∀ o, op ≠ .bin o) (h2 : ∀ p, op ≠ .cmp p) (h3 : op ≠ .select) (h4 : ∀ k, op ≠ .cast k) :
    Chk.bad σ (.p op a b pr cl) = testVal op a.width (a.get σ) (b.get σ) := by
  cases op <;> first
    | (exfalso; first | exact h1 _ rfl | exact h2 _ rfl | exact h3 rfl | exact h4 _ rfl)
    | simp [Chk.bad, evalOp]

theorem bad_cmp (σ : Store) (p : Pred) (a b : Arg) (pr cl : String) :
    Chk.bad σ (.p (.cmp p) a b pr cl) = evalPred p (bv a.width (a.get σ)) (bv a.width (b.get σ)) := by
  simp [Chk.bad, evalOp]

theorem bv_zero (w : Nat) : bv w 0 = 0#w := rfl

theorem any_opt (f : Chk → Bool) (b : Bool) (c : Chk) :
    (opt b c).any f = (b && f c) := by
  cases b <;> simp [opt]

theorem checks_bad (σ : Store) (op : BinOp) (fl : LFlags) (w : Nat) (A B : Arg)
    (hA : A.width = w) (hB : B.width = w) (hf : flagsOk op fl = true) :
    (checks op fl w A B).any (Chk.bad σ) =
      (binUB op w (A.get σ) (B.get σ) || cUB op fl w (A.get σ) (B.get σ) ||
        binPoison op fl w (A.get σ) (B.get σ)) := by
  obtain ⟨nsw, nuw, ex, dj, cs⟩ := fl
  have hs : ∀ (op : POp) pr cl, (∀ o, op ≠ .bin o) → (∀ p, op ≠ .cmp p) → op ≠ .select →
      (∀ k, op ≠ .cast k) → Chk.bad σ (.p op A B pr cl) = testVal op w (A.get σ) (B.get σ) := by
    intro op pr cl h1 h2 h3 h4; rw [bad_test σ op A B pr cl h1 h2 h3 h4, hA]
  have hc : ∀ p pr cl, Chk.bad σ (.p (.cmp p) B (.c w 0) pr cl) =
      evalPred p (bv w (B.get σ)) (bv w 0) := by
    intro p pr cl; rw [bad_cmp, hB]; rfl
  have hd : Chk.bad σ (.disj A B w) = ((bv w (A.get σ) &&& bv w (B.get σ)) != 0#w) := rfl
  generalize A.get σ = x at hs hc hd ⊢
  generalize B.get σ = y at hs hc hd ⊢
  cases op <;> simp only [flagsOk, Bool.and_eq_true, Bool.not_eq_true'] at hf
  · -- add
    simp only [checks, List.any_append, any_opt]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    simp [testVal, test_sadd, test_uadd, binUB, cUB, binPoison]
  · -- sub
    simp only [checks, List.any_append, any_opt]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    simp [testVal, test_ssub, test_usub, binUB, cUB, binPoison]
  · -- mul
    simp only [checks, List.any_append, any_opt]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    simp [testVal, test_smul, test_umul, binUB, cUB, binPoison]
  · -- udiv
    simp only [checks, List.any_append, any_opt, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hc]
    simp only [bv_zero, evalPred, binUB, cUB, binPoison, Bool.or_false]
    cases ex
    · simp
    · simp only [Bool.true_and, Bool.false_or]; exact test_inexactU w x y
  · -- sdiv
    simp only [checks, List.any_append, any_opt, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp),
      hc]
    simp only [bv_zero, evalPred, binUB, cUB, binPoison, Bool.or_false]
    cases ex
    · simp [testVal]
    · simp only [Bool.true_and]
      have := test_inexactS w x y
      simp only [testVal] at this ⊢
      exact this
  · -- urem
    simp only [checks, List.any_cons, List.any_nil, Bool.or_false]
    rw [hc]
    simp [bv_zero, evalPred, binUB, cUB, binPoison]
  · -- srem
    simp only [checks, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hc]
    simp [bv_zero, evalPred, binUB, cUB, binPoison, testVal]
  · -- shl
    simp only [checks, List.any_append, any_opt, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp),
      hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    rw [test_shlS, test_shlNsw, test_shlNuw]
    simp only [binUB, cUB, binPoison, Bool.false_or, testVal]
    generalize bv w x = a
    generalize bv w y = b
    by_cases h : w ≤ b.toNat
    · simp [testVal, h]
    · have h' : decide (b.toNat < w) = true := by simp; omega
      simp only [testVal, h, h', decide_false, Bool.false_or, Bool.true_and]
      cases cs <;> cases nsw <;> cases nuw <;> simp [Bool.or_assoc]
  · -- lshr
    simp only [checks, List.any_append, any_opt, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    rw [test_lostL]
    simp only [binUB, cUB, binPoison, Bool.false_or, testVal]
    generalize bv w x = a
    generalize bv w y = b
    by_cases h : w ≤ b.toNat
    · simp [testVal, h]
    · simp only [testVal, h, decide_false, Bool.false_or]
  · -- ashr
    simp only [checks, List.any_append, any_opt, List.any_cons, List.any_nil, Bool.or_false]
    rw [hs _ _ _ (by simp) (by simp) (by simp) (by simp), hs _ _ _ (by simp) (by simp) (by simp) (by simp)]
    rw [test_lostA]
    simp only [binUB, cUB, binPoison, Bool.false_or, testVal]
    generalize bv w x = a
    generalize bv w y = b
    by_cases h : w ≤ b.toNat
    · simp [testVal, h]
    · simp only [testVal, h, decide_false, Bool.false_or]
  · -- and
    simp [checks, binUB, cUB, binPoison]
  · -- or
    simp only [checks, any_opt, hd]
    simp [binUB, cUB, binPoison]
  · -- xor
    simp [checks, binUB, cUB, binPoison]

/-- `zext nneg`: the inserted `slt a, 0` test is exactly the poison
condition. -/
theorem nneg_bad (σ : Store) (fw : Nat) (A : Arg) (hA : A.width = fw) (nneg : Bool) :
    (opt nneg (.p (.cmp .slt) A (.c fw 0) "nneg" "UB-POISON")).any (Chk.bad σ) =
      castPoison .zext nneg fw (A.get σ) := by
  cases nneg
  · simp [opt, castPoison]
  · have e : (opt true (.p (.cmp .slt) A (.c fw 0) "nneg" "UB-POISON")).any (Chk.bad σ) =
        evalPred .slt (bv fw (A.get σ)) (bv fw 0) := by
      simp only [opt, ite_true, List.any_cons, List.any_nil, Bool.or_false, bad_cmp]; rw [hA]; rfl
    rw [e, slt_zero_msb]
    simp [castPoison]

end PrismRefine
