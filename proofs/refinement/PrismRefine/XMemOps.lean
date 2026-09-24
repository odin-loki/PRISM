/-
PRISM refinement, extended fragment — `llvm.memcpy` / `memmove` / `memset`:
the statements of `MemTr::memcpy_` / `memset_` (`XTranslate.lean`:
`accChkG`, `overlapChk`) fail exactly when the copy or fill is undefined
(`cpyBad`, `XLlvm.lean`), for every length, constant or not.
-/
import PrismRefine.XMemSim

namespace PrismRefine

open PrismSem

theorem uadd_ovf64 (f n : Nat) (hf : f < 2 ^ 64) (hn : n < 2 ^ 64) :
    ovfTest .uadd 64 f n = decide (2 ^ 64 ≤ f + n) := by
  simp only [ovfTest, BitVec.toNat_add, BitVec.toNat_ofNat]
  rw [Nat.mod_eq_of_lt hf, Nat.mod_eq_of_lt hn]
  apply Bool.eq_iff_iff.mpr
  simp only [decide_eq_true_eq]
  constructor
  · intro h
    by_cases hc : 2 ^ 64 ≤ f + n
    · exact hc
    · rw [Nat.mod_eq_of_lt (by omega)] at h; omega
  · intro h
    have : (f + n) % 2 ^ 64 = f + n - 2 ^ 64 := by
      rw [Nat.mod_eq_sub_mod h, Nat.mod_eq_of_lt (by omega)]
    omega

/-- The bounds test of a guarded access: `f + n` (64-bit) above the size, or
the addition overflows, is `size < f + n`. -/
theorem oob_iff (f n sz : Nat) (hs : sz < 2 ^ 64) :
    (decide (sz < (f + n) % 2 ^ 64) || decide (2 ^ 64 ≤ f + n)) = decide (sz < f + n) := by
  apply Bool.eq_iff_iff.mpr
  simp only [Bool.or_eq_true, decide_eq_true_eq]
  by_cases h : f + n < 2 ^ 64
  · rw [Nat.mod_eq_of_lt h]; omega
  · omega

theorem accessBad_one (m : Mem) (q n : Nat) (write : Bool) :
    accessBad m q n write 1 = (bad0 m q n || (write && (m.live q && m.kind q == 4))) := by
  simp only [accessBad_eq, show ¬ (1 < 1) from by decide, decide_false, Bool.false_and, Bool.or_false]

/-- `MemTr::access_checks` under the guard `g` (`n ≠ 0`), byte alignment:
the statements fail exactly when `n ≠ 0` and the access is bad. -/
theorem accChkG_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k g : Nat) (A N : Arg)
    (hA : A.below k) (hN : N.below k) (hg : g < k) (write : Bool) (hT : TempsOK P k (accTG write))
    {n : Nat} (hn : N.get σ = n) (hn64 : n < 2 ^ 64) (hgv : σ g = (n != 0).toNat) :
    ((n != 0 && accessBad t.mem (A.get σ % 2 ^ 64) n write 1) = true →
      xStmts P ω σ t (accChkG k g A N write) = .fail) ∧
    ((n != 0 && accessBad t.mem (A.get σ % 2 ^ 64) n write 1) = false →
      ∃ σ', xStmts P ω σ t (accChkG k g A N write) = .ok σ' t ∧ ∀ i, i < k → σ' i = σ i) := by
  have hw : ∀ j (h : j < (accTG write).length), P.wd (k + j) = (accTG write)[j] := hT
  have w0 : P.wd k = 64 := by
    have := hw 0 (by cases write <;> decide); cases write <;> simpa [accTG] using this
  have hkk : ∀ a b : Nat, (k + a = k + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (k + a = k) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (k = k + a) = (a = 0) := fun a => by simp [eq_comm]
  have hgk : ∀ a : Nat, (g = k + a) = False := fun a => by simp; omega
  have hgk' : (g = k) = False := by simp; omega
  have hAs := fun τ j v (hj : k ≤ j) => Arg.get_set_ge hA τ (j := j) v hj
  have hNs := fun τ j v (hj : k ≤ j) => Arg.get_set_ge hN τ (j := j) v hj
  generalize hq : A.get σ % 2 ^ 64 = q
  have hoff := ptrOff_lt q
  have hobj : ptrObj q % 2 ^ 64 = ptrObj q := by rw [← hq]; exact ptrObj_mod _
  have hov : ovfTest .uadd 64 (ptrOff q) n = decide (2 ^ 64 ≤ ptrOff q + n) :=
    uadd_ovf64 _ _ (by omega) hn64
  have hsz := Nat.mod_lt ((t.mem.obj? q).map (·.size) |>.getD 0) (Nat.two_pow_pos 64)
  have hoob := oob_iff (ptrOff q) n (t.mem.size q) (by simpa [Mem.size] using hsz)
  have hn' : n % 2 ^ 64 = n := Nat.mod_eq_of_lt hn64
  have w21 : write = true → P.wd (k + 21) = 1 := fun h => by subst h; exact hw 21 (by decide)
  have w22 : write = true → P.wd (k + 22) = 1 := fun h => by subst h; exact hw 22 (by decide)
  have w23 : write = true → P.wd (k + 23) = 1 := fun h => by subst h; exact hw 23 (by decide)
  cases write <;>
  simp (config := { decide := true }) only [accChkG, accTG, List.cons_append, List.nil_append,
    List.append_nil, ite_true, ite_false, Bool.false_eq_true, xStmts_assign, xStmts_check,
    w0, hw 1 (by decide), hw 2 (by decide), hw 3 (by decide), hw 4 (by decide),
    hw 5 (by decide), hw 6 (by decide), hw 7 (by decide), hw 8 (by decide), hw 9 (by decide),
    hw 10 (by decide), hw 11 (by decide), hw 12 (by decide), hw 13 (by decide), hw 14 (by decide),
    hw 15 (by decide), hw 16 (by decide), hw 17 (by decide), hw 18 (by decide), hw 19 (by decide),
    hw 20 (by decide), List.getElem_cons_succ, List.getElem_cons_zero,
    evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1, hgk, hgk',
    Arg.width_v', Arg.width_c', c64_width, hAs, hNs, Nat.le_add_right, Nat.le_refl,
    lshr48, mask48, hq, ptrObj_mod, ptrOff_mod, kind_mod, size_mod, icmpVal_eq', pred_eq, pred_ne, pred_ugt,
    boolToNat_mod1, ite_toNat, and1, or1, add64, hn, hn', hgv, testVal, hov, BitVec.toNat_ofBool,
    truthN_boolToNat, Nat.zero_mod, Nat.mod_mod, toNat_eq0, Bool.or_false, xStmts_nil, hobj, hoob, w21, w22, w23]
  all_goals
    rw [accessBad_one]; simp only [bad0]
    refine ⟨fun hb => ?_, fun hb => ?_⟩
  all_goals
    by_cases h0 : n = 0 <;> by_cases h1 : ptrObj q = 0 <;> by_cases h2 : t.mem.kind q = 0 <;>
      cases h3 : t.mem.live q <;> by_cases h4 : t.mem.size q < ptrOff q + n <;>
      simp_all [-Nat.not_lt]
  all_goals
    intro i hi
    have hne : ∀ c, i ≠ k + c := fun c => by omega
    simp [set_apply, hne, show i ≠ k by omega]

end PrismRefine
