/-
PRISM refinement, extended fragment — strict vs LangRef semantics of the
machine (`XLlvm.lean`), as `Lazy.lean` for the base fragment:

* `strict_lazyX`: for every oracle, if the strict run returns `v`, runs out
  of fuel, or is stuck, the LangRef run does the same and never created
  poison; if the strict run reaches UB, the LangRef run reaches UB, creates
  poison, or is stuck.
-/
import PrismRefine.Lazy
import PrismRefine.XLlvm

namespace PrismRefine

open PrismSem

/-! ## One instruction -/

def SLX : Res (SRegs × World) → Res (LSt × World) → Prop
  | .ok (R', t'), l => l = .ok (⟨lift R', false⟩, t')
  | .stuck, l => l = .stuck
  | .ub, l => l = .ub ∨ l = .stuck ∨ ∃ S' t', l = .ok (S', t') ∧ S'.c = true

theorem lower_lift (R : SRegs) : lower (lift R) = R := by
  funext n; simp only [lower, lift]
  cases R n with
  | none => rfl
  | some x => cases x <;> rfl

theorem sinst_liftX (ω : Nat → Nat) (R : SRegs) (t : World) (i : SInst) :
    SLX (sSInst ω R t i) (lSInst ω ⟨lift R, false⟩ t i) := by
  cases i with
  | i x =>
    have h := inst_lift R x
    simp only [sSInst, lSInst]
    cases hs : sInst R x with
    | ok R' => rw [hs] at h; simp only [SL] at h; rw [h]; rfl
    | stuck => rw [hs] at h; simp only [SL] at h; rw [h]; rfl
    | ub =>
      rw [hs] at h; simp only [SL] at h
      simp only [Res.bind, SLX]
      rcases h with h | h | ⟨S', h, hc⟩
      · rw [h]; exact .inl rfl
      · rw [h]; exact .inr (.inl rfl)
      · rw [h]; exact .inr (.inr ⟨S', _, rfl, hc⟩)
  | freeze d w a =>
    cases a with
    | undef => simp [sSInst, lSInst, SLX, lift_set]
    | o o =>
      have h := opnd_lift R w o
      simp only [sSInst, lSInst]
      cases hs : sOpnd R w o with
      | ok v => rw [hs] at h; simp only [OpRel] at h; rw [h]; simp [Res.bind, SLX, lift_set]
      | stuck => rw [hs] at h; simp only [OpRel] at h; rw [h]; rfl
      | ub =>
        rw [hs] at h; simp only [OpRel] at h
        rcases h with h | h
        · rw [h]; simp [Res.bind, SLX]
        · rw [h]; simp [Res.bind, SLX]
  | alloca d size al => simp [sSInst, lSInst, SLX, lift_set]
  | load d w p al =>
    have h := opnd_lift R 64 p
    simp only [sSInst, lSInst]
    cases hs : sOpnd R 64 p with
    | ok pv =>
      rw [hs] at h; simp only [OpRel] at h; rw [h]
      simp only [Res.bind]
      split
      · exact .inl rfl
      · split
        · rename_i hn
          exact .inr (.inr ⟨_, t, rfl, by simp [hn]⟩)
        · rename_i hn
          simp only [Bool.not_eq_true] at hn
          simp [SLX, lift_set, hn]
    | stuck => rw [hs] at h; simp only [OpRel] at h; rw [h]; rfl
    | ub =>
      rw [hs] at h; simp only [OpRel] at h
      rcases h with h | h <;> rw [h] <;> exact .inl rfl
  | store w v p al =>
    have h := opnd_lift R 64 p
    simp only [sSInst, lSInst, lower_lift]
    cases hv : sStoreVal ω R t w v with
    | ok a =>
      obtain ⟨vv, init, W1⟩ := a
      simp only [Res.bind]
      cases hs : sOpnd R 64 p with
      | ok pv =>
        rw [hs] at h; simp only [OpRel] at h; rw [h]
        simp only [Res.bind]
        split
        · exact .inl rfl
        · rfl
      | stuck => rw [hs] at h; simp only [OpRel] at h; rw [h]; rfl
      | ub =>
        rw [hs] at h; simp only [OpRel] at h
        rcases h with h | h <;> rw [h] <;> exact .inl rfl
    | ub => exact .inl rfl
    | stuck => rfl
  | gep d inb base ix =>
    simp only [sSInst, lSInst, lower_lift]
    have e : ((sOpnd R 64 base).bind fun b => (sIdxVals R ix).bind fun vs =>
        (gepVal t.mem inb b ix vs).bind fun r => Res.ok (R.set d r, t)) =
        ((sOpnd R 64 base).bind fun b => (sIdxVals R ix).bind fun vs => gepVal t.mem inb b ix vs).bind
          (fun r => Res.ok (R.set d r, t)) := by
      cases sOpnd R 64 base <;> simp only [Res.bind]
      rename_i b
      cases sIdxVals R ix <;> simp only [Res.bind]
    rw [e]
    cases hg : ((sOpnd R 64 base).bind fun b => (sIdxVals R ix).bind fun vs => gepVal t.mem inb b ix vs) with
    | ok r => simp [Res.bind, SLX, lift_set]
    | ub => exact .inr (.inr ⟨_, t, rfl, rfl⟩)
    | stuck => rfl

theorem lSInst_mono {ω : Nat → Nat} {S S' : LSt} {t t' : World} {i : SInst}
    (h : lSInst ω S t i = .ok (S', t')) (hc : S.c = true) : S'.c = true := by
  cases i with
  | i x =>
    simp only [lSInst] at h
    cases hl : lInst S x with
    | ok S1 =>
      rw [hl] at h; simp only [Res.bind, Res.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, _⟩ := h; exact lInst_mono hl hc
    | ub => rw [hl] at h; cases h
    | stuck => rw [hl] at h; cases h
  | freeze d w a =>
    cases a with
    | undef => simp only [lSInst, Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; exact hc
    | o o =>
      simp only [lSInst] at h
      cases hl : lOpnd S.R w o with
      | ok xc =>
        obtain ⟨x, cx⟩ := xc
        rw [hl] at h; simp only [Res.bind] at h
        split at h
        · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; simp [hc]
        · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; simp [hc]
        · cases h
      | ub => rw [hl] at h; cases h
      | stuck => rw [hl] at h; cases h
  | alloca d size al => simp only [lSInst, Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; exact hc
  | load d w p al =>
    simp only [lSInst] at h
    split at h
    · split at h
      · cases h
      · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; simp [hc]
    all_goals cases h
  | store w v p al =>
    simp only [lSInst] at h
    cases hv : sStoreVal ω (lower S.R) t w v with
    | ok a =>
      rw [hv] at h; simp only [Res.bind] at h
      split at h
      · split at h
        · cases h
        · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; exact hc
      all_goals cases h
    | ub => rw [hv] at h; cases h
    | stuck => rw [hv] at h; cases h
  | gep d inb base ix =>
    simp only [lSInst] at h
    split at h
    · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; exact hc
    · simp only [Res.ok.injEq, Prod.mk.injEq] at h; obtain ⟨rfl, _⟩ := h; rfl
    · cases h

/-- Once poison has been created, the rest of a segment stays bad. -/
def LBadX : Res (LSt × World) → Prop
  | .ok (S, _) => S.c = true
  | .ub => True
  | .stuck => True

theorem lSInsts_mono (ω : Nat → Nat) : ∀ (is : List SInst) (S : LSt) (t : World), S.c = true →
    LBadX (lSInsts ω S t is)
  | [], S, t, hc => hc
  | i :: is, S, t, hc => by
    simp only [lSInsts]
    cases h : lSInst ω S t i with
    | ok a => obtain ⟨S', t'⟩ := a; exact lSInsts_mono ω is S' t' (lSInst_mono h hc)
    | ub => trivial
    | stuck => trivial

theorem sinsts_liftX (ω : Nat → Nat) : ∀ (is : List SInst) (R : SRegs) (t : World),
    SLX (sSInsts ω R t is) (lSInsts ω ⟨lift R, false⟩ t is)
  | [], R, t => rfl
  | i :: is, R, t => by
    simp only [sSInsts, lSInsts]
    have h1 := sinst_liftX ω R t i
    cases hs : sSInst ω R t i with
    | ok a =>
      obtain ⟨R', t'⟩ := a
      rw [hs] at h1; simp only [SLX] at h1; rw [h1]
      exact sinsts_liftX ω is R' t'
    | stuck => rw [hs] at h1; simp only [SLX] at h1; rw [h1]; rfl
    | ub =>
      rw [hs] at h1; simp only [SLX] at h1
      simp only [Res.bind, SLX]
      rcases h1 with h1 | h1 | ⟨S', t', h1, hc⟩
      · rw [h1]; exact .inl rfl
      · rw [h1]; exact .inr (.inl rfl)
      · rw [h1]; simp only [Res.bind]
        have := lSInsts_mono ω is S' t' hc
        cases h : lSInsts ω S' t' is with
        | ok a => obtain ⟨S'', t''⟩ := a; rw [h] at this; exact .inr (.inr ⟨S'', t'', rfl, this⟩)
        | ub => exact .inl rfl
        | stuck => exact .inr (.inl rfl)

/-! ## Frames and steps -/

def liftFr (fr : Frame) : LFrame :=
  { F := fr.F, prev := fr.prev, cur := fr.cur, seg := fr.seg, R := lift fr.R, pend := fr.pend }

/-- Block entry. -/
def SLE : Res SRegs → Res LSt → Prop
  | .ok R0, l => l = .ok ⟨lift R0, false⟩
  | .stuck, l => l = .stuck
  | .ub, l => l = .ub ∨ l = .stuck ∨ ∃ S, l = .ok S ∧ S.c = true

theorem enter_lift (fr : Frame) (B : XBlock) : SLE (sEnter fr B) (lEnter (liftFr fr) B false) := by
  by_cases hs : fr.seg = 0
  · have hp := phis_lift fr.F.shape fr.R fr.prev B.phis
    simp only [sEnter, lEnter, hs, liftFr, ite_true]
    cases hsp : sPhis fr.F.shape fr.R fr.prev B.phis with
    | ok upd => rw [hp.1 upd hsp]; simp [Res.bind, SLE, lift_setAll]
    | stuck => rw [hp.2.1 hsp]; rfl
    | ub =>
      simp only [Res.bind, SLE]
      rcases hp.2.2 hsp with h | h | ⟨upd, h⟩
      · rw [h]; exact .inl rfl
      · rw [h]; exact .inr (.inl rfl)
      · rw [h]; exact .inr (.inr ⟨_, rfl, by simp⟩)
  · simp only [sEnter, lEnter, hs, liftFr, ite_false]
    cases B.segs[fr.seg - 1]? with
    | none => rfl
    | some sc =>
      obtain ⟨_, c⟩ := sc
      simp only
      cases c.dst with
      | none => rfl
      | some d =>
        cases fr.pend with
        | none => rfl
        | some v => simp [SLE, lift_set]

theorem lEnter_mono {fr : LFrame} {B : XBlock} {S : LSt} (h : lEnter fr B true = .ok S) : S.c = true := by
  unfold lEnter at h
  split at h
  · cases hp : lPhis fr.F.shape fr.R fr.prev B.phis with
    | ok uc => rw [hp] at h; simp only [Res.bind, Res.ok.injEq] at h; subst h; simp
    | ub => rw [hp] at h; cases h
    | stuck => rw [hp] at h; cases h
  · split at h
    · split at h
      · split at h
        · simp only [Res.ok.injEq] at h; subst h; rfl
        · cases h
      · simp only [Res.ok.injEq] at h; subst h; rfl
    · cases h

def SLStep : Step → LStep → Prop
  | .next st t, l => l = .next (st.map liftFr) t false
  | .ret v, l => l = .ret v false
  | .stuck, l => l = .stuck
  | .ub, l => l = .ub ∨ l = .stuck ∨ (∃ st t, l = .next st t true) ∨ ∃ v, l = .ret v true

def ArgRel : Res (List Nat) → Res (List LV × Bool) → Prop
  | .ok vs, l => l = .ok (vs.map LV.val, false)
  | .stuck, l => l = .stuck
  | .ub, l => l = .ub ∨ l = .stuck ∨ ∃ vs, l = .ok (vs, true)

theorem args_lift (R : SRegs) : ∀ (args : List (Opnd × Nat)), ArgRel (sArgs R args) (lArgs (lift R) args)
  | [] => rfl
  | (o, w) :: t => by
    have h := opnd_lift R w o
    have ih := args_lift R t
    simp only [sArgs, lArgs]
    cases hs : sOpnd R w o with
    | ok v =>
      rw [hs] at h; simp only [OpRel] at h; rw [h]
      simp only [Res.bind]
      cases ht : sArgs R t with
      | ok vs => rw [ht] at ih; simp only [ArgRel] at ih; rw [ih]; rfl
      | stuck => rw [ht] at ih; simp only [ArgRel] at ih; rw [ih]; rfl
      | ub =>
        rw [ht] at ih; simp only [ArgRel] at ih
        rcases ih with ih | ih | ⟨vs, ih⟩ <;> rw [ih]
        · exact .inl rfl
        · exact .inr (.inl rfl)
        · exact .inr (.inr ⟨LV.val v :: vs, by simp [Res.bind]⟩)
    | stuck => rw [hs] at h; simp only [OpRel] at h; rw [h]; rfl
    | ub =>
      rw [hs] at h; simp only [OpRel] at h
      simp only [Res.bind, ArgRel]
      rcases h with h | h
      · rw [h]
        cases ht : lArgs (lift R) t with
        | ok a => obtain ⟨vs, c2⟩ := a; exact .inr (.inr ⟨LV.poison :: vs, by simp [Res.bind]⟩)
        | ub => exact .inl rfl
        | stuck => exact .inr (.inl rfl)
      · rw [h]; exact .inl rfl

theorem lift_bindArgs : ∀ (ps : List (String × Nat)) (vs : List Nat),
    lift (bindArgs ps vs) = lBindArgs ps (vs.map LV.val)
  | [], _ => by funext n; simp [lift, bindArgs, lBindArgs]
  | (p, w) :: ps, [] => by funext n; simp [lift, bindArgs, lBindArgs]
  | (p, w) :: ps, v :: vs => by
    funext n
    simp only [bindArgs, lBindArgs, List.map_cons]
    rw [← lift_bindArgs ps vs]
    simp only [lift]
    split <;> rfl

/-- A LangRef step result that is bad (poison created, UB) or stuck. -/
def LStep.badish (l : LStep) : Prop :=
  l = .ub ∨ l = .stuck ∨ (∃ st t, l = .next st t true) ∨ ∃ v, l = .ret v true

theorem lRetTo_bad (rest : List LFrame) (v : Option Nat) (t : World) : (lRetTo rest v t true).badish := by
  cases rest with
  | nil => exact .inr (.inr (.inr ⟨v, rfl⟩))
  | cons f fs => exact .inr (.inr (.inl ⟨_, t, rfl⟩))

theorem lEnd_mono (M : XMod) (fr : LFrame) (rest : List LFrame) (B : XBlock) (S : LSt) (t : World)
    (hc : S.c = true) : (lEnd M fr rest B S t).badish := by
  unfold lEnd
  split
  · rename_i is cl _
    cases ha : lArgs S.R cl.args with
    | ok a =>
      obtain ⟨vs, ca⟩ := a
      simp only [Res.lstep]
      split
      · split
        · exact .inr (.inr (.inl ⟨_, t, by rw [hc, Bool.true_or]⟩))
        · exact .inr (.inl rfl)
      · exact .inr (.inl rfl)
    | ub => exact .inl rfl
    | stuck => exact .inr (.inl rfl)
  · split
    · split
      · exact .inr (.inr (.inl ⟨_, t, by rw [hc]⟩))
      · exact .inr (.inl rfl)
    · split
      · split
        · exact .inr (.inr (.inl ⟨_, t, by rw [hc]⟩))
        · exact .inr (.inl rfl)
      all_goals first | exact .inl rfl | exact .inr (.inl rfl)
    · rw [hc]; exact lRetTo_bad _ _ _
    · split
      · simp only [hc, Bool.true_or]; exact lRetTo_bad _ _ _
      all_goals first | exact .inl rfl | exact .inr (.inl rfl)
    · exact .inl rfl

theorem bad_cont (M : XMod) (ω : Nat → Nat) (fr : LFrame) (rest : List LFrame) (B : XBlock) (S : LSt)
    (t : World) (hc : S.c = true) (is : List SInst) :
    ((lSInsts ω S t is).lstep fun (S', t') => lEnd M fr rest B S' t').badish := by
  have := lSInsts_mono ω is S t hc
  cases h : lSInsts ω S t is with
  | ok a =>
    obtain ⟨S', t'⟩ := a
    rw [h] at this
    exact lEnd_mono M fr rest B S' t' this
  | ub => exact .inl rfl
  | stuck => exact .inr (.inl rfl)

theorem SLStep.of_badish {s : Step} {l : LStep} (hs : s = .ub) (hl : l.badish) : SLStep s l := by
  subst hs; exact hl

theorem end_lift (M : XMod) (fr : Frame) (rest : List Frame) (B : XBlock) (R : SRegs) (t : World) :
    SLStep (sEnd M fr rest B R t) (lEnd M (liftFr fr) (rest.map liftFr) B ⟨lift R, false⟩ t) := by
  unfold sEnd lEnd
  simp only [liftFr]
  split
  · rename_i is cl hsg
    have ha := args_lift R cl.args
    cases hs : sArgs R cl.args with
    | ok vs =>
      rw [hs] at ha; simp only [ArgRel] at ha
      simp only [Res.step, ha, Res.lstep]
      cases M.find cl.f with
      | none => rfl
      | some G =>
        simp only [List.length_map]
        split
        · simp [SLStep, liftFr, lift_bindArgs]
        · rfl
    | stuck => rw [hs] at ha; simp only [ArgRel] at ha; simp [Res.step, ha, Res.lstep, SLStep]
    | ub =>
      rw [hs] at ha; simp only [ArgRel] at ha
      simp only [Res.step]
      rcases ha with ha | ha | ⟨vs, ha⟩
      · rw [ha]; exact .inl rfl
      · rw [ha]; exact .inr (.inl rfl)
      · rw [ha]; simp only [Res.lstep, Bool.false_or]
        cases M.find cl.f with
        | none => exact .inr (.inl rfl)
        | some G =>
          simp only
          split
          · exact .inr (.inr (.inl ⟨_, t, rfl⟩))
          · exact .inr (.inl rfl)
  · cases htm : B.term with
    | br tn =>
      simp only
      split
      · simp [SLStep, liftFr]
      · rfl
    | cbr cnd tn fn =>
      simp only
      have h := opnd_lift R 1 cnd
      cases hs : sOpnd R 1 cnd with
      | ok v =>
        rw [hs] at h; simp only [OpRel] at h
        simp only [Res.step, h]
        split
        · simp [SLStep, liftFr]
        · rfl
      | stuck => rw [hs] at h; simp only [OpRel] at h; simp [Res.step, h, SLStep]
      | ub =>
        rw [hs] at h; simp only [OpRel] at h
        rcases h with h | h <;> simp [Res.step, h, SLStep]
    | ret o =>
      cases o with
      | none =>
        simp only
        cases rest with
        | nil => rfl
        | cons c cs => simp [retTo, lRetTo, SLStep, liftFr]
      | some o =>
        simp only
        have h := opnd_lift R fr.F.retw o
        cases hs : sOpnd R fr.F.retw o with
        | ok v =>
          rw [hs] at h; simp only [OpRel] at h
          simp only [Res.step, h, Bool.or_false]
          cases rest with
          | nil => rfl
          | cons c cs => simp [retTo, lRetTo, SLStep, liftFr]
        | stuck => rw [hs] at h; simp only [OpRel] at h; simp [Res.step, h, SLStep]
        | ub =>
          rw [hs] at h; simp only [OpRel] at h
          rcases h with h | h <;> simp [Res.step, h, SLStep]
    | unreachable => exact .inl rfl

theorem step_lift (M : XMod) (ω : Nat → Nat) (t : World) :
    ∀ st, SLStep (sStep M ω t st) (lStep M ω t false (st.map liftFr))
  | [] => rfl
  | fr :: rest => by
    simp only [sStep, lStep, List.map_cons]
    have hfb : (liftFr fr).F.blocks[(liftFr fr).cur]? = fr.F.blocks[fr.cur]? := rfl
    rw [hfb]
    cases hb : fr.F.blocks[fr.cur]? with
    | none => rfl
    | some B =>
      simp only
      have he := enter_lift fr B
      have hseg : (liftFr fr).seg = fr.seg := rfl
      cases hs : sEnter fr B with
      | ok R0 =>
        rw [hs] at he; simp only [SLE] at he
        simp only [Res.step, he, Res.lstep, hseg]
        have hi := sinsts_liftX ω (B.insts fr.seg) R0 t
        cases hsi : sSInsts ω R0 t (B.insts fr.seg) with
        | ok a =>
          obtain ⟨R, t'⟩ := a
          rw [hsi] at hi; simp only [SLX] at hi
          simp only [hi]
          exact end_lift M fr rest B R t'
        | stuck => rw [hsi] at hi; simp only [SLX] at hi; simp [hi, SLStep]
        | ub =>
          rw [hsi] at hi; simp only [SLX] at hi
          apply SLStep.of_badish rfl
          rcases hi with hi | hi | ⟨S', t', hi, hc⟩
          · rw [hi]; exact .inl rfl
          · rw [hi]; exact .inr (.inl rfl)
          · rw [hi]; exact lEnd_mono M _ _ B S' t' hc
      | stuck => rw [hs] at he; simp only [SLE] at he; simp [Res.step, he, Res.lstep, SLStep]
      | ub =>
        rw [hs] at he; simp only [SLE] at he
        apply SLStep.of_badish rfl
        rcases he with he | he | ⟨S, he, hc⟩
        · rw [he]; exact .inl rfl
        · rw [he]; exact .inr (.inl rfl)
        · rw [he]; exact bad_cont M ω _ _ B S t hc _

theorem lStep_mono (M : XMod) (ω : Nat → Nat) (t : World) (st : List LFrame) :
    (lStep M ω t true st).badish := by
  cases st with
  | nil => exact .inr (.inl rfl)
  | cons fr rest =>
    simp only [lStep]
    cases hb : fr.F.blocks[fr.cur]? with
    | none => exact .inr (.inl rfl)
    | some B =>
      simp only
      cases he : lEnter fr B true with
      | ok S => exact bad_cont M ω fr rest B S t (lEnter_mono he) _
      | ub => exact .inl rfl
      | stuck => exact .inr (.inl rfl)

theorem lRunX_mono (M : XMod) (ω : Nat → Nat) : ∀ (n : Nat) (t : World) (st : List LFrame),
    (lRunX M ω n t true st).bad = true ∨ lRunX M ω n t true st = .stuck
  | 0, _, _ => by simp [lRunX, LOut.bad]
  | n + 1, t, st => by
    simp only [lRunX]
    rcases lStep_mono M ω t st with h | h | ⟨st', t', h⟩ | ⟨v, h⟩ <;> rw [h]
    · simp [LOut.bad]
    · simp
    · exact lRunX_mono M ω n t' st'
    · simp [LOut.bad]

theorem run_liftX (M : XMod) (ω : Nat → Nat) : ∀ (n : Nat) (t : World) (st : List Frame),
    OL (sRunX M ω n t st) (lRunX M ω n t false (st.map liftFr))
  | 0, _, _ => rfl
  | n + 1, t, st => by
    simp only [sRunX, lRunX]
    have h := step_lift M ω t st
    cases hs : sStep M ω t st with
    | next st' t' => rw [hs] at h; simp only [SLStep] at h; rw [h]; exact run_liftX M ω n t' st'
    | ret v => rw [hs] at h; simp only [SLStep] at h; rw [h]; rfl
    | stuck => rw [hs] at h; simp only [SLStep] at h; rw [h]; rfl
    | ub =>
      rw [hs] at h; simp only [SLStep] at h
      simp only [OL]
      rcases h with h | h | ⟨st'', t'', h⟩ | ⟨v, h⟩ <;> rw [h]
      · simp [LOut.bad]
      · simp
      · exact lRunX_mono M ω n t'' st''
      · simp [LOut.bad]

/-- **Strict vs LangRef semantics, extended fragment.** -/
theorem strict_lazyX (M : XMod) (F : XFunc) (args : List Nat) (ω : Nat → Nat) (n : Nat) :
    OL (sRunXF M F args ω n) (lRunXF M F args ω n) := by
  have := run_liftX M ω n World.init [initFrame F args]
  simpa [sRunXF, lRunXF, liftFr, initFrame, lInitFrame, lInit_lift] using this

end PrismRefine
