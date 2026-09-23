/-
PRISM refinement — strict vs LangRef ("lazy poison") semantics.

PRISM's instrumentation checks every poison-producing flag where the
operation happens (`translate.cpp` inserts `check`s at the instruction), i.e.
it implements the strict semantics `sRun`.  The LLVM LangRef semantics
`lRun` lets poison flow and only makes it UB at a branch, a return, or a
divisor.  This file proves the two agree up to the first creation of poison:

* `strict_lazy`: if the strict run returns `v`, runs out of fuel, or is
  stuck, the lazy run does exactly the same and never created poison; if the
  strict run reaches UB, the lazy run reaches UB, creates poison, or is stuck.
-/
import PrismRefine.Llvm

namespace PrismRefine

open PrismSem

/-- A strict register file seen as a lazy one (no poison). -/
def lift (R : SRegs) : LRegs := fun n => (R n).map LV.val

theorem lift_set (R : SRegs) (n : String) (v : Nat) :
    lift (R.set n v) = (lift R).set n (.val v) := by
  funext m; simp only [lift, SRegs.set, LRegs.set]; split <;> rfl

theorem lift_setAll : ∀ (upd : List (String × Nat)) (R : SRegs),
    lift (R.setAll upd) = (lift R).setAll (upd.map (fun p => (p.1, LV.val p.2)))
  | [], _ => rfl
  | (n, v) :: t, R => by
    simp only [SRegs.setAll, List.map, LRegs.setAll]
    rw [lift_setAll t, lift_set]

/-- Operand reads correspond. -/
def OpRel : Res Nat → Res (LV × Bool) → Prop
  | .ok v, l => l = .ok (.val v, false)
  | .stuck, l => l = .stuck
  | .ub, l => l = .ok (.poison, true)

theorem opnd_lift (R : SRegs) (w : Nat) (o : Opnd) : OpRel (sOpnd R w o) (lOpnd (lift R) w o) := by
  cases o with
  | reg n => cases h : R n <;> simp [sOpnd, lOpnd, lift, h, OpRel]
  | const b => rfl
  | poison => rfl

theorem ite_val (b : Bool) (x y : Nat) :
    (if b = true then LV.val x else LV.val y) = LV.val (if b = true then x else y) := by
  split <;> rfl

/-- One instruction: strict and lazy agree, or the strict step is UB and the
lazy step is UB, stuck, or has created poison. -/
def SL : Res SRegs → Res LSt → Prop
  | .ok R', l => l = .ok ⟨lift R', false⟩
  | .stuck, l => l = .stuck
  | .ub, l => l = .ub ∨ l = .stuck ∨ ∃ S', l = .ok S' ∧ S'.c = true

theorem SL_ite (c : Bool) (R' : SRegs) (l : Res LSt) :
    SL (if c = true then .ub else .ok R') l =
      (if c = true then SL .ub l else l = .ok ⟨lift R', false⟩) := by
  split <;> rfl

theorem inst_lift (R : SRegs) (i : Inst) : SL (sInst R i) (lInst ⟨lift R, false⟩ i) := by
  cases i with
  | bin d op fl w a b =>
    have ha := opnd_lift R w a
    have hb := opnd_lift R w b
    simp only [sInst, lInst]
    cases hsa : sOpnd R w a <;> cases hsb : sOpnd R w b <;>
      simp only [hsa, hsb, OpRel] at ha hb <;> rw [ha, hb] <;> simp only [Res.bind, lBin, SL_ite] <;>
      simp only [SL] <;> (repeat' split) <;> simp_all [lift_set]
  | icmp d p w a b =>
    have ha := opnd_lift R w a
    have hb := opnd_lift R w b
    simp only [sInst, lInst]
    cases hsa : sOpnd R w a <;> cases hsb : sOpnd R w b <;>
      simp only [hsa, hsb, OpRel] at ha hb <;> rw [ha, hb] <;> simp [Res.bind, SL, lift_set]
  | select d w c a b =>
    have hc := opnd_lift R 1 c
    have ha := opnd_lift R w a
    have hb := opnd_lift R w b
    simp only [sInst, lInst]
    cases hsc : sOpnd R 1 c <;> cases hsa : sOpnd R w a <;> cases hsb : sOpnd R w b <;>
      simp only [hsc, hsa, hsb, OpRel] at hc ha hb <;> rw [hc, ha, hb] <;>
      simp only [Res.bind, SL, ite_val] <;> (repeat' split) <;> subst_vars <;>
      simp_all [lift_set, selVal, apply_ite]
  | cast d k nneg fw tw a =>
    have ha := opnd_lift R fw a
    simp only [sInst, lInst]
    cases hsa : sOpnd R fw a <;> simp only [hsa, OpRel] at ha <;> rw [ha] <;>
      simp only [Res.bind, SL_ite] <;> simp only [SL] <;> (repeat' split) <;> simp_all [lift_set]

theorem lInst_mono {S S' : LSt} {i : Inst} (h : lInst S i = .ok S') (hc : S.c = true) :
    S'.c = true := by
  cases i with
  | bin d op fl w a b =>
    simp only [lInst] at h
    cases h1 : lOpnd S.R w a <;> cases h2 : lOpnd S.R w b <;> simp [h1, h2, Res.bind] at h
    rename_i A B
    obtain ⟨x, cx⟩ := A; obtain ⟨y, cy⟩ := B
    simp only [lBin] at h
    (repeat' split at h) <;> simp_all <;> (subst h; simp [hc])
  | icmp d p w a b =>
    simp only [lInst] at h
    cases h1 : lOpnd S.R w a <;> cases h2 : lOpnd S.R w b <;> simp [h1, h2, Res.bind] at h
    (repeat' split at h) <;> simp_all <;> (subst h; simp [hc])
  | select d w c a b =>
    simp only [lInst] at h
    cases h0 : lOpnd S.R 1 c <;> cases h1 : lOpnd S.R w a <;> cases h2 : lOpnd S.R w b <;>
      simp [h0, h1, h2, Res.bind] at h
    (repeat' split at h) <;> simp_all <;> (subst h; simp [hc])
  | cast d k nneg fw tw a =>
    simp only [lInst] at h
    cases h1 : lOpnd S.R fw a <;> simp [h1, Res.bind] at h
    (repeat' split at h) <;> simp_all <;> (subst h; simp [hc])

/-- Once poison has been created, the rest of a block stays "bad". -/
def LBad : Res LSt → Prop
  | .ok S => S.c = true
  | .ub => True
  | .stuck => True

theorem lInsts_mono : ∀ (is : List Inst) (S : LSt), S.c = true → LBad (lInsts S is)
  | [], S, hc => hc
  | i :: is, S, hc => by
    simp only [lInsts]
    cases h : lInst S i with
    | ok S' => exact lInsts_mono is S' (lInst_mono h hc)
    | ub => trivial
    | stuck => trivial

theorem insts_lift : ∀ (is : List Inst) (R : SRegs), SL (sInsts R is) (lInsts ⟨lift R, false⟩ is)
  | [], R => rfl
  | i :: is, R => by
    simp only [sInsts, lInsts]
    have h1 := inst_lift R i
    cases hs : sInst R i with
    | ok R' =>
      rw [hs] at h1; simp only [SL] at h1; rw [h1]
      exact insts_lift is R'
    | stuck => rw [hs] at h1; simp only [SL] at h1; rw [h1]; rfl
    | ub =>
      rw [hs] at h1; simp only [SL] at h1
      simp only [Res.bind, SL]
      rcases h1 with h1 | h1 | ⟨S', h1, hc⟩
      · rw [h1]; simp [Res.bind]
      · rw [h1]; simp [Res.bind]
      · rw [h1]; simp only [Res.bind]
        have := lInsts_mono is S' hc
        cases h : lInsts S' is with
        | ok S'' => rw [h] at this; exact .inr (.inr ⟨S'', rfl, this⟩)
        | ub => exact .inl rfl
        | stuck => exact .inr (.inl rfl)

/-- Phis. -/
theorem phis_lift (F : LFunc) (R : SRegs) (prev : Option Nat) : ∀ (ps : List PhiI),
    (∀ upd, sPhis F R prev ps = .ok upd →
      lPhis F (lift R) prev ps = .ok (upd.map (fun p => (p.1, LV.val p.2)), false)) ∧
    (sPhis F R prev ps = .stuck → lPhis F (lift R) prev ps = .stuck) ∧
    (sPhis F R prev ps = .ub → lPhis F (lift R) prev ps = .ub ∨ lPhis F (lift R) prev ps = .stuck ∨
      ∃ upd, lPhis F (lift R) prev ps = .ok (upd, true))
  | [] => by simp [sPhis, lPhis]
  | p :: ps => by
    obtain ⟨ih1, ih2, ih3⟩ := phis_lift F R prev ps
    cases prev with
    | none => simp [sPhis, lPhis]
    | some pv =>
      simp only [sPhis, lPhis]
      cases hpk : phiPick F pv p.inc with
      | none => simp
      | some o =>
        simp only
        have ho := opnd_lift R p.w o
        cases hso : sOpnd R p.w o <;> simp only [hso, OpRel] at ho <;> rw [ho] <;> simp only [Res.bind]
        · cases hsp : sPhis F R (some pv) ps with
          | ok upd => simp [ih1 upd hsp]
          | stuck => simp [ih2 hsp]
          | ub =>
            rcases ih3 hsp with h | h | ⟨upd, h⟩ <;> simp [h]
        · simp only [reduceCtorEq, false_implies, true_and, forall_const, imp_self]
          cases hl : lPhis F (lift R) (some pv) ps with
          | ok uc => simp
          | ub => simp
          | stuck => simp
        · simp
/-! ## Runs -/

/-- Strict and lazy outcomes correspond. -/
def OL : Out → LOut → Prop
  | .ret v, l => l = .ret v false
  | .fuel, l => l = .fuel false
  | .stuck, l => l = .stuck
  | .ub, l => l.bad = true ∨ l = .stuck

theorem lRun_mono (F : LFunc) : ∀ (n : Nat) (prev : Option Nat) (cur : Nat) (S : LSt),
    S.c = true → (lRun F n prev cur S).bad = true ∨ lRun F n prev cur S = .stuck
  | 0, _, _, S, hc => by simp [lRun, LOut.bad, hc]
  | n + 1, prev, cur, S, hc => by
    simp only [lRun]
    cases hb : F.blocks[cur]? with
    | none => simp
    | some B =>
      simp only
      cases hp : lPhis F S.R prev B.phis with
      | ub => simp [Res.lout, LOut.bad]
      | stuck => simp [Res.lout]
      | ok uc =>
        obtain ⟨upd, c1⟩ := uc
        simp only [Res.lout]
        have hm := lInsts_mono B.insts ⟨S.R.setAll upd, S.c || c1⟩ (by simp [hc])
        cases hi : lInsts ⟨S.R.setAll upd, S.c || c1⟩ B.insts with
        | ub => simp [Res.lout, LOut.bad]
        | stuck => simp [Res.lout]
        | ok S' =>
          rw [hi] at hm; simp only [LBad] at hm
          simp only [Res.lout]
          cases ht : B.term with
          | br t =>
            simp only [lTerm]; split
            · exact lRun_mono F n _ _ S' hm
            · simp
          | cbr c t f =>
            simp only [lTerm]; split
            · split
              · exact lRun_mono F n _ _ S' hm
              · simp
            · simp [LOut.bad]
            · simp [LOut.bad]
            · simp
          | ret o =>
            cases o with
            | none => simp [lTerm, LOut.bad, hm]
            | some o =>
              simp only [lTerm]; split <;> simp [LOut.bad, hm]
          | unreachable => simp [lTerm, LOut.bad]

theorem term_lift (F : LFunc) (R : SRegs) (runS : Nat → SRegs → Out) (runL : Nat → LSt → LOut)
    (hrun : ∀ j R, OL (runS j R) (runL j ⟨lift R, false⟩)) (t : LTerm) :
    OL (sTerm F R runS t) (lTerm F ⟨lift R, false⟩ runL t) := by
  cases t with
  | br tn =>
    simp only [sTerm, lTerm]; split
    · exact hrun _ R
    · rfl
  | cbr c tn fn =>
    have hc := opnd_lift R 1 c
    simp only [sTerm, lTerm]
    cases hs : sOpnd R 1 c <;> simp only [hs, OpRel] at hc <;> rw [hc] <;> simp only [Res.out]
    · split
      · exact hrun _ R
      · rfl
    · simp [OL, LOut.bad]
    · rfl
  | ret o =>
    cases o with
    | none => rfl
    | some o =>
      have hc := opnd_lift R F.retw o
      simp only [sTerm, lTerm]
      cases hs : sOpnd R F.retw o <;> simp only [hs, OpRel] at hc <;> rw [hc] <;>
        simp [Res.out, OL, LOut.bad]
  | unreachable => simp [sTerm, lTerm, OL, LOut.bad]

theorem run_lift (F : LFunc) : ∀ (n : Nat) (prev : Option Nat) (cur : Nat) (R : SRegs),
    OL (sRun F n prev cur R) (lRun F n prev cur ⟨lift R, false⟩)
  | 0, _, _, _ => rfl
  | n + 1, prev, cur, R => by
    simp only [sRun, lRun]
    cases hb : F.blocks[cur]? with
    | none => rfl
    | some B =>
      simp only
      have hp := phis_lift F R prev B.phis
      cases hs : sPhis F R prev B.phis with
      | ok upd =>
        rw [hp.1 upd hs]
        simp only [Res.out, Res.lout, Bool.or_false]
        rw [← lift_setAll]
        have hi := insts_lift B.insts (R.setAll upd)
        cases hsi : sInsts (R.setAll upd) B.insts with
        | ok R' =>
          rw [hsi] at hi; simp only [SL] at hi; rw [hi]
          exact term_lift F R' _ _ (fun j R'' => run_lift F n (some cur) j R'') B.term
        | stuck => rw [hsi] at hi; simp only [SL] at hi; rw [hi]; rfl
        | ub =>
          rw [hsi] at hi; simp only [SL] at hi
          simp only [Res.out, OL]
          rcases hi with hi | hi | ⟨S', hi, hc⟩
          · rw [hi]; simp [Res.lout, LOut.bad]
          · rw [hi]; simp [Res.lout]
          · rw [hi]; simp only [Res.lout]
            cases ht : B.term with
            | br t =>
              simp only [lTerm]; split
              · exact lRun_mono F n _ _ S' hc
              · simp
            | cbr c t f =>
              simp only [lTerm]; split
              · split
                · exact lRun_mono F n _ _ S' hc
                · simp
              · simp [LOut.bad]
              · simp [LOut.bad]
              · simp
            | ret o =>
              cases o with
              | none => simp [lTerm, LOut.bad, hc]
              | some o => simp only [lTerm]; split <;> simp [LOut.bad, hc]
            | unreachable => simp [lTerm, LOut.bad]
      | stuck => rw [hp.2.1 hs]; rfl
      | ub =>
        simp only [Res.out, OL]
        rcases hp.2.2 hs with h | h | ⟨upd, h⟩
        · rw [h]; simp [Res.lout, LOut.bad]
        · rw [h]; simp [Res.lout]
        · rw [h]; simp only [Res.lout, Bool.or_true]
          have hm := lInsts_mono B.insts ⟨(lift R).setAll upd, true⟩ rfl
          cases hi : lInsts ⟨(lift R).setAll upd, true⟩ B.insts with
          | ub => simp [Res.lout, LOut.bad]
          | stuck => simp [Res.lout]
          | ok S' =>
            rw [hi] at hm; simp only [LBad] at hm
            simp only [Res.lout]
            cases ht : B.term with
            | br t =>
              simp only [lTerm]; split
              · exact lRun_mono F n _ _ S' hm
              · simp
            | cbr c t f =>
              simp only [lTerm]; split
              · split
                · exact lRun_mono F n _ _ S' hm
                · simp
              · simp [LOut.bad]
              · simp [LOut.bad]
              · simp
            | ret o =>
              cases o with
              | none => simp [lTerm, LOut.bad, hm]
              | some o => simp only [lTerm]; split <;> simp [LOut.bad, hm]
            | unreachable => simp [lTerm, LOut.bad]

theorem lInit_lift (ps : List (String × Nat)) (args : List Nat) :
    lInit ps args = lift (initRegs ps args) := rfl

/-- **Strict vs LangRef semantics.** -/
theorem strict_lazy (F : LFunc) (args : List Nat) (n : Nat) :
    OL (sRunF F args n) (lRunF F args n) := by
  simp only [sRunF, lRunF, lInit_lift]
  exact run_lift F n none 0 _

end PrismRefine
