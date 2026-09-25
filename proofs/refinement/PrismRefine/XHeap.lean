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

end PrismRefine
