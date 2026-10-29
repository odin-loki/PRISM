/-
PRISM refinement, extended fragment — `__prism_havoc_bytes` (D136, the stdio
models' input; `MemTr::write_havoc`).

`havoc_run`: the statements `havocCore` emits for `n` bytes at `D` fail
exactly when the guarded write checks fail (`n ≠ 0` and the write is bad);
otherwise, for a size below `2^47`, they leave memory as `havocW` (a fresh
`extern` object of arbitrary bytes copied to `D`, then released) and write
only temporaries.
-/
import PrismRefine.XMemOps

namespace PrismRefine

open PrismSem

/-- `MemTr::read_range` (`__prism_read_range`): the guard `N ≠ 0` and the read
checks under it fail exactly when the read is bad; otherwise only temporaries
are written. -/
theorem hread_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (g : Nat) (D N : Arg)
    (hD : D.below g) (hN : N.below g) (hNw : N.width = 64)
    (wr : Bool) (hT : TempsOK P g ([1] ++ accTG wr)) :
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) wr 1) = true →
      xStmts P ω σ t ([.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N wr) = .fail) ∧
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) wr 1) = false →
      ∃ σ', xStmts P ω σ t ([.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N wr) = .ok σ' t ∧
        ∀ i, i < g → σ' i = σ i) := by
  generalize hn : N.get σ % 2 ^ 64 = n
  have wg : P.wd g = 1 := by simpa using wd_of hT 0 (by simp)
  have hT1 : TempsOK P (g + 1) (accTG wr) := TempsOK.cast hT.right (by simp)
  simp only [List.singleton_append]
  rw [xStmts_assign]
  have e2 : evalOpM t.mem σ (.cmp .ne) (P.wd g) [N, c64 0] % 2 ^ P.wd g = (n != 0).toNat := by
    simp only [evalOpM, evalOp, wg, icmpVal_eq', hNw, pred_ne, c64_get, Nat.zero_mod, boolToNat_mod1, hn]
    cases h : (n != 0) <;> simp_all
  rw [e2]
  generalize hσ2 : σ.set g (n != 0).toNat = σ2
  have ag2 : ∀ i, i < g → σ2 i = σ i := fun i hi => by rw [← hσ2]; exact Store.set_other _ _ (by omega)
  have hNv2 : N.get σ2 % 2 ^ 64 = n := by rw [Arg.get_agree ag2 hN, hn]
  have hDv2 : D.get σ2 = D.get σ := Arg.get_agree ag2 hD
  have hg2 : σ2 g = (n != 0).toNat := by rw [← hσ2]; simp [set_apply]
  have hb2 : ∀ {A : Arg}, A.below g → A.below (g + 1) := fun h => Arg.below_mono (by omega) h
  have c1 := accChkG_run P ω σ2 t (g + 1) g D N (hb2 hD) (hb2 hN) (by omega) wr hT1 hNv2 hg2
  rw [hDv2] at c1
  cases hb1 : (n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n wr 1)
  · obtain ⟨σ3, e3, ag3⟩ := c1.2 hb1
    refine ⟨fun h => by simp at h, fun _ => ⟨σ3, e3, fun i hi => ?_⟩⟩
    rw [ag3 i (by omega), ag2 i hi]
  · exact ⟨fun _ => c1.1 hb1, fun h => by simp at h⟩


/-- `__prism_memset`'s tail: truncate the `i32` byte (after the checks, as
`MemTr::memset_` does), then fill. -/
theorem hmemTail_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (o : Nat) (D B N : Arg)
    (hD : D.below o) (_hB : B.below o) (hN : N.below o) (hBw : B.width = 32) (hwo : P.wd o = 8) :
    xStmts P ω σ t [.assign o (.cast .trunc) [B], .memset D (.v o 8) N] =
      .ok (σ.set o (B.get σ % 256)) { t with mem := t.mem.fill (D.get σ % 2 ^ 64) (B.get σ % 256) (N.get σ % 2 ^ 64) } := by
  have gD : ∀ v, D.get (σ.set o v) = D.get σ := fun v => Arg.get_set_ge hD σ v (Nat.le_refl o)
  have gN : ∀ v, N.get (σ.set o v) = N.get σ := fun v => Arg.get_set_ge hN σ v (Nat.le_refl o)
  have e : (B.get σ % 2 ^ 32) % 2 ^ 8 = B.get σ % 256 := by
    rw [Nat.mod_mod_of_dvd _ (by decide)]
  simp only [xStmts, evalOpM, evalOp, castVal, hwo, hBw, BitVec.toNat_setWidth, BitVec.toNat_ofNat, e,
    Nat.mod_mod, Arg.get_v', Store.set_same, gD, gN]

theorem hmemset_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (g : Nat) (D B N : Arg)
    (hD : D.below g) (hB : B.below g) (hN : N.below g) (hNw : N.width = 64) (hBw : B.width = 32)
    (hT : TempsOK P g (([1] ++ accTG true) ++ [8])) :
    let o := g + 1 + (accTG true).length
    let body := ([.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N true) ++
      [.assign o (.cast .trunc) [B], .memset D (.v o 8) N]
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) true 1) = true →
      xStmts P ω σ t body = .fail) ∧
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) true 1) = false →
      ∃ σ', xStmts P ω σ t body =
        .ok σ' { t with mem := t.mem.fill (D.get σ % 2 ^ 64) (B.get σ % 256) (N.get σ % 2 ^ 64) } ∧
        ∀ i, i < g → σ' i = σ i) := by
  intro o body
  have hr := hread_run P ω σ t g D N hD hN hNw true hT.left
  have wo : P.wd o = 8 := by simpa using wd_of (TempsOK.cast hT.right (show g + ([1] ++ accTG true).length = o by simp [o]; omega)) 0 (by simp)
  simp only [body]
  rw [xStmts_append]
  refine ⟨fun hb => by rw [hr.1 hb]; rfl, fun hb => ?_⟩
  obtain ⟨σ1, e1, ag1⟩ := hr.2 hb
  rw [e1]
  simp only [XPRes.then]
  have hlo : g ≤ o := by simp [o]; omega
  have hD1 : D.get σ1 = D.get σ := Arg.get_agree ag1 hD
  have hB1 : B.get σ1 = B.get σ := Arg.get_agree ag1 hB
  have hN1 : N.get σ1 = N.get σ := Arg.get_agree ag1 hN
  rw [hmemTail_run P ω σ1 t o D B N (Arg.below_mono hlo hD) (Arg.below_mono hlo hB) (Arg.below_mono hlo hN) hBw wo,
    hD1, hB1, hN1]
  exact ⟨_, rfl, fun i hi => by rw [Store.set_other _ _ (by omega), ag1 i hi]⟩

/-- The last three statements: allocate, copy, release. -/
theorem havocTail_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (o : Nat) (D N : Arg)
    (hD : D.below o) (hN : N.below o) (hwo : P.wd o = 64) :
    xStmts P ω σ t [.alloc o N 7 2 16, .memcpy D (.v o 64) N, .free (.v o 64)] =
      .ok (σ.set o ((t.allocW ω (N.get σ % 2 ^ 64) 7 16 2).2 % 2 ^ 64))
        (havocW ω t (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64)) := by
  have gD : ∀ v, D.get (σ.set o v) = D.get σ := fun v => Arg.get_set_ge hD σ v (Nat.le_refl o)
  have gN : ∀ v, N.get (σ.set o v) = N.get σ := fun v => Arg.get_set_ge hN σ v (Nat.le_refl o)
  simp only [xStmts, hwo, Arg.get_v', Store.set_same, gD, gN, Nat.mod_mod, havocW]

theorem havoc_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (g : Nat) (D N : Arg)
    (hNc : (match N with | .c _ b => decide (b < 2 ^ 47) | .v _ _ => true) = true)
    (hD : D.below g) (hN : N.below g) (hNw : N.width = 64)
    (hT : TempsOK P g (havocT g N)) :
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) true 1) = true →
      xStmts P ω σ t (havocCore g D N) = .fail) ∧
    ((N.get σ % 2 ^ 64 != 0 && accessBad t.mem (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64) true 1) = false →
      N.get σ % 2 ^ 64 < 2 ^ 47 →
      ∃ σ', xStmts P ω σ t (havocCore g D N) =
        .ok σ' (havocW ω t (D.get σ % 2 ^ 64) (N.get σ % 2 ^ 64)) ∧ ∀ i, i < g → σ' i = σ i) := by
  generalize hn : N.get σ % 2 ^ 64 = n
  have wg : P.wd g = 1 := by simpa [havocT] using wd_of hT 0 (by simp [havocT])
  have hT1 : TempsOK P (g + 1) (accTG true) :=
    TempsOK.cast hT.left.left.right (by simp)
  have hTa : TempsOK P (g + 1 + (accTG true).length) (hallocSz (g + 1 + (accTG true).length) N).2 :=
    TempsOK.cast hT.left.right (by simp; omega)
  have hTo : TempsOK P (g + 1 + (accTG true).length + (hallocSz (g + 1 + (accTG true).length) N).2.length) [64] :=
    TempsOK.cast hT.right (by simp; omega)
  have wo : P.wd (g + 1 + (accTG true).length + (hallocSz (g + 1 + (accTG true).length) N).2.length) = 64 := by
    simpa using hTo 0 (by simp)
  unfold havocCore
  simp only [List.singleton_append]
  rw [xStmts_assign]
  have e2 : evalOpM t.mem σ (.cmp .ne) (P.wd g) [N, c64 0] % 2 ^ P.wd g = (n != 0).toNat := by
    simp only [evalOpM, evalOp, wg, icmpVal_eq', hNw, pred_ne, c64_get, Nat.zero_mod, boolToNat_mod1, hn]
    cases h : (n != 0) <;> simp_all
  rw [e2]
  generalize hσ2 : σ.set g (n != 0).toNat = σ2
  have ag2 : ∀ i, i < g → σ2 i = σ i := fun i hi => by rw [← hσ2]; exact Store.set_other _ _ (by omega)
  have hNv2 : N.get σ2 % 2 ^ 64 = n := by rw [Arg.get_agree ag2 hN, hn]
  have hDv2 : D.get σ2 = D.get σ := Arg.get_agree ag2 hD
  have hg2 : σ2 g = (n != 0).toNat := by rw [← hσ2]; simp [set_apply]
  have hb2 : ∀ {A : Arg}, A.below g → A.below (g + 1) := fun h => Arg.below_mono (by omega) h
  have c1 := accChkG_run P ω σ2 t (g + 1) g D N (hb2 hD) (hb2 hN) (by omega) true hT1 hNv2 hg2
  rw [hDv2] at c1
  rw [xStmts_append]
  cases hb1 : (n != 0 && accessBad t.mem (D.get σ % 2 ^ 64) n true 1)
  · obtain ⟨σ3, e3, ag3⟩ := c1.2 hb1
    rw [e3]; simp only [XPRes.then]
    refine ⟨fun h => by simp at h, fun _ hlt => ?_⟩
    have ag3' : ∀ i, i < g + 1 → σ3 i = σ2 i := fun i hi => ag3 i hi
    have hD3 : D.get σ3 = D.get σ := by rw [Arg.get_agree ag3' (hb2 hD), hDv2]
    have hN3 : N.get σ3 % 2 ^ 64 = n := by rw [Arg.get_agree ag3' (hb2 hN), hNv2]
    have ha : g ≤ g + 1 + (accTG true).length := by omega
    cases N with
    | c w b =>
      simp only [hallocSz, List.nil_append, List.length_nil, Nat.add_zero] at wo ⊢
      rw [havocTail_run P ω σ3 t _ D (.c w b) (Arg.below_mono (by omega) hD) (by simp [Arg.below]) wo, hD3, hN3]
      refine ⟨_, rfl, fun i hi => ?_⟩
      rw [Store.set_other _ _ (by omega), ag3' i (by omega), ag2 i hi]
    | v j w' =>
      simp only [hallocSz, List.length_cons, List.length_nil, Nat.zero_add] at wo hTa ⊢
      have w0 : P.wd (g + 1 + (accTG true).length) = 1 := by simpa using hTa 0 (by simp)
      have hjg : j < g := by simpa [Arg.below] using hN
      simp only [Arg.width_v'] at hNw
      subst hNw
      rw [xStmts_append]
      have hN3' : σ3 j % 2 ^ 64 = n := by simpa using hN3
      have e1 : xStmts P ω σ3 t [PStmt.assign (g + 1 + (accTG true).length) (POp.cmp Pred.ult)
            [Arg.v j 64, c64 (2 ^ 47)], PStmt.assume (Arg.v (g + 1 + (accTG true).length) 1)] =
          .ok (σ3.set (g + 1 + (accTG true).length) (true).toNat) t := by
        rw [xStmts_assign]
        simp only [xStmts, evalOpM, evalOp, w0, Arg.width_v', Arg.get_v', c64_get, icmpVal_eq', pred_ult,
          boolToNat_mod1, Store.set_same, truthN_boolToNat, hN3']
        have h47 : (2 ^ 47 : Nat) % 2 ^ 64 = 2 ^ 47 := by decide
        simp [h47, hlt]
      rw [e1]
      simp only [XPRes.then]
      generalize hσ4 : σ3.set (g + 1 + (accTG true).length) (true).toNat = σ4
      have ag4 : ∀ i, i < g + 1 + (accTG true).length → σ4 i = σ3 i :=
        fun i hi => by rw [← hσ4]; exact Store.set_other _ _ (by omega)
      have hD4 : D.get σ4 = D.get σ := by rw [Arg.get_agree ag4 (Arg.below_mono (by omega) hD), hD3]
      have hN4 : (Arg.v j 64).get σ4 % 2 ^ 64 = n := by simp only [Arg.get_v']; rw [ag4 j (by omega), hN3']
      rw [havocTail_run P ω σ4 t _ D (.v j 64) (Arg.below_mono (by omega) hD)
        (by simp [Arg.below]; omega) wo, hD4, hN4]
      refine ⟨_, rfl, fun i hi => ?_⟩
      rw [Store.set_other _ _ (by omega), ag4 i (by omega), ag3' i (by omega), ag2 i hi]
  · rw [c1.1 hb1]
    exact ⟨fun _ => rfl, fun h => by simp at h⟩

theorem truthN1 : truthN 1 = true := rfl

theorem binVal_add64 (x y : Nat) : binVal .add 64 x y = (x % 2 ^ 64 + y % 2 ^ 64) % 2 ^ 64 := by
  simp [binVal, evalBin, bv, BitVec.toNat_add]

/-- `MemTr::fresh_cstr`'s statements against `sFresh`. -/
theorem freshCore_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k kd : Nat)
    (hT : TempsOK P k freshT) :
    (∀ p W', sFresh ω t kd = .ok (p, W') → ∃ σ', xStmts P ω σ t (freshCore k kd) = .ok σ' W' ∧
      σ' (k + 4) = p ∧ ∀ i, i < k → σ' i = σ i) ∧
    (sFresh ω t kd = .stuck → xStmts P ω σ t (freshCore k kd) = .blocked) := by
  have w0 : P.wd k = 64 := by simpa [freshT] using hT 0 (by simp [freshT])
  have w1 : P.wd (k + 1) = 1 := by simpa [freshT] using hT 1 (by simp [freshT])
  have w2 : P.wd (k + 2) = 64 := by simpa [freshT] using hT 2 (by simp [freshT])
  have w3 : P.wd (k + 3) = 1 := by simpa [freshT] using hT 3 (by simp [freshT])
  have w4 : P.wd (k + 4) = 64 := by simpa [freshT] using hT 4 (by simp [freshT])
  have w5 : P.wd (k + 5) = 64 := by simpa [freshT] using hT 5 (by simp [freshT])
  generalize hn : ω t.t % 2 ^ 64 = n
  by_cases hlt : n < 4096
  · have hs : sFresh ω t kd = .ok (((t.adv 1).allocW ω (n + 1) kd 16 2).2 % 2 ^ 64,
        ((t.adv 1).allocW ω (n + 1) kd 16 2).1.store
          ((((t.adv 1).allocW ω (n + 1) kd 16 2).2 % 2 ^ 64 + n) % 2 ^ 64) 0 8 true) := by
      simp [sFresh, hn, hlt]
    refine ⟨fun p W' h => ?_, fun h => (by rw [hs] at h; cases h)⟩
    rw [hs] at h
    simp only [Res.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hn1 : (n + 1) % 2 ^ 64 = n + 1 := Nat.mod_eq_of_lt (by omega)
    have hnn : n % 2 ^ 64 = n := Nat.mod_eq_of_lt (by omega)
    have hp := World.allocW_lt (t.adv 1) ω (n + 1) kd 16 2
    have h47 : n + 1 < 2 ^ 47 := by omega
    simp [freshCore, xStmts, evalOpM, evalOp, w0, w1, w2, w3, w4, w5,
      icmpVal_eq', pred_ult, binVal_add64, set_apply, hn, hnn, hn1, hlt, h47, truthN1]
    intro i hi
    simp [show i ≠ k by omega, show i ≠ k + 1 by omega, show i ≠ k + 2 by omega, show i ≠ k + 3 by omega,
      show i ≠ k + 4 by omega, show i ≠ k + 5 by omega]
  · have hs : sFresh ω t kd = .stuck := by simp [sFresh, hn, hlt]
    refine ⟨fun p W' h => (by rw [hs] at h; cases h), fun _ => ?_⟩
    have hnn : n % 2 ^ 64 = n := by rw [← hn, Nat.mod_mod]
    simp [freshCore, xStmts, evalOpM, evalOp, w0, w1, icmpVal_eq', pred_ult, set_apply, hnn, hlt]
    intro h; exfalso; apply hlt; rw [← hn]; simpa using h


theorem sFresh_ok_lt {ω : Nat → Nat} {t : World} {kd p : Nat} {W' : World} (h : sFresh ω t kd = .ok (p, W')) :
    p < 2 ^ 64 := by
  unfold sFresh at h
  simp only at h
  split at h
  · simp only [Res.ok.injEq, Prod.mk.injEq] at h; rw [← h.1]; exact Nat.mod_lt _ (Nat.two_pow_pos 64)
  · cases h

theorem sFresh_ne_ub (ω : Nat → Nat) (t : World) (kd : Nat) : sFresh ω t kd ≠ .ub := by
  unfold sFresh; simp only; split <;> simp

end PrismRefine
