/-
PRISM refinement, extended fragment — one machine step is one PIR block,
and the translation is exact (`translateX_exact`).
-/
import PrismRefine.XStep
import PrismRefine.XEscape

namespace PrismRefine

open PrismSem

/-! ## The invariant -/

/-- The suspended callers: frame `fr` called instance `ι` from the segment
before `fr.seg`; the callee's `ret` jumps to `fr`'s next PIR block; the
caller's registers are related and it reads only below `hi ≤ lo` of the
callee. -/
def TailX (P : PFunc) (C : List IInfo) : Nat → List Frame → Store → Nat → Prop
  | ι, [], _, _ => ι = 0
  | ι, fr :: rest, σ, lo => ∃ ι' I', C[ι']? = some I' ∧ fr.F = I'.fn ∧ 0 < fr.seg ∧
      (I'.kids[fr.cur]?).bind (·[fr.seg - 1]?) = some ι ∧ fr.pend = none ∧
      (∃ cb, (I'.blks[fr.cur]?).bind (·[fr.seg]?) = some cb ∧ ∀ J, C[ι]? = some J → J.retTo = some cb) ∧
      RelX (I'.ctx P.vars) fr.R σ ∧ I'.hi ≤ lo ∧ TailX P C ι' rest σ I'.lo

/-- What the PIR block needs on entry: the predecessor for the phis
(segment 0), or the continuation phis' entry for the returned value. -/
def EntryX (P : PFunc) (I : IInfo) (fr : Frame) (prevP : Option Nat) (curP : Nat) (σ : Store) : Prop :=
  (fr.seg = 0 → ∀ p, fr.prev = some p → ∃ q, I.tails[p]? = some q ∧ prevP = some q) ∧
  (0 < fr.seg → ∀ v, fr.pend = some v → ∃ PB pu pr p a u, P.blocks[curP]? = some PB ∧
    PB.phis = [pu, pr] ∧ prevP = some p ∧ pickInc p pr.inc = some a ∧ a.get σ = v ∧
    pickInc p pu.inc = some u)

def InvX (P : PFunc) (C : List IInfo) (st : List Frame) (prevP : Option Nat) (curP : Nat) (σ : Store) :
    Prop :=
  ∃ fr rest ι I, st = fr :: rest ∧ C[ι]? = some I ∧ fr.F = I.fn ∧
    RelX (I.ctx P.vars) fr.R σ ∧
    (I.blks[fr.cur]?).bind (·[fr.seg]?) = some curP ∧
    EntryX P I fr prevP curP σ ∧ TailX P C ι rest σ I.lo

theorem TailX.agree {M : XMod} {P : PFunc} {C : List IInfo}
    (hVF : ∀ (ι : Nat) (I : IInfo), C[ι]? = some I → InstFacts M P C I) :
    ∀ {ι : Nat} {rest : List Frame} {σ σ' : Store} {lo : Nat}, TailX P C ι rest σ lo →
    (∀ j, j < lo → σ' j = σ j) → TailX P C ι rest σ' lo
  | _, [], _, _, _, h, _ => h
  | _, _ :: _, _, _, _, ⟨ι', I', hI', hF, hs, hk, hp, hcb, hR, hhi, ht⟩, ha =>
    ⟨ι', I', hI', hF, hs, hk, hp, hcb, RelX.agree (hVF _ _ hI').ctx hR hhi ha, hhi,
      TailX.agree hVF ht (fun j hj => ha j (by have := (hVF _ _ hI').lohi; omega))⟩

/-! ## Helpers -/

/-- The arguments' values (`List.Forall₂` is not in core). -/
inductive ArgVals (σ : Store) : List Arg → List Nat → Prop
  | nil : ArgVals σ [] []
  | cons {a : Arg} {v : Nat} {as : List Arg} {vs : List Nat} :
      a.get σ = v → ArgVals σ as vs → ArgVals σ (a :: as) (v :: vs)

theorem ArgVals.length {σ : Store} : ∀ {as : List Arg} {vs : List Nat}, ArgVals σ as vs → as.length = vs.length
  | [], [], .nil => rfl
  | _ :: _, _ :: _, .cons _ h => by simp [ArgVals.length h]

def Step.out (s : Step) (K : List Frame → World → Out) : Out :=
  match s with
  | .next st t => K st t
  | .ret v => .ret v
  | .ub => .ub
  | .stuck => .stuck

theorem Res.step_out {α : Type} (r : Res α) (f : α → Step) (K : List Frame → World → Out) :
    (r.step f).out K = r.out (fun a => (f a).out K) := by
  cases r <;> rfl

theorem Res.bind_out {α β : Type} (r : Res α) (f : α → Res β) (g : β → Out) :
    (r.bind f).out g = r.out (fun a => (f a).out g) := by
  cases r <;> rfl

theorem XPRes.then_run (p : XPRes) (q : Store → World → XPRes) (g : Store → World → POut) :
    (p.then q).run g = p.run (fun σ t => (q σ t).run g) := by
  cases p <;> rfl

theorem SimX.out {c : Ctx} {σ0 : Store} {r : Res (SRegs × World)} {p : XPRes} (h : SimX c σ0 r p)
    {f : SRegs × World → Out} {g : Store → World → POut}
    (hfg : ∀ R t' σ', RelX c R σ' → (∀ j, j < c.lo → σ' j = σ0 j) → OSim (f (R, t')) (g σ' t')) :
    OSim (r.out f) (p.run g) := by
  cases r with
  | ok a =>
    obtain ⟨R, t'⟩ := a
    obtain ⟨σ', rfl, hr, ha⟩ := h
    exact hfg R t' σ' hr ha
  | ub => simp only [SimX] at h; subst h; rfl
  | stuck => trivial

theorem opndX_osim {P : PFunc} {ω : Nat → Nat} {c : Ctx} {R : SRegs} {σ : Store} (hR : RelX c R σ)
    {w : Nat} {keep : Bool} {k : Nat} {o : Opnd} {s : List PStmt} {tws : List Nat} {A : Arg}
    (h : trOpndX c w keep k o = .ok (s, tws, A)) (hk : c.hi ≤ k) (t : World) (rest : List PStmt)
    (f : Nat → Out) (g : Store → World → POut)
    (hc : ∀ v, sOpnd R w o = .ok v → tws = [] → A.get σ = v → A.below k →
      OSim (f v) ((xStmts P ω σ t rest).run g)) :
    OSim ((sOpnd R w o).out f) ((xStmts P ω σ t (s ++ rest)).run g) := by
  cases o with
  | const b =>
    simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hc _ rfl rfl rfl trivial
  | reg n =>
    simp only [trOpndX] at h
    split at h
    · rename_i a hl
      split at h
      · rename_i hlt
        simp only [Except.ok.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl, rfl⟩ := h
        have hab : a.below k := Arg.below_mono hk (Arg.lt_below hlt)
        cases hn : R n with
        | none => simp [sOpnd, hn, Res.out, OSim]
        | some x =>
          obtain ⟨a', hl', hx⟩ := hR n x hn
          rw [hl] at hl'; cases hl'
          cases x with
          | val v =>
            obtain ⟨hv, hsh⟩ := hx
            rw [shChecks_pass P ω σ t hsh rest]
            have hs : sOpnd R w (.reg n) = .ok v := by simp [sOpnd, hn]
            rw [hs]
            refine hc v hs rfl ?_ ?_
            · cases keep <;> simp [hv]
            · cases keep
              · exact Arg.setW_below w hab
              · exact hab
          | ind =>
            obtain ⟨s', hs', ht⟩ := hx
            simp only [sOpnd, hn, Res.out, OSim, hs', shChecks, List.cons_append, xStmts, ht, ite_true,
              XPRes.run]
      · simp at h
    · simp at h
  | poison =>
    simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    rfl

theorem argsX_osim {P : PFunc} {ω : Nat → Nat} {c : Ctx} {R : SRegs} {σ : Store} (hR : RelX c R σ)
    (t : World) (f : List Nat → Out) (g : Store → World → POut) :
    ∀ {k : Nat} {args3 : List (Opnd × Nat × Nat)} {s : List PStmt} {tws : List Nat} {As : List Arg},
    c.hi ≤ k → trArgsX c k args3 = .ok (s, tws, As) →
    (∀ vs, sArgs R (args3.map fun (o, w, _) => (o, w)) = .ok vs →
      ArgVals σ As vs → OSim (f vs) (g σ t)) →
    OSim ((sArgs R (args3.map fun (o, w, _) => (o, w))).out f) ((xStmts P ω σ t s).run g)
  | k, [], s, tws, As, _, h, hc => by
    simp only [trArgsX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hc [] rfl .nil
  | k, (o, wo, wp) :: rest, s, tws, As, hk, h, hc => by
    simp only [trArgsX] at h
    obtain ⟨⟨s1, t1, A⟩, h1, h⟩ := Except.bind_ok h
    obtain ⟨⟨s2, t2, As'⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [List.map_cons, sArgs, Res.bind_out]
    refine opndX_osim hR h1 hk t s2 _ g ?_
    intro v hv ht1 hAv _
    subst ht1
    simp only [List.length_nil, Nat.add_zero] at h2
    refine argsX_osim hR t _ g hk h2 ?_
    intro vs hvs hF
    simp only [Res.bind, Res.out, hvs]
    exact hc (v :: vs) (by simp [sArgs, hv, hvs, Res.bind]) (.cons (by simp [hAv]) hF)

theorem zip_map_back {α β γ δ : Type} :
    ∀ (l : List (α × β)) (ps : List (γ × δ)), l.length = ps.length →
    (((l.zip ps).map fun ((o, w), (_, wp)) => (o, w, wp)).map fun (o, w, _) => (o, w)) = l
  | [], [], _ => rfl
  | [], _ :: _, h => by simp at h
  | _ :: _, [], h => by simp at h
  | (o, w) :: l, (_, _) :: ps, h => by
    simp only [List.zip_cons_cons, List.map_cons, List.cons.injEq, true_and]
    exact zip_map_back l ps (by simpa using h)

theorem bindArgs_rel {J : IInfo} {vars : List Nat} {σ : Store} :
    ∀ (ps : List (String × Nat)) (As : List Arg) (vs : List Nat),
    paramsOK J ps As = true → ArgVals σ As vs →
    RelX (J.ctx vars) (bindArgs ps vs) σ
  | [], _, _, _, _ => by intro n x h; simp [bindArgs] at h
  | (p, w) :: ps, As, vs, hp, hF => by
    cases hF with
    | nil => intro n x h; simp [bindArgs] at h
    | @cons A v As' vs' hAv hF' =>
      simp only [paramsOK, List.zip_cons_cons, List.all_cons, Bool.and_eq_true, beq_iff_eq,
        Option.isNone_iff_eq_none] at hp
      obtain ⟨⟨hl, hsh⟩, hrest⟩ := hp
      intro n x h
      simp only [bindArgs] at h
      by_cases e : n = p
      · subst e
        simp only [ite_true, Option.some.injEq] at h; subst h
        refine ⟨A, hl, hAv, fun s hs => ?_⟩
        have := Ctx.shOf_some hs
        simp only [IInfo.ctx] at this
        rw [hsh] at this; cases this
      · simp only [e, ite_false] at h
        exact bindArgs_rel ps As' vs' hrest hF' n x h

theorem getLastD_of {l : List Nat} {n x : Nat} (hl : l.length = n + 1) (hx : l[n]? = some x) (d : Nat) :
    l.getLastD d = x := by
  rw [List.getLastD_eq_getLast?, List.getLast?_eq_getElem?, hl, Nat.add_sub_cancel, hx]; rfl

theorem headD_of {l : List Nat} {x : Nat} (hx : l[0]? = some x) (d : Nat) : l.headD d = x := by
  cases l with
  | nil => simp at hx
  | cons y ys => simp at hx; simp [hx]

theorem lookupBlock_lt {F : LFunc} {n : String} {j : Nat} (h : lookupBlock F n = some j) :
    j < F.blocks.length := by
  unfold lookupBlock at h
  exact (List.findIdx?_eq_some_iff_getElem.mp h).1

theorem shape_len (G : XFunc) : G.shape.blocks.length = G.blocks.length := by
  simp [XFunc.shape]

/-- A branch target: the PIR block of the target's first segment. -/
theorem targetX_spec {M : XMod} {P : PFunc} {C : List IInfo} {I : IInfo} (hIF : InstFacts M P C I)
    {t : String} {h : Nat} (ht : targetX I.fn I.heads t = .ok h) :
    ∃ j, lookupBlock I.fn.shape t = some j ∧ (I.blks[j]?).bind (·[0]?) = some h := by
  unfold targetX at ht
  split at ht
  · rename_i j hj
    split at ht
    · rename_i h' hh
      simp only [Except.ok.injEq] at ht; subst ht
      refine ⟨j, hj, ?_⟩
      have hjl : j < I.fn.blocks.length := by rw [← shape_len]; exact lookupBlock_lt hj
      obtain ⟨bl, hbl, hlen⟩ := hIF.blen j I.fn.blocks[j] (List.getElem?_eq_getElem hjl)
      simp only [IInfo.heads, List.getElem?_map, hbl, Option.map_some, Option.some.injEq] at hh
      simp only [hbl, Option.bind_some]
      have : 0 < bl.length := by omega
      rw [List.getElem?_eq_getElem this, ← hh]
      cases bl with
      | nil => simp at this
      | cons y ys => rfl
    · simp at ht
  · simp at ht

theorem tails_of {I : IInfo} {b : Nat} {bl : List Nat}
    {B : XBlock} {x : Nat} (hbl : I.blks[b]? = some bl) (hlen : bl.length = B.segs.length + 1)
    (hx : bl[B.segs.length]? = some x) : I.tails[b]? = some x := by
  simp only [IInfo.tails, List.getElem?_map, hbl, Option.map_some, getLastD_of hlen hx]

theorem sArgs_length {R : SRegs} : ∀ {args : List (Opnd × Nat)} {vs : List Nat},
    sArgs R args = .ok vs → vs.length = args.length
  | [], vs, h => by simp only [sArgs] at h; cases h; rfl
  | (o, w) :: t, vs, h => by
    simp only [sArgs] at h
    cases h1 : sOpnd R w o with
    | ok v =>
      rw [h1] at h; simp only [Res.bind] at h
      cases h2 : sArgs R t with
      | ok vs' => rw [h2] at h; simp only [Res.bind] at h; cases h; simp [sArgs_length h2]
      | ub => rw [h2] at h; cases h
      | stuck => rw [h2] at h; cases h
    | ub => rw [h1] at h; cases h
    | stuck => rw [h1] at h; cases h

/-! ## Block entry -/

def EnterOK (c : Ctx) (σ0 σ1 : Store) : Res SRegs → Prop
  | .ok R0 => RelX c R0 σ1 ∧ ∀ j, j < c.lo → σ1 j = σ0 j
  | .ub => False
  | .stuck => True

theorem enter_sim {M : XMod} {P : PFunc} {C : List IInfo} {I : IInfo} (hIF : InstFacts M P C I)
    {fr : Frame} (hfF : fr.F = I.fn) {B : XBlock} {PB : PBlock} {prevP : Option Nat} {curP : Nat}
    {σ : Store} (hPB : P.blocks[curP]? = some PB) (hR : RelX (I.ctx P.vars) fr.R σ)
    (hE : EntryX P I fr prevP curP σ)
    (h0 : fr.seg = 0 → trPhisX I.fn (I.ctx P.vars) I.tails B.phis = .ok PB.phis)
    (h1 : 0 < fr.seg → ∃ is cl, B.segs[fr.seg - 1]? = some (is, cl) ∧
      contOK (I.ctx P.vars) cl PB.phis = true) :
    EnterOK (I.ctx P.vars) σ (σ.setAll (phiUpd σ prevP PB.phis)) (sEnter fr B) := by
  have hctx := hIF.ctx
  have hlohi := hIF.lohi
  by_cases hs : fr.seg = 0
  · have hph := phisX_sim (G := I.fn) hIF.tails hR fr.prev prevP (hE.1 hs) B.phis PB.phis (h0 hs)
    simp only [sEnter, hs, ite_true, hfF]
    cases hsp : sPhis I.fn.shape fr.R fr.prev B.phis with
    | ub => exact hph.1 hsp
    | stuck => trivial
    | ok upd => exact RelX.setAllX (hph.2 upd hsp) hR
  · have hpos : 0 < fr.seg := Nat.pos_of_ne_zero hs
    obtain ⟨is, cl, hsg, hco⟩ := h1 hpos
    simp only [sEnter, hs, ite_false, hsg]
    rcases contOK_spec hco with hnil | ⟨pu, pr, hph, hpu, hd, hdn⟩
    · cases hdst : cl.dst with
      | none =>
        rw [hnil]
        exact ⟨hR, fun _ _ => rfl⟩
      | some d =>
        cases hp : fr.pend with
        | none => trivial
        | some v =>
          obtain ⟨PB', pu', pr', p, a, u, hPB', hph', _⟩ := hE.2 hpos v hp
          rw [hPB] at hPB'; cases hPB'; rw [hnil] at hph'; cases hph'
    · cases hdst : cl.dst with
      | none =>
        have hpr := hdn hdst
        have hag : ∀ j, j < (I.ctx P.vars).hi → σ.setAll (phiUpd σ prevP PB.phis) j = σ j := by
          intro j hj
          apply setAll_phiUpd_other
          rw [hph]; intro q hq
          simp only [List.mem_cons, List.not_mem_nil, or_false] at hq
          rcases hq with rfl | rfl <;> omega
        refine ⟨RelX.agree hctx hR (Nat.le_refl _) hag, fun j hj => hag j ?_⟩
        simp only [IInfo.ctx] at hj ⊢; omega
      | some d =>
        cases hp : fr.pend with
        | none => trivial
        | some v =>
          obtain ⟨PB', pu', pr', p, a, u, hPB', hph', hprev, hpa, hav, hpu'⟩ := hE.2 hpos v hp
          rw [hPB] at hPB'; cases hPB'
          rw [hph] at hph'
          simp only [List.cons.injEq, and_true] at hph'
          obtain ⟨rfl, rfl⟩ := hph'
          obtain ⟨hdx, hsh⟩ := hd d hdst
          obtain ⟨hl, hlo, _, _, hu⟩ := dstX_ok hdx
          subst hprev
          rw [hph]
          simp only [phiUpd, Option.bind, hpu', hpa, Store.setAll, hav]
          refine ⟨RelX.set (RelX.agree hctx hR (Nat.le_refl _)
            (fun j hj => Store.set_other _ _ (by omega))) hl hu hsh v, ?_⟩
          intro j hj
          simp only [IInfo.ctx] at hj hlo hpu ⊢
          rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]

/-! ## One step -/

/-- One PIR block visit, continued by `Kp`. -/
def pVisit (P : PFunc) (ω : Nat → Nat) (t : World) (prev : Option Nat) (cur : Nat) (σ : Store)
    (Kp : World → Option Nat → Nat → Store → POut) : POut :=
  match P.blocks[cur]? with
  | none => .stop
  | some B =>
    (xStmts P ω (σ.setAll (phiUpd σ prev B.phis)) t B.stmts).run fun σ' t' =>
      pTerm σ' (fun j σ'' => Kp t' (some cur) j σ'') B.term

theorem xpRun_succ (P : PFunc) (ω : Nat → Nat) (n : Nat) (t : World) (prev : Option Nat) (cur : Nat) (σ : Store) :
    xpRun P ω (n + 1) t prev cur σ = pVisit P ω t prev cur σ (xpRun P ω n) := rfl

theorem sRunX_succ (M : XMod) (ω : Nat → Nat) (n : Nat) (t : World) (st : List Frame) :
    sRunX M ω (n + 1) t st = (sStep M ω t st).out (fun st' t' => sRunX M ω n t' st') := by
  simp only [sRunX]; cases sStep M ω t st <;> rfl

theorem trArgsX_length {c : Ctx} : ∀ {k : Nat} {args3 : List (Opnd × Nat × Nat)} {s : List PStmt}
    {tws : List Nat} {As : List Arg}, trArgsX c k args3 = .ok (s, tws, As) → As.length = args3.length
  | _, [], _, _, _, h => by simp only [trArgsX, Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨_, _, rfl⟩ := h; rfl
  | _, _ :: _, _, _, _, h => by
    simp only [trArgsX] at h
    obtain ⟨⟨s1, t1, A⟩, _, h⟩ := Except.bind_ok h
    obtain ⟨⟨s2, t2, As'⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨_, _, rfl⟩ := h
    simp [trArgsX_length h2]

theorem step_sim {M : XMod} {F : XFunc} {P : PFunc} {C : List IInfo} {ω : Nat → Nat}
    (hVF : ValidFacts M F P C) {st : List Frame} {prevP : Option Nat} {curP : Nat} {σ : Store}
    (hinv : InvX P C st prevP curP σ) (t : World)
    (K : List Frame → World → Out) (Kp : World → Option Nat → Nat → Store → POut)
    (hK : ∀ st' t' prevP' curP' σ', InvX P C st' prevP' curP' σ' → OSim (K st' t') (Kp t' prevP' curP' σ')) :
    OSim ((sStep M ω t st).out K) (pVisit P ω t prevP curP σ Kp) := by
  obtain ⟨fr, rest, ι, I, rfl, hI, hfF, hR, hcur, hE, hT⟩ := hinv
  have hIF := hVF.inst ι I hI
  have hctx := hIF.ctx
  obtain ⟨bl, hbl, hs'⟩ : ∃ bl, I.blks[fr.cur]? = some bl ∧ bl[fr.seg]? = some curP := by
    cases h : I.blks[fr.cur]? with
    | none => rw [h] at hcur; simp at hcur
    | some bl => rw [h] at hcur; exact ⟨bl, rfl, hcur⟩
  obtain ⟨B, hB, hlen, hsle, hseg⟩ := hIF.seg fr.cur bl fr.seg curP hbl hs'
  obtain ⟨k, PB, s1, t1, _, hPB, hhk, hins, hT1, hph0, hphc, hcall, hterm⟩ := segOK_spec hseg hB hcur
  have hBf : fr.F.blocks[fr.cur]? = some B := by rw [hfF]; exact hB
  have hen := enter_sim hIF hfF hPB hR hE hph0 hphc
  simp only [sStep, hBf, pVisit, hPB, Res.step_out]
  cases hse : sEnter fr B with
  | ub => rw [hse] at hen; exact hen.elim
  | stuck => trivial
  | ok R0 =>
    rw [hse] at hen
    obtain ⟨hR0, ha0⟩ := hen
    simp only [Res.out, Res.step_out]
    have hTa : ∀ σ', (∀ j, j < (I.ctx P.vars).lo → σ' j = σ j) → TailX P C ι rest σ' I.lo :=
      fun σ' ha => TailX.agree hVF.inst hT ha
    cases hsg : B.segs[fr.seg]? with
    | some isc =>
      -- the segment ends in a call
      obtain ⟨is, cl⟩ := isc
      have hBi : B.insts fr.seg = is := by simp [XBlock.insts, hsg]
      obtain ⟨κ, J, s2, t2, As, h0, rest0, hκ, hJ, hfind, hlen', _, _, hargs, _, hst, hb0, hterm', hpar,
        _, hhi, hret⟩ := callOK_spec (hcall is cl hsg)
      rw [hst, xStmts_append, XPRes.then_run, hterm', hBi]
      rw [hBi] at hins
      refine SimX.out (sinstsX_sim hctx is R0 _ k t s1 t1 hR0 ha0 hhk hins hT1) ?_
      intro R t' σ' hR' ha'
      simp only [sEnd, hsg, Res.step_out]
      rw [← zip_map_back cl.args J.fn.params hlen']
      refine argsX_osim hR' t' _ _ (Nat.le_trans hhk (Nat.le_add_right k t1.length)) hargs ?_
      intro vs hvs hAV
      have hvl : vs.length = J.fn.params.length := by
        rw [sArgs_length hvs, zip_map_back cl.args J.fn.params hlen', hlen']
      simp only [hfind, hvl, ite_true, Step.out, pTerm]
      apply hK
      -- the callee's frame
      have hsl : fr.seg < B.segs.length := (List.getElem?_eq_some_iff.mp hsg).1
      obtain ⟨cb, hcb⟩ : ∃ cb, bl[fr.seg + 1]? = some cb :=
        ⟨bl[fr.seg + 1], List.getElem?_eq_getElem (by omega)⟩
      refine ⟨_, _, κ, J, rfl, hJ, rfl, bindArgs_rel J.fn.params As vs hpar hAV, by simp [hb0],
        ⟨fun _ p hp => by simp at hp, fun h => by simp at h⟩, ?_⟩
      refine ⟨ι, I, hI, hfF, by simp, by simpa using hκ, rfl, ⟨cb, by simp [hbl, hcb], ?_⟩, hR', hhi,
        hTa σ' ha'⟩
      intro J' hJ'
      rw [hJ] at hJ'; cases hJ'
      rw [hret, hbl]; simp [hcb]
    | none =>
      -- the last segment: the terminator
      have hBi : B.insts fr.seg = B.last := by simp [XBlock.insts, hsg]
      have hseq : fr.seg = B.segs.length := by
        have := List.getElem?_eq_none_iff.mp hsg; omega
      have hcurT : I.tails[fr.cur]? = some curP := tails_of hbl hlen (by rw [← hseq]; exact hs')
      obtain ⟨s2, t2, T, A?, htr, hT2, hst, hterm', hretA⟩ := termOK_spec (hterm hsg)
      rw [hst, xStmts_append, XPRes.then_run, hterm', hBi]
      rw [hBi] at hins
      refine SimX.out (sinstsX_sim hctx B.last R0 _ k t s1 t1 hR0 ha0 hhk hins hT1) ?_
      intro R t' σ' hR' ha'
      simp only [sEnd, hsg]
      -- a jump within the instance
      have hgo : ∀ tn j', targetX I.fn I.heads tn = .ok j' →
          OSim ((match lookupBlock fr.F.shape tn with
            | some j => Step.next ({ fr with prev := some fr.cur, cur := j, seg := 0, R := R, pend := none } :: rest) t'
            | none => Step.stuck).out K) (Kp t' (some curP) j' σ') := by
        intro tn j' hj
        obtain ⟨jb, hlk, hjb⟩ := targetX_spec hIF hj
        rw [hfF, hlk]
        apply hK
        refine ⟨_, rest, ι, I, rfl, hI, rfl, hR', hjb, ⟨fun _ p hp => ?_, fun h => by simp at h⟩, hTa σ' ha'⟩
        simp only [Option.some.injEq] at hp; subst hp
        exact ⟨curP, hcurT, rfl⟩
      cases htm : B.term with
      | br tn =>
        rw [htm] at htr
        simp only [trTermX] at htr
        obtain ⟨j', hj', htr⟩ := Except.bind_ok htr
        simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at htr
        obtain ⟨rfl, rfl, rfl, rfl⟩ := htr
        simp only [xStmts, XPRes.run, pTerm]
        exact hgo tn j' hj'
      | cbr cnd tn fn =>
        rw [htm] at htr
        simp only [trTermX] at htr
        obtain ⟨⟨sc, tc, Cc⟩, hcn, htr⟩ := Except.bind_ok htr
        obtain ⟨tj, htj, htr⟩ := Except.bind_ok htr
        obtain ⟨fj, hfj, htr⟩ := Except.bind_ok htr
        simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at htr
        obtain ⟨rfl, rfl, rfl, rfl⟩ := htr
        simp only [Res.step_out]
        rw [← List.append_nil sc]
        refine opndX_osim hR' hcn (Nat.le_trans hhk (Nat.le_add_right k t1.length)) t' [] _ _ ?_
        intro v _ _ hCv _
        simp only [xStmts, XPRes.run, pTerm, hCv]
        cases truthN v
        · simp only [Bool.false_eq_true, ite_false]; exact hgo fn fj hfj
        · simp only [ite_true]; exact hgo tn tj htj
      | ret o =>
        rw [htm] at htr
        cases o with
        | none =>
          simp only [trTermX] at htr
          obtain ⟨_, _, htr⟩ := Except.bind_ok htr
          simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at htr
          obtain ⟨rfl, rfl, rfl, rfl⟩ := htr
          simp only [xStmts, XPRes.run]
          cases rest with
          | nil =>
            simp only [TailX] at hT; subst hT
            obtain ⟨I0, hC0, _, hr0, _⟩ := hVF.top
            rw [hC0] at hI; cases hI
            simp only [hr0, pTerm, retTo, Step.out]
            rfl
          | cons c cs =>
            obtain ⟨ι', I', hI', hcF, hcs, _, _, ⟨cb, hcb, hret⟩, hcR, hhi', hT'⟩ := hT
            simp only [hret I hI, pTerm, retTo, Step.out]
            apply hK
            refine ⟨_, cs, ι', I', rfl, hI', hcF, ?_, hcb, ⟨fun h => by simp at h; omega, fun _ v hv => by simp at hv⟩,
              TailX.agree hVF.inst hT' (fun j hj => ha' j (by simp [IInfo.ctx]; have := (hVF.inst _ _ hI').lohi; omega))⟩
            exact RelX.agree (hVF.inst _ _ hI').ctx hcR hhi' (fun j hj => ha' j (by simpa [IInfo.ctx] using hj))
        | some o =>
          simp only [trTermX] at htr
          obtain ⟨_, _, htr⟩ := Except.bind_ok htr
          obtain ⟨⟨so, tw, A⟩, ho, htr⟩ := Except.bind_ok htr
          obtain ⟨⟨se, te⟩, hse, htr⟩ := Except.bind_ok htr
          simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at htr
          obtain ⟨rfl, rfl, rfl, rfl⟩ := htr
          simp only [Res.step_out, hfF]
          have hk2 : I.hi ≤ k + t1.length := Nat.le_trans hhk (Nat.le_add_right k t1.length)
          refine opndX_osim hR' ho hk2 t' se _ _ ?_
          intro v _ htw hAv hAb
          subst htw
          cases rest with
          | nil =>
            simp only [TailX] at hT; subst hT
            obtain ⟨I0, hC0, _, hr0, _⟩ := hVF.top
            rw [hC0] at hI; cases hI
            simp only [escStmts, hr0, Option.isNone_none, Bool.true_and, List.isEmpty_nil,
              List.length_nil, Nat.add_zero] at hse ⊢
            cases hp : I.fn.retPtr with
            | false =>
              rw [hp] at hse
              simp only [Bool.false_eq_true, ite_false, Except.ok.injEq, Prod.mk.injEq] at hse
              obtain ⟨rfl, rfl⟩ := hse
              simp only [xStmts, XPRes.run, hr0, pTerm, retTo, Step.out, hAv]
              rfl
            | true =>
              rw [hp] at hse
              simp only [ite_true] at hse
              obtain ⟨As, hAs⟩ : ∃ As, escArgsX (I.ctx P.vars) I.fn.escNames = .ok As := by
                cases h : escArgsX (I.ctx P.vars) I.fn.escNames with
                | error e => rw [h] at hse; cases hse
                | ok As => exact ⟨As, rfl⟩
              rw [hAs] at hse
              simp only [Except.map, Except.ok.injEq] at hse
              have hAsb : ∀ a ∈ As, a.below (k + t1.length) := by
                intro a ha
                exact Arg.below_mono hk2 (escArgsX_below hAs a ha)
              have hTe : TempsOK P (k + t1.length) (escChk (k + t1.length) A As).2 := by
                rw [hse]; simpa using hT2
              have hrun := escChk_run P ω σ' t' (k + t1.length) A As hAsb hTe
              rw [hse] at hrun
              rcases escHit_ok hR' v I.fn.escNames As hAs with hst | hok
              · rw [hst]; trivial
              · rw [hok]
                simp only [Res.out]
                rw [hAv] at hrun
                cases hb : (As.any (fun a => ptrObj (v % 2 ^ 64) == ptrObj (a.get σ' % 2 ^ 64)) &&
                    ptrObj (v % 2 ^ 64) != 0) with
                | true =>
                  simp only [Res.step, hb, ite_true, Step.out, XPRes.run, hrun.1 hb, OSim]
                | false =>
                  obtain ⟨σ'', e, hag⟩ := hrun.2 hb
                  simp only [Res.step, ite_true, hb, Bool.false_eq_true, ite_false, e, XPRes.run, hr0, pTerm, retTo,
                    Step.out]
                  rw [Arg.get_agree hag hAb, hAv]; rfl
          | cons c cs =>
            obtain ⟨ι', I', hI', hcF, hcs, _, _, ⟨cb, hcb, hret⟩, hcR, hhi', hT'⟩ := hT
            simp only [escStmts, hret I hI, Option.isNone_some, Bool.false_and, Bool.false_eq_true, ite_false,
              Except.ok.injEq, Prod.mk.injEq] at hse
            obtain ⟨rfl, rfl⟩ := hse
            obtain ⟨CB, pu, pr, u, hCB, hph, hpa, hpu⟩ := hretA cb A (hret I hI) rfl
            simp only [List.isEmpty_cons, Bool.false_and, Bool.false_eq_true, ite_false, xStmts, XPRes.run,
              hret I hI, pTerm, retTo, Step.out]
            apply hK
            refine ⟨_, cs, ι', I', rfl, hI', hcF, ?_, hcb,
              ⟨fun h => by simp at h; omega, fun _ v' hv' => ?_⟩,
              TailX.agree hVF.inst hT' (fun j hj => ha' j (by simp [IInfo.ctx]; have := (hVF.inst _ _ hI').lohi; omega))⟩
            · exact RelX.agree (hVF.inst _ _ hI').ctx hcR hhi' (fun j hj => ha' j (by simpa [IInfo.ctx] using hj))
            · simp only [Option.some.injEq] at hv'; subst hv'
              exact ⟨CB, pu, pr, curP, A, u, hCB, hph, rfl, hpa, hAv, hpu⟩
      | unreachable =>
        rw [htm] at htr
        simp only [trTermX, Except.ok.injEq, Prod.mk.injEq] at htr
        obtain ⟨rfl, rfl, rfl, rfl⟩ := htr
        rfl

/-! ## Runs -/

theorem run_simX {M : XMod} {F : XFunc} {P : PFunc} {C : List IInfo} {ω : Nat → Nat}
    (hVF : ValidFacts M F P C) :
    ∀ (n : Nat) (t : World) (st : List Frame) (prevP : Option Nat) (curP : Nat) (σ : Store),
    InvX P C st prevP curP σ → OSim (sRunX M ω n t st) (xpRun P ω n t prevP curP σ)
  | 0, _, _, _, _, _, _ => rfl
  | n + 1, t, st, prevP, curP, σ, hinv => by
    rw [sRunX_succ, xpRun_succ]
    exact step_sim hVF hinv t _ _ (fun st' t' prevP' curP' σ' hi => run_simX hVF n t' st' prevP' curP' σ' hi)

/-- **The extended translation is exact with respect to the strict
semantics**, for every certificate that passes `validB`: for every input,
every oracle and every fuel, PIR returns `v` iff LLVM returns `v`, PIR fails
a check iff LLVM reaches UB (strict), PIR runs out of fuel iff LLVM does —
whenever the LLVM run is not stuck. -/
theorem translateX_exact {M : XMod} {F : XFunc} {P : PFunc} {C : List IInfo}
    (hV : validB M F P C = true) (args : List Nat) (ω : Nat → Nat) (n : Nat) :
    OSim (sRunXF M F args ω n) (xpRunF P args ω n) := by
  have hVF := validB_facts hV
  obtain ⟨I0, hC0, hF0, _, ⟨r0, hb0⟩, htp⟩ := hVF.top
  apply run_simX hVF
  refine ⟨initFrame F args, [], 0, I0, rfl, hC0, hF0.symm, ?_, by simp [initFrame, hb0],
    ⟨fun _ p hp => by simp [initFrame] at hp, fun h => by simp [initFrame] at h⟩, rfl⟩
  -- the parameters
  intro m x hm
  simp only [initFrame] at hm
  rw [initRegs_eq] at hm
  cases hf : findName F.params m with
  | none => simp [hf] at hm
  | some p =>
    obtain ⟨j, w⟩ := p
    simp only [hf, Option.map_some, Option.some.injEq] at hm
    subst hm
    have hg := findName_get hf
    have hjl := findName_lt hf
    unfold topParamsOK at htp
    rw [List.all_eq_true] at htp
    have hmem : ((m, w), j) ∈ F.params.zipIdx := by
      rw [List.mem_iff_getElem?]
      exact ⟨j, by simp [List.getElem?_zipIdx, hg]⟩
    have := htp _ hmem
    simp only [Bool.and_eq_true, beq_iff_eq, Option.isNone_iff_eq_none] at this
    obtain ⟨⟨hl, hsh⟩, hw⟩ := this
    refine ⟨.v j w, hl, ?_, fun s hs => ?_⟩
    · simp only [Arg.get, pInit, hVF.params]
      rw [pInitAux_eq]
      simp only [Nat.zero_le, true_and, Nat.zero_add, hjl, ite_true, Nat.sub_zero, hw]
    · have := Ctx.shOf_some hs
      simp only [IInfo.ctx] at this
      rw [hsh] at this; cases this

end PrismRefine
