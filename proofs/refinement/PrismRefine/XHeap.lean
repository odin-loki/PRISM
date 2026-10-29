/-
PRISM refinement, extended fragment — the libc models' heap: the release
checks of `MemTr::dealloc` (`__prism_free`, `__prism_free_check`).

`freeChk_run`: the 25 statements the translator emits for a release of
pointer `p` as an object of kind `kd` fail exactly when `freeBad` holds (`p`
not null and not the start of a live object of that kind: MEM-INVALID-FREE,
MEM-MISMATCHED-FREE, MEM-DOUBLE-FREE); otherwise they write only
temporaries.
-/
import PrismRefine.XMemSim

namespace PrismRefine

open PrismSem

theorem freeChk_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (kd : Nat) (hkd : kd < 256) (hT : TempsOK P k freeT) :
    (freeBad t.mem (A.get σ % 2 ^ 64) kd = true → xStmts P ω σ t (freeChk k A kd) = .fail) ∧
    (freeBad t.mem (A.get σ % 2 ^ 64) kd = false → ∃ σ', xStmts P ω σ t (freeChk k A kd) = .ok σ' t ∧
      ∀ i, i < k → σ' i = σ i) := by
  have w0 : P.wd k = 64 := by simpa [freeT] using wd_of hT 0 (by decide)
  have hkk : ∀ a b : Nat, (k + a = k + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (k + a = k) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (k = k + a) = (a = 0) := fun a => by simp [eq_comm]
  have hAs := fun τ j v (hj : k ≤ j) => Arg.get_set_ge hA τ (j := j) v hj
  generalize hq : A.get σ % 2 ^ 64 = q
  have hobj : ptrObj q % 2 ^ 64 = ptrObj q := by rw [← hq]; exact ptrObj_mod _
  have hkd8 : kd % 2 ^ 8 = kd := Nat.mod_eq_of_lt hkd
  simp (config := { decide := true }) only [freeChk, xStmts_assign, xStmts_check, w0,
    wd_of hT 1 (by decide), wd_of hT 2 (by decide), wd_of hT 3 (by decide), wd_of hT 4 (by decide),
    wd_of hT 5 (by decide), wd_of hT 6 (by decide), wd_of hT 7 (by decide), wd_of hT 8 (by decide),
    wd_of hT 9 (by decide), wd_of hT 10 (by decide), wd_of hT 11 (by decide), wd_of hT 12 (by decide),
    wd_of hT 13 (by decide), wd_of hT 14 (by decide), wd_of hT 15 (by decide), wd_of hT 16 (by decide),
    wd_of hT 17 (by decide), wd_of hT 18 (by decide), wd_of hT 19 (by decide), wd_of hT 20 (by decide),
    wd_of hT 21 (by decide), wd_of hT 22 (by decide), wd_of hT 23 (by decide), wd_of hT 24 (by decide),
    freeT, List.getElem_cons_succ,
    List.getElem_cons_zero, evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1,
    Arg.width_v', Arg.width_c', c64_width, hAs, Nat.le_add_right, Nat.le_refl, ite_true, ite_false,
    lshr48, mask48, hq, ptrObj_mod, ptrOff_mod, kind_mod, icmpVal_eq', pred_eq, pred_ne,
    boolToNat_mod1, ite_toNat, and1, or1, BitVec.toNat_ofBool,
    truthN_boolToNat, Nat.zero_mod, Nat.mod_mod, toNat_eq0, xStmts_nil, hobj, hkd8, Nat.reduceMod]
  constructor
  · intro hb
    unfold freeBad at hb
    by_cases h1 : ptrObj q = 0
    · simp [h1] at hb
    by_cases h2 : t.mem.kind q = 0
    · simp [h1, h2]
    by_cases h3 : ptrOff q = 0
    · by_cases h4 : t.mem.kind q = kd
      · cases h5 : t.mem.live q
        · simp [h1, h2, h3, h4, h5]
        · simp [h1, h2, h3, h4, h5] at hb; exact absurd (h4.trans hb) h2
      · by_cases h6 : (t.mem.kind q = 2 ∨ t.mem.kind q = 5 ∨ t.mem.kind q = 6)
        · rcases h6 with h | h | h
          · have e : ¬ (2 : Nat) = kd := fun e' => h4 (h.trans e')
            simp [h1, h, e]
          · have e : ¬ (5 : Nat) = kd := fun e' => h4 (h.trans e')
            simp [h1, h, e]
          · have e : ¬ (6 : Nat) = kd := fun e' => h4 (h.trans e')
            simp [h1, h, e]
        · simp only [not_or] at h6
          simp [h1, h2, h3, h4, h6]
    · simp [h1, h2, h3]
  · intro hb
    unfold freeBad at hb
    have hne : ∀ i, i < k → ∀ c, i ≠ k + c := fun i hi c => by omega
    by_cases h1 : ptrObj q = 0
    · simp only [h1, decide_true, Bool.not_true, Bool.false_and, Bool.false_eq_true, ite_false]
      refine ⟨_, rfl, fun i hi => ?_⟩
      simp [set_apply, hne i hi, show i ≠ k by omega]
    · have hb' : t.mem.kind q ≠ 0 ∧ ptrOff q = 0 ∧ t.mem.kind q = kd ∧ t.mem.live q = true := by
        simp [h1] at hb; exact ⟨hb.1.1.1, hb.1.1.2, hb.1.2, hb.2⟩
      obtain ⟨h2, h3, h4, h5⟩ := hb'
      have hkd0 : kd ≠ 0 := h4 ▸ h2
      simp [h1, h3, h4, h5, hkd0]
      first
        | (refine ⟨_, rfl, fun i hi => ?_⟩; simp [set_apply, hne i hi, show i ≠ k by omega])
        | (intro i hi; simp [set_apply, hne i hi, show i ≠ k by omega])

theorem freeChkF_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (hT : TempsOK P k freeTF) :
    (freeBad t.mem (A.get σ % 2 ^ 64) 8 = true → xStmts P ω σ t (freeChkF k A) = .fail) ∧
    (freeBad t.mem (A.get σ % 2 ^ 64) 8 = false → ∃ σ', xStmts P ω σ t (freeChkF k A) = .ok σ' t ∧
      ∀ i, i < k → σ' i = σ i) := by
  have w0 : P.wd k = 64 := by simpa [freeTF] using wd_of hT 0 (by decide)
  have hkk : ∀ a b : Nat, (k + a = k + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (k + a = k) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (k = k + a) = (a = 0) := fun a => by simp [eq_comm]
  have hAs := fun τ j v (hj : k ≤ j) => Arg.get_set_ge hA τ (j := j) v hj
  generalize hq : A.get σ % 2 ^ 64 = q
  have hobj : ptrObj q % 2 ^ 64 = ptrObj q := by rw [← hq]; exact ptrObj_mod _
  simp (config := { decide := true }) only [freeChkF, xStmts_assign, xStmts_check, w0,
    wd_of hT 1 (by decide), wd_of hT 2 (by decide), wd_of hT 3 (by decide), wd_of hT 4 (by decide),
    wd_of hT 5 (by decide), wd_of hT 6 (by decide), wd_of hT 7 (by decide), wd_of hT 8 (by decide),
    wd_of hT 9 (by decide), wd_of hT 10 (by decide), wd_of hT 11 (by decide), wd_of hT 12 (by decide),
    wd_of hT 13 (by decide), wd_of hT 14 (by decide), wd_of hT 15 (by decide), wd_of hT 16 (by decide),
    wd_of hT 17 (by decide), wd_of hT 18 (by decide), wd_of hT 19 (by decide), wd_of hT 20 (by decide),
    wd_of hT 21 (by decide), 
    freeTF, List.getElem_cons_succ,
    List.getElem_cons_zero, evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1,
    Arg.width_v', Arg.width_c', c64_width, hAs, Nat.le_add_right, Nat.le_refl, ite_true, ite_false,
    lshr48, mask48, hq, ptrObj_mod, ptrOff_mod, kind_mod, icmpVal_eq', pred_eq, pred_ne,
    boolToNat_mod1, ite_toNat, and1, or1, BitVec.toNat_ofBool,
    truthN_boolToNat, Nat.zero_mod, Nat.mod_mod, toNat_eq0, xStmts_nil, hobj, Nat.reduceMod]
  constructor
  · intro hb
    unfold freeBad at hb
    by_cases h1 : ptrObj q = 0
    · simp [h1] at hb
    by_cases h2 : t.mem.kind q = 0
    · simp [h1, h2]
    by_cases h3 : ptrOff q = 0
    · by_cases h4 : t.mem.kind q = 8
      · cases h5 : t.mem.live q
        · simp [h1, h2, h3, h4, h5]
        · simp [h1, h2, h3, h4, h5] at hb
      · simp [h1, h2, h3, h4]
    · simp [h1, h2, h3]
  · intro hb
    unfold freeBad at hb
    have hne : ∀ i, i < k → ∀ c, i ≠ k + c := fun i hi c => by omega
    by_cases h1 : ptrObj q = 0
    · simp only [h1, decide_true, Bool.not_true, Bool.false_and, Bool.false_eq_true, ite_false]
      refine ⟨_, rfl, fun i hi => ?_⟩
      simp [set_apply, hne i hi, show i ≠ k by omega]
    · have hb' : t.mem.kind q ≠ 0 ∧ ptrOff q = 0 ∧ t.mem.kind q = 8 ∧ t.mem.live q = true := by
        simp [h1] at hb; exact ⟨hb.1.1.1, hb.1.1.2, hb.1.2, hb.2⟩
      obtain ⟨h2, h3, h4, h5⟩ := hb'
      simp [h1, h3, h4, h5]
      first
        | (refine ⟨_, rfl, fun i hi => ?_⟩; simp [set_apply, hne i hi, show i ≠ k by omega])
        | (intro i hi; simp [set_apply, hne i hi, show i ≠ k by omega])

theorem freeChkK_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (kd : Nat) (hkd : kd < 256) (hT : TempsOK P k (freeTK kd)) :
    (freeBad t.mem (A.get σ % 2 ^ 64) kd = true → xStmts P ω σ t (freeChkK k A kd) = .fail) ∧
    (freeBad t.mem (A.get σ % 2 ^ 64) kd = false → ∃ σ', xStmts P ω σ t (freeChkK k A kd) = .ok σ' t ∧
      ∀ i, i < k → σ' i = σ i) := by
  unfold freeChkK
  unfold freeTK at hT
  by_cases h8 : kd = 8
  · subst h8; simp only [ite_true] at hT ⊢; exact freeChkF_run P ω σ t k A hA hT
  · simp only [h8, ite_false] at hT ⊢; exact freeChk_run P ω σ t k A hA kd hkd hT

/-- The select stored by `hobjszCore`, after the destination-width modulus, is
`hobjszVal` (`0` when the offset is past the object, otherwise `size - offset`). -/
theorem hobjszMachine (sz off : Nat) (hsz : sz < 2 ^ 64) (hoff : off < 2 ^ 64) :
    (selVal 64 ((decide (sz < off)).toNat) 0 (binVal .sub 64 sz off % 2 ^ 64) % 2 ^ 64) % 2 ^ 64 =
      if off > sz then 0 else (sz - off) % 2 ^ 64 := by
  unfold selVal
  by_cases hlt : sz < off
  · rw [decide_eq_true hlt, truthN_boolToNat]
    split
    · have hgt : off > sz := hlt
      simp only [hgt, ite_true, Nat.zero_mod, Nat.mod_mod]
    · rename_i hf
      first | nomatch hf | exact absurd rfl hf
  · rw [decide_eq_false hlt, truthN_boolToNat]
    have hle : off ≤ sz := Nat.le_of_not_gt hlt
    have hsub : sz - off < 2 ^ 64 := by omega
    split
    · rename_i ht
      first | nomatch ht | exact absurd rfl ht
    · have hgt : ¬ off > sz := hlt
      simp only [hgt, ite_false, sub64_ge sz off hsz hoff hle, Nat.mod_eq_of_lt hsub, Nat.mod_mod]

/-- `MemTr::obj_size_remaining` (`__prism_obj_size`): temporaries `k..k+4` hold the
bytes from `p` to the end of its object; no memory is changed. -/
theorem hobjszCore_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (hT : TempsOK P k hobjszT) :
    ∃ σ', xStmts P ω σ t (hobjszCore k A) = .ok σ' t ∧
      σ' (k + 4) % 2 ^ 64 = hobjszVal t.mem (A.get σ) ∧
      ∀ i, i < k → σ' i = σ i := by
  generalize hq : A.get σ % 2 ^ 64 = q
  have hoff : ptrOff q % 2 ^ 64 = ptrOff q := ptrOff_mod q
  have hsz : t.mem.size q % 2 ^ 64 = t.mem.size q := size_mod t.mem q
  have hsz64 : t.mem.size q < 2 ^ 64 := by
    have hlt := Nat.mod_lt (t.mem.size q) (Nat.two_pow_pos 64)
    rwa [hsz] at hlt
  have hoff64 : ptrOff q < 2 ^ 64 := Nat.lt_trans (ptrOff_lt q) (by decide : (2 : Nat) ^ 48 < 2 ^ 64)
  have w0 : P.wd k = 64 := by simpa [hobjszT] using wd_of hT 0 (by decide)
  have w1 : P.wd (k + 1) = 64 := by simpa [hobjszT] using wd_of hT 1 (by decide)
  have w2 : P.wd (k + 2) = 64 := by simpa [hobjszT] using wd_of hT 2 (by decide)
  have w3 : P.wd (k + 3) = 1 := by simpa [hobjszT] using wd_of hT 3 (by decide)
  have w4 : P.wd (k + 4) = 64 := by simpa [hobjszT] using wd_of hT 4 (by decide)
  unfold hobjszCore
  rw [xStmts_assign]
  have step1 :
      evalOpM t.mem σ .objSize (P.wd k) [A] % 2 ^ P.wd k = t.mem.size q := by
    simp only [evalOpM, w0, hq, hsz]
  rw [step1]
  generalize hσ1 : σ.set k (t.mem.size q) = σ1
  have hA1 : A.get σ1 = A.get σ := hσ1 ▸ Arg.get_set_ge hA σ (j := k) (t.mem.size q) (Nat.le_refl k)
  rw [xStmts_assign]
  have step2 :
      evalOpM t.mem σ1 (.bin .and) (P.wd (k + 1)) [A, c64 (2 ^ 48 - 1)] % 2 ^ P.wd (k + 1) =
        ptrOff q := by
    simp only [evalOpM, evalOp, w1, hA1, c64_get, hq, mask48, hoff]
  rw [step2]
  generalize hσ2 : σ1.set (k + 1) (ptrOff q) = σ2
  have hk2 : σ2 k = t.mem.size q := by
    simp only [← hσ2, Store.set_other _ _ (show k ≠ k + 1 by omega), ← hσ1, Store.set_same]
  have h12 : σ2 (k + 1) = ptrOff q := by
    simp only [← hσ2, Store.set_same]
  rw [xStmts_assign]
  have step3 :
      evalOpM t.mem σ2 (.bin .sub) (P.wd (k + 2)) [.v k 64, .v (k + 1) 64] % 2 ^ P.wd (k + 2) =
        binVal .sub 64 (t.mem.size q) (ptrOff q) % 2 ^ 64 := by
    simp only [evalOpM, evalOp, w2, Arg.get_v', hk2, h12]
  rw [step3]
  generalize hσ3 : σ2.set (k + 2) (binVal .sub 64 (t.mem.size q) (ptrOff q) % 2 ^ 64) = σ3
  have hk3 : σ3 k = t.mem.size q := by
    simp only [← hσ3, Store.set_other _ _ (show k ≠ k + 2 by omega), hk2]
  have h13 : σ3 (k + 1) = ptrOff q := by
    simp only [← hσ3, Store.set_other _ _ (show k + 1 ≠ k + 2 by omega), h12]
  rw [xStmts_assign]
  have step4 :
      evalOpM t.mem σ3 (.cmp .ugt) (P.wd (k + 3)) [.v (k + 1) 64, .v k 64] % 2 ^ P.wd (k + 3) =
        (decide (t.mem.size q < ptrOff q)).toNat := by
    simp only [evalOpM, evalOp, w3, Arg.get_v', Arg.width_v', hk3, h13, icmpVal_eq', pred_ugt, hsz,
      hoff, boolToNat_mod1]
  rw [step4]
  generalize hσ4 : σ3.set (k + 3) ((decide (t.mem.size q < ptrOff q)).toNat) = σ4
  have h43 : σ4 (k + 3) = (decide (t.mem.size q < ptrOff q)).toNat := by
    simp only [← hσ4, Store.set_same]
  have h42 : σ4 (k + 2) = binVal .sub 64 (t.mem.size q) (ptrOff q) % 2 ^ 64 := by
    simp only [← hσ4, Store.set_other _ _ (show k + 2 ≠ k + 3 by omega), ← hσ3, Store.set_same]
  rw [xStmts_assign, xStmts_nil]
  have step5 :
      evalOpM t.mem σ4 .select (P.wd (k + 4)) [.v (k + 3) 1, .c 64 0, .v (k + 2) 64] %
          2 ^ P.wd (k + 4) =
        selVal 64 ((decide (t.mem.size q < ptrOff q)).toNat) 0
          (binVal .sub 64 (t.mem.size q) (ptrOff q) % 2 ^ 64) % 2 ^ 64 := by
    simp only [evalOpM, evalOp, w4, Arg.get_v', Arg.get_c', h43, h42]
  rw [step5]
  generalize hσ5 : σ4.set (k + 4)
    (selVal 64 ((decide (t.mem.size q < ptrOff q)).toNat) 0
        (binVal .sub 64 (t.mem.size q) (ptrOff q) % 2 ^ 64) % 2 ^ 64) = σ5
  refine ⟨σ5, rfl, ?_, ?_⟩
  · simp only [← hσ5, Store.set_same]
    simpa [hobjszVal, hq] using hobjszMachine (t.mem.size q) (ptrOff q) hsz64 hoff64
  · intro i hi
    have hne4 : i ≠ k + 4 := by omega
    have hne3 : i ≠ k + 3 := by omega
    have hne2 : i ≠ k + 2 := by omega
    have hne1 : i ≠ k + 1 := by omega
    have hne0 : i ≠ k := by omega
    simp only [← hσ5, ← hσ4, ← hσ3, ← hσ2, ← hσ1, set_apply, ite_false hne4, ite_false hne3, ite_false hne2,
      ite_false hne1, ite_false hne0]
    simp [hne4, hne3, hne2, hne1, hne0]

end PrismRefine
