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
    {n : Nat} (hn : N.get σ % 2 ^ 64 = n) (hgv : σ g = (n != 0).toNat) :
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
  have hn64 : n < 2 ^ 64 := by rw [← hn]; exact Nat.mod_lt _ (Nat.two_pow_pos 64)
  have hov : ovfTest .uadd 64 (ptrOff q) (N.get σ) = decide (2 ^ 64 ≤ ptrOff q + n) := by
    rw [← ovf_mod64, hn]; exact uadd_ovf64 _ _ (by omega) hn64
  have hadd : (ptrOff q + N.get σ) % 2 ^ 64 = (ptrOff q + n) % 2 ^ 64 := by
    rw [← hn, Nat.add_mod_mod]
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
    boolToNat_mod1, ite_toNat, and1, or1, add64, hadd, hn', hgv, testVal, hov, BitVec.toNat_ofBool,
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

namespace PrismRefine

open PrismSem

/-- The overlap test of `memcpy`: ranges `[fd, fd+n)` and `[fs, fs+n)` in one object. -/
def ovlB (d s n : Nat) : Bool :=
  ptrObj d == ptrObj s && decide (ptrOff d < ptrOff s + n) && decide (ptrOff s < ptrOff d + n)

theorem overlapChk_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j g : Nat) (D S N : Arg)
    (hD : D.below j) (hS : S.below j) (hN : N.below j) (hg : g < j) (hT : TempsOK P j overlapT)
    {n : Nat} (hn : N.get σ % 2 ^ 64 = n) (hgv : σ g = (n != 0).toNat)
    (hd : n ≠ 0 → ptrOff (D.get σ % 2 ^ 64) + n < 2 ^ 64) (hs : n ≠ 0 → ptrOff (S.get σ % 2 ^ 64) + n < 2 ^ 64) :
    ((n != 0 && ovlB (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n) = true →
      xStmts P ω σ t (overlapChk j g D S N) = .fail) ∧
    ((n != 0 && ovlB (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n) = false →
      ∃ σ', xStmts P ω σ t (overlapChk j g D S N) = .ok σ' t ∧ ∀ i, i < j → σ' i = σ i) := by
  have hw : ∀ i (h : i < overlapT.length), P.wd (j + i) = overlapT[i] := hT
  have w0 : P.wd j = 64 := by simpa [overlapT] using hw 0 (by decide)
  have hkk : ∀ a b : Nat, (j + a = j + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (j + a = j) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (j = j + a) = (a = 0) := fun a => by simp [eq_comm]
  have hgk : ∀ a : Nat, (g = j + a) = False := fun a => by simp; omega
  have hgk' : (g = j) = False := by simp; omega
  have hDs := fun τ i v (hi : j ≤ i) => Arg.get_set_ge hD τ (j := i) v hi
  have hSs := fun τ i v (hi : j ≤ i) => Arg.get_set_ge hS τ (j := i) v hi
  have hNs := fun τ i v (hi : j ≤ i) => Arg.get_set_ge hN τ (j := i) v hi
  generalize hdq : D.get σ % 2 ^ 64 = dq at hd
  generalize hsq : S.get σ % 2 ^ 64 = sq at hs
  have hn64 : n < 2 ^ 64 := by rw [← hn]; exact Nat.mod_lt _ (Nat.two_pow_pos 64)
  have hn' : n % 2 ^ 64 = n := Nat.mod_eq_of_lt hn64
  have hadd : ∀ a, (a + N.get σ) % 2 ^ 64 = (a + n) % 2 ^ 64 := fun a => by rw [← hn, Nat.add_mod_mod]
  have hod := ptrOff_lt dq
  have hos := ptrOff_lt sq
  have hdo : ptrObj dq % 2 ^ 64 = ptrObj dq := by rw [← hdq]; exact ptrObj_mod _
  have hso : ptrObj sq % 2 ^ 64 = ptrObj sq := by rw [← hsq]; exact ptrObj_mod _
  simp (config := { decide := true }) only [overlapChk, overlapT, xStmts_assign, xStmts_check,
    w0, hw 1 (by decide), hw 2 (by decide), hw 3 (by decide), hw 4 (by decide), hw 5 (by decide),
    hw 6 (by decide), hw 7 (by decide), hw 8 (by decide), hw 9 (by decide), hw 10 (by decide),
    hw 11 (by decide), List.getElem_cons_succ, List.getElem_cons_zero,
    evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1, hgk, hgk',
    Arg.width_v', Arg.width_c', c64_width, hDs, hSs, hNs, Nat.le_add_right, Nat.le_refl,
    lshr48, mask48, hdq, hsq, ptrOff_mod, icmpVal_eq', pred_eq, pred_ult,
    boolToNat_mod1, ite_toNat, and1, add64, hadd, hn', hgv, Nat.mod_mod, hdo, hso,
    truthN_boolToNat, xStmts_nil, ↓reduceIte, -Nat.reducePow]
  by_cases h0 : n = 0
  · subst h0
    simp only [bne_self_eq_false, Bool.false_and, Bool.false_eq_true, ite_false, false_implies, true_implies,
      true_and]
    refine ⟨_, rfl, fun i hi => ?_⟩
    have hne : ∀ c, i ≠ j + c := fun c => by omega
    simp [set_apply, hne, show i ≠ j by omega]
  · have e1 : (ptrOff dq + n) % 2 ^ 64 = ptrOff dq + n := Nat.mod_eq_of_lt (hd h0)
    have e2 : (ptrOff sq + n) % 2 ^ 64 = ptrOff sq + n := Nat.mod_eq_of_lt (hs h0)
    have e3 : ptrOff dq % 2 ^ 64 = ptrOff dq := Nat.mod_eq_of_lt (by omega)
    have e4 : ptrOff sq % 2 ^ 64 = ptrOff sq := Nat.mod_eq_of_lt (by omega)
    have h0' : (n != 0) = true := by simpa using h0
    simp only [e1, e2, e3, e4, h0', Bool.true_and, ovlB]
    refine ⟨fun hb => ?_, fun hb => ?_⟩
    · simp only [Bool.and_eq_true, beq_iff_eq, decide_eq_true_eq] at hb
      simp [hb]
    · have hb' : (decide (ptrObj dq = ptrObj sq) && (decide (ptrOff dq < ptrOff sq + n) &&
          decide (ptrOff sq < ptrOff dq + n))) = false := by
        rw [← hb]; simp only [Bool.and_assoc]; congr 1
      simp only [hb', Bool.false_eq_true, ite_false]
      refine ⟨_, rfl, fun i hi => ?_⟩
      have hne : ∀ c, i ≠ j + c := fun c => by omega
      simp [set_apply, hne, show i ≠ j by omega]

end PrismRefine

namespace PrismRefine

open PrismSem

theorem nowrap_of_ok {m : Mem} {q n : Nat} {w : Bool} (h : accessBad m q n w 1 = false) :
    ptrOff q + n < 2 ^ 64 := by
  have hs : m.size q < 2 ^ 64 := by simp only [Mem.size]; exact Nat.mod_lt _ (Nat.two_pow_pos 64)
  simp only [accessBad, Bool.or_eq_false_iff, Bool.and_eq_false_iff, bne_iff_ne, ne_eq, beq_iff_eq,
    decide_eq_false_iff_not, Bool.not_eq_false', Bool.not_eq_true] at h
  obtain ⟨⟨⟨⟨⟨h1, h2⟩, h3⟩, h4⟩, _⟩, _⟩ := h
  have hl : m.live q = true := by
    rcases h3 with h3 | h3
    · rcases h2 with h2 | h2
      · exact absurd h2 (by simpa using h1)
      · exact absurd h3 (by simpa using h2)
    · exact h3
  rcases h4 with h4 | h4
  · rw [hl] at h4; cases h4
  · omega

theorem TempsOK.cast {P : PFunc} {a b : Nat} {l : List Nat} (h : TempsOK P a l) (e : a = b) : TempsOK P b l := by
  subst e; exact h

theorem cpyBad_eq (m : Mem) (d s n : Nat) (mv : Bool) :
    cpyBad m d s n mv = ((n != 0 && accessBad m d n true 1) || (n != 0 && accessBad m s n false 1) ||
      (!mv && (n != 0 && ovlB d s n))) := by
  unfold cpyBad ovlB
  cases (n != 0) <;> cases mv <;> simp [Bool.and_assoc]

theorem zext64_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (L : Arg)
    (hL : L.below k) (hw : L.width ≤ 64) (hT : TempsOK P k (zext64 k L).2.1) :
    ∃ σ1, xStmts P ω σ t (zext64 k L).1 = .ok σ1 t ∧ (∀ i, i < k → σ1 i = σ i) ∧
      (zext64 k L).2.2.below (k + (zext64 k L).2.1.length) ∧
      (zext64 k L).2.2.get σ1 % 2 ^ 64 = L.get σ % 2 ^ L.width ∧ (zext64 k L).2.2.width = 64 := by
  by_cases hlt : L.width < 64
  · have e : zext64 k L = ([.assign k (.cast .zext) [L]], [64], .v k 64) := by simp [zext64, hlt]
    rw [e] at hT ⊢
    have w0 : P.wd k = 64 := by simpa using hT 0 (by simp)
    refine ⟨_, rfl, fun i hi => by simp [set_apply, show i ≠ k by omega], by simp [Arg.below], ?_, rfl⟩
    simp only [xStmts, evalOpM, evalOp, castVal, w0, Arg.get_v', set_apply, ite_true, BitVec.toNat_setWidth,
      BitVec.toNat_ofNat, Nat.mod_mod]
    rw [Nat.mod_eq_of_lt (a := L.get σ % 2 ^ L.width)]
    have := Nat.mod_lt (L.get σ) (Nat.two_pow_pos L.width)
    have := Nat.pow_lt_pow_right (a := 2) (by decide) hlt
    omega
  · have e : zext64 k L = ([], [], L) := by simp [zext64, hlt]
    rw [e]
    have h64 : L.width = 64 := by omega
    refine ⟨σ, rfl, fun _ _ => rfl, by simpa using hL, by rw [h64], h64⟩

/-- `MemTr::memcpy_`'s statements after the operands: the widened length,
the guard `n ≠ 0`, the checks of both accesses, the overlap check (not for
`memmove`), the copy.  They fail exactly on `cpyBad`, and otherwise copy. -/
theorem memcpy_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (D S L : Arg)
    (hD : D.below k) (hS : S.below k) (hL : L.below k) (hw : L.width ≤ 64) (mv : Bool)
    (hT : TempsOK P k ((zext64 k L).2.1 ++ [1] ++ accTG true ++ accTG false ++ (if mv then [] else overlapT))) :
    let g := k + (zext64 k L).2.1.length
    let N := (zext64 k L).2.2
    let n := L.get σ % 2 ^ L.width
    let body := (zext64 k L).1 ++ ([.assign g (.cmp .ne) [N, c64 0]] ++ (accChkG (g + 1) g D N true ++
      (accChkG (g + 1 + (accTG true).length) g S N false ++
      ((if mv then [] else overlapChk (g + 1 + (accTG true).length + (accTG false).length) g D S N) ++
      [.memcpy D S N]))))
    (cpyBad t.mem (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n mv = true → xStmts P ω σ t body = .fail) ∧
    (cpyBad t.mem (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n mv = false →
      ∃ σ', xStmts P ω σ t body =
        .ok σ' { t with mem := t.mem.copy (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n } ∧
        ∀ i, i < k → σ' i = σ i) := by
  intro g N n body
  obtain ⟨σ1, e1, ag1, hNb, hNv, hNw⟩ := zext64_run P ω σ t k L hL hw hT.left.left.left.left
  have hNw' : N.width = 64 := hNw
  have hNv' : N.get σ1 % 2 ^ 64 = n := hNv
  have hTg : TempsOK P g [1] := by have := hT.left.left.left.right; simpa using this
  have wg : P.wd g = 1 := by simpa using hTg 0 (by decide)
  have hT1 : TempsOK P (g + 1) (accTG true) :=
    TempsOK.cast hT.left.left.right (by simp [g]; omega)
  have hT2 : TempsOK P (g + 1 + (accTG true).length) (accTG false) :=
    TempsOK.cast hT.left.right (by simp [g]; omega)
  have hT3 : TempsOK P (g + 1 + (accTG true).length + (accTG false).length) (if mv then [] else overlapT) :=
    TempsOK.cast hT.right (by simp [g]; omega)
  have hkg : k ≤ g := Nat.le_add_right _ _
  -- the guard
  let σ2 := σ1.set g ((!decide (N.get σ1 % 2 ^ 64 = 0)).toNat)
  have e2 : xStmts P ω σ1 t [.assign g (.cmp .ne) [N, c64 0]] = .ok σ2 t := by
    simp only [xStmts, evalOpM, evalOp, wg, icmpVal_eq', hNw', pred_ne, c64_get, Nat.zero_mod,
      boolToNat_mod1, xStmts_nil, σ2]
  have ag2 : ∀ i, i < g → σ2 i = σ1 i := fun i hi => by simp [σ2, set_apply, show i ≠ g by omega]
  have hNv2 : N.get σ2 % 2 ^ 64 = n := by rw [Arg.get_agree ag2 hNb, hNv']
  have hg2 : σ2 g = (n != 0).toNat := by
    show (σ1.set g _) g = _
    simp only [set_apply, ite_true, hNv']
    cases h : (n != 0) <;> simp_all
  have hDg : D.get σ2 = D.get σ := by rw [Arg.get_agree ag2 (Arg.below_mono hkg hD), Arg.get_agree ag1 hD]
  have hSg : S.get σ2 = S.get σ := by rw [Arg.get_agree ag2 (Arg.below_mono hkg hS), Arg.get_agree ag1 hS]
  have hb2 : ∀ {A : Arg}, A.below g → A.below (g + 1) := Arg.below_mono (by omega)
  -- the destination
  have c1 := accChkG_run P ω σ2 t (g + 1) g D N (hb2 (Arg.below_mono hkg hD)) (hb2 hNb) (by omega) true hT1
    hNv2 hg2
  rw [hDg] at c1
  simp only [body]
  rw [xStmts_run_append e1, xStmts_append, e2]
  simp only [XPRes.then]
  rw [cpyBad_eq]
  have hlast : ∀ σ5 : Store, (∀ i, i < g + 1 → σ5 i = σ2 i) →
      xStmts P ω σ5 t [.memcpy D S N] =
        .ok σ5 { t with mem := t.mem.copy (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n } := by
    intro σ5 ag
    have h1 : D.get σ5 = D.get σ := by rw [Arg.get_agree ag (hb2 (Arg.below_mono hkg hD)), hDg]
    have h2 : S.get σ5 = S.get σ := by rw [Arg.get_agree ag (hb2 (Arg.below_mono hkg hS)), hSg]
    have h3 : N.get σ5 % 2 ^ 64 = n := by rw [Arg.get_agree ag (hb2 hNb), hNv2]
    simp only [xStmts, h1, h2, h3]
  have agk : ∀ σ5 : Store, (∀ i, i < g + 1 → σ5 i = σ2 i) → ∀ i, i < k → σ5 i = σ i := by
    intro σ5 ag i hi
    rw [ag i (by omega), ag2 i (by omega), ag1 i hi]
  have hA1 : g + 1 + (accTG true).length ≤ g + 1 + (accTG true).length := Nat.le_refl _
  rw [xStmts_append]
  cases hb1 : (n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n true 1)
  · obtain ⟨σ3, e3, ag3⟩ := c1.2 hb1
    rw [e3]; simp only [XPRes.then]; rw [xStmts_append]
    have ag3' : ∀ i, i < g + 1 → σ3 i = σ2 i := fun i hi => ag3 i hi
    have hNv3 : N.get σ3 % 2 ^ 64 = n := by rw [Arg.get_agree ag3' (hb2 hNb), hNv2]
    have hg3 : σ3 g = (n != 0).toNat := by rw [ag3 g (by omega), hg2]
    have hSg3 : S.get σ3 = S.get σ := by rw [Arg.get_agree ag3' (hb2 (Arg.below_mono hkg hS)), hSg]
    have hDg3 : D.get σ3 = D.get σ := by rw [Arg.get_agree ag3' (hb2 (Arg.below_mono hkg hD)), hDg]
    have hb3 : ∀ {A : Arg}, A.below (g + 1) → A.below (g + 1 + (accTG true).length) := Arg.below_mono (by omega)
    have c2 := accChkG_run P ω σ3 t (g + 1 + (accTG true).length) g S N
      (hb3 (hb2 (Arg.below_mono hkg hS))) (hb3 (hb2 hNb)) (by omega) false hT2 hNv3 hg3
    rw [hSg3] at c2
    cases hb2' : (n != 0 && accessBad t.mem (S.get σ % 2 ^ 64) n false 1)
    · obtain ⟨σ4, e4, ag4⟩ := c2.2 hb2'
      rw [e4]; simp only [XPRes.then]; rw [xStmts_append]
      have ag4' : ∀ i, i < g + 1 → σ4 i = σ2 i := fun i hi => by rw [ag4 i (by omega), ag3 i hi]
      cases mv
      · -- memcpy: the overlap check
        have hNv4 : N.get σ4 % 2 ^ 64 = n := by rw [Arg.get_agree ag4' (hb2 hNb), hNv2]
        have hg4 : σ4 g = (n != 0).toNat := by rw [ag4' g (by omega), hg2]
        have hD4 : D.get σ4 = D.get σ := by rw [Arg.get_agree ag4' (hb2 (Arg.below_mono hkg hD)), hDg]
        have hS4 : S.get σ4 = S.get σ := by rw [Arg.get_agree ag4' (hb2 (Arg.below_mono hkg hS)), hSg]
        have hbo : ∀ {A : Arg}, A.below (g + 1) →
            A.below (g + 1 + (accTG true).length + (accTG false).length) := Arg.below_mono (by omega)
        have c3 := overlapChk_run P ω σ4 t (g + 1 + (accTG true).length + (accTG false).length) g D S N
          (hbo (hb2 (Arg.below_mono hkg hD))) (hbo (hb2 (Arg.below_mono hkg hS))) (hbo (hb2 hNb)) (by omega)
          (by simpa using hT3) hNv4 hg4
          (fun h0 => by
            rw [hD4]; apply nowrap_of_ok (w := true)
            have : (n != 0) = true := by simpa using h0
            simpa [this] using hb1)
          (fun h0 => by
            rw [hS4]; apply nowrap_of_ok (w := false)
            have : (n != 0) = true := by simpa using h0
            simpa [this] using hb2')
        rw [hD4, hS4] at c3
        simp only [ite_false, Bool.false_eq_true]
        cases hb4 : (n != 0 && ovlB (D.get σ % 2 ^ 64) (S.get σ % 2 ^ 64) n)
        · obtain ⟨σ5, e5, ag5⟩ := c3.2 hb4
          rw [e5]; simp only [XPRes.then]
          have ag5' : ∀ i, i < g + 1 → σ5 i = σ2 i := fun i hi => by rw [ag5 i (by omega), ag4' i hi]
          exact ⟨fun h => by simp at h, fun _ => ⟨σ5, hlast σ5 ag5', agk σ5 ag5'⟩⟩
        · rw [c3.1 hb4]
          exact ⟨fun _ => rfl, fun h => by simp at h⟩
      · -- memmove
        simp only [ite_true, List.nil_append]
        simp only [xStmts_nil, XPRes.then]
        exact ⟨fun h => by simp at h, fun _ => ⟨σ4, hlast σ4 ag4', agk σ4 ag4'⟩⟩
    · rw [c2.1 hb2']
      exact ⟨fun _ => rfl, fun h => by simp at h⟩
  · rw [c1.1 hb1]
    exact ⟨fun _ => rfl, fun h => by simp at h⟩

end PrismRefine

namespace PrismRefine

open PrismSem

/-- `MemTr::memset_`'s statements after the operands. -/
theorem memset_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (D B L : Arg)
    (hD : D.below k) (hB : B.below k) (hL : L.below k) (hw : L.width ≤ 64)
    (hT : TempsOK P k ((zext64 k L).2.1 ++ [1] ++ accTG true)) :
    let g := k + (zext64 k L).2.1.length
    let N := (zext64 k L).2.2
    let n := L.get σ % 2 ^ L.width
    let body := (zext64 k L).1 ++ ([.assign g (.cmp .ne) [N, c64 0]] ++ (accChkG (g + 1) g D N true ++
      [.memset D B N]))
    ((n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n true 1) = true → xStmts P ω σ t body = .fail) ∧
    ((n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n true 1) = false →
      ∃ σ', xStmts P ω σ t body =
        .ok σ' { t with mem := t.mem.fill (D.get σ % 2 ^ 64) (B.get σ % 256) n } ∧
        ∀ i, i < k → σ' i = σ i) := by
  intro g N n body
  obtain ⟨σ1, e1, ag1, hNb, hNv, hNw⟩ := zext64_run P ω σ t k L hL hw hT.left.left
  have hNw' : N.width = 64 := hNw
  have hNv' : N.get σ1 % 2 ^ 64 = n := hNv
  have hTg : TempsOK P g [1] := by have := hT.left.right; simpa using this
  have wg : P.wd g = 1 := by simpa using hTg 0 (by decide)
  have hT1 : TempsOK P (g + 1) (accTG true) := TempsOK.cast hT.right (by simp [g]; omega)
  have hkg : k ≤ g := Nat.le_add_right _ _
  let σ2 := σ1.set g ((!decide (N.get σ1 % 2 ^ 64 = 0)).toNat)
  have e2 : xStmts P ω σ1 t [.assign g (.cmp .ne) [N, c64 0]] = .ok σ2 t := by
    simp only [xStmts, evalOpM, evalOp, wg, icmpVal_eq', hNw', pred_ne, c64_get, Nat.zero_mod,
      boolToNat_mod1, xStmts_nil, σ2]
  have ag2 : ∀ i, i < g → σ2 i = σ1 i := fun i hi => by simp [σ2, set_apply, show i ≠ g by omega]
  have hNv2 : N.get σ2 % 2 ^ 64 = n := by rw [Arg.get_agree ag2 hNb, hNv']
  have hg2 : σ2 g = (n != 0).toNat := by
    show (σ1.set g _) g = _
    simp only [set_apply, ite_true, hNv']
    cases h : (n != 0) <;> simp_all
  have hDg : D.get σ2 = D.get σ := by rw [Arg.get_agree ag2 (Arg.below_mono hkg hD), Arg.get_agree ag1 hD]
  have hBg : B.get σ2 = B.get σ := by rw [Arg.get_agree ag2 (Arg.below_mono hkg hB), Arg.get_agree ag1 hB]
  have hb2 : ∀ {A : Arg}, A.below g → A.below (g + 1) := Arg.below_mono (by omega)
  have c1 := accChkG_run P ω σ2 t (g + 1) g D N (hb2 (Arg.below_mono hkg hD)) (hb2 hNb) (by omega) true hT1
    hNv2 hg2
  rw [hDg] at c1
  simp only [body]
  rw [xStmts_run_append e1, xStmts_append, e2]
  simp only [XPRes.then]
  rw [xStmts_append]
  cases hb1 : (n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n true 1)
  · obtain ⟨σ3, e3, ag3⟩ := c1.2 hb1
    rw [e3]; simp only [XPRes.then]
    have ag3' : ∀ i, i < g + 1 → σ3 i = σ2 i := fun i hi => ag3 i hi
    have h1 : D.get σ3 = D.get σ := by rw [Arg.get_agree ag3' (hb2 (Arg.below_mono hkg hD)), hDg]
    have h2 : B.get σ3 = B.get σ := by rw [Arg.get_agree ag3' (hb2 (Arg.below_mono hkg hB)), hBg]
    have h3 : N.get σ3 % 2 ^ 64 = n := by rw [Arg.get_agree ag3' (hb2 hNb), hNv2]
    refine ⟨fun h => by simp at h, fun _ => ⟨σ3, by simp only [xStmts, h1, h2, h3], ?_⟩⟩
    intro i hi
    rw [ag3' i (by omega), ag2 i (by omega), ag1 i hi]
  · rw [c1.1 hb1]
    exact ⟨fun _ => rfl, fun h => by simp at h⟩

end PrismRefine
