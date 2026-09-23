/-
PRISM refinement, extended fragment — block entry: phis (segment 0) and the
continuation phis of a call (later segments).
-/
import PrismRefine.XValidSpec

namespace PrismRefine

open PrismSem

/-! ## Phis of a block's first segment -/

/-- What a phi incoming value translates to (`trIncArgX`). -/
def OpndArgX (c : Ctx) (w : Nat) (o : Opnd) (a : Arg) : Prop :=
  (∃ b, o = .const b ∧ a = .c w (b % 2 ^ w)) ∨ (∃ n, o = .reg n ∧ look c.env n = some a)

theorem trIncX_pick {G : XFunc} {c : Ctx} {tails : List Nat} {w pv q : Nat} (htl : nodupNat tails = true)
    (hq : tails[pv]? = some q) :
    ∀ {inc : List (Opnd × String)} {L : List (Nat × Arg)} {o : Opnd},
    trIncX G c tails w inc = .ok L → phiPick G.shape pv inc = some o →
    ∃ a, pickInc q L = some a ∧ OpndArgX c w o a
  | [], _, _, _, h => by simp [phiPick] at h
  | (o', pr) :: t, L, o, h, hp => by
    simp only [trIncX] at h
    obtain ⟨a', ha', h⟩ := Except.bind_ok h
    obtain ⟨rest, hr, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq] at h
    have hoa : OpndArgX c w o' a' := by
      cases o' with
      | const b => simp only [trIncArgX, Except.ok.injEq] at ha'; exact .inl ⟨b, rfl, ha'.symm⟩
      | reg n =>
        simp only [trIncArgX] at ha'
        split at ha'
        · rename_i a hl
          split at ha'
          · simp only [Except.ok.injEq] at ha'; subst ha'; exact .inr ⟨n, rfl, hl⟩
          · simp at ha'
        · simp at ha'
      | poison => simp [trIncArgX] at ha'
    simp only [phiPick] at hp
    split at hp
    · rename_i hl
      simp only [Option.some.injEq] at hp; subst hp
      rw [hl] at h; simp only [hq] at h; subst h
      exact ⟨a', by simp [pickInc], hoa⟩
    · rename_i hl
      obtain ⟨a, hpa, hoa'⟩ := trIncX_pick htl hq hr hp
      refine ⟨a, ?_, hoa'⟩
      subst h
      split
      · rename_i j hj
        split
        · rename_i q' hq'
          have : q' ≠ q := by
            intro e; subst e
            exact hl (by rw [hj, nodupNat_inj htl hq' hq])
          simp [pickInc, this, hpa]
        · exact hpa
      · exact hpa

theorem trIncShX_pick {G : XFunc} {c : Ctx} {tails : List Nat} {pv q : Nat} (htl : nodupNat tails = true)
    (hq : tails[pv]? = some q) :
    ∀ {inc : List (Opnd × String)} {o : Opnd}, phiPick G.shape pv inc = some o →
    pickInc q (trIncShX G c tails inc) = some (incShadowX c o)
  | [], _, h => by simp [phiPick] at h
  | (o', pr) :: t, o, hp => by
    simp only [phiPick] at hp
    simp only [trIncShX]
    split at hp
    · rename_i hl
      simp only [Option.some.injEq] at hp; subst hp
      rw [hl]; simp [hq, pickInc]
    · rename_i hl
      split
      · rename_i j hj
        split
        · rename_i q' hq'
          have : q' ≠ q := by
            intro e; subst e
            exact hl (by rw [hj, nodupNat_inj htl hq' hq])
          simp [pickInc, this, trIncShX_pick htl hq hp]
        · exact trIncShX_pick htl hq hp
      · exact trIncShX_pick htl hq hp

/-- The updates of the LLVM phis and of the PIR phis (value phi, then the
shadow phi of a register that may be uninitialised) correspond. -/
inductive UpdX (c : Ctx) : List (String × SV) → List (Nat × Nat) → Prop
  | nil : UpdX c [] []
  | plain {n : String} {i w v : Nat} {t : List (String × SV)} {L : List (Nat × Nat)} :
      look c.env n = some (.v i w) → c.uniq n i = true → look c.sh n = none → c.lo ≤ i →
      UpdX c t L → UpdX c ((n, .val v) :: t) ((i, v) :: L)
  | shVal {n : String} {i w s w' v b : Nat} {t : List (String × SV)} {L : List (Nat × Nat)} :
      look c.env n = some (.v i w) → c.uniq n i = true → look c.sh n = some (.v s w') →
      c.uniqS n s = true → c.lo ≤ i → c.lo ≤ s → truthN b = false →
      UpdX c t L → UpdX c ((n, .val v) :: t) ((i, v) :: (s, b) :: L)
  | shInd {n : String} {i w s w' u b : Nat} {t : List (String × SV)} {L : List (Nat × Nat)} :
      look c.env n = some (.v i w) → c.uniq n i = true → look c.sh n = some (.v s w') →
      c.uniqS n s = true → c.lo ≤ i → c.lo ≤ s → truthN b = true →
      UpdX c t L → UpdX c ((n, .ind) :: t) ((i, u) :: (s, b) :: L)

theorem uniq_ne_sh {c : Ctx} {n : String} {i s w' : Nat} (hu : c.uniq n i = true)
    (hs : look c.sh n = some (.v s w')) : s ≠ i := by
  have := Ctx.uniq_sh hu hs
  intro e; subst e; exact this rfl

theorem RelX.setAllX {c : Ctx} : ∀ {upd : List (String × SV)} {L : List (Nat × Nat)} {R : SRegs} {σ : Store},
    UpdX c upd L → RelX c R σ →
    RelX c (R.setAll upd) (σ.setAll L) ∧ ∀ j, j < c.lo → σ.setAll L j = σ j
  | _, _, _, _, .nil, hR => ⟨hR, fun _ _ => rfl⟩
  | _, _, R, σ, @UpdX.plain _ n i w v _ _ hl hu hsh hlo ht, hR => by
    simp only [SRegs.setAll, Store.setAll]
    have h1 := RelX.setAllX ht (RelX.set hR hl hu hsh v)
    exact ⟨h1.1, fun j hj => by rw [h1.2 j hj]; exact Store.set_other _ _ (by omega)⟩
  | _, _, R, σ, @UpdX.shVal _ n i w s w' v b _ _ hl hu hs hus hlo hslo hb ht, hR => by
    have hsi := uniq_ne_sh hu hs
    simp only [SRegs.setAll, Store.setAll]
    have hput : RelX c (R.put n (.val v)) ((σ.set i v).set s b) := by
      refine RelX.put hR hl hu (fun s'' w'' h'' => by rw [hs] at h''; cases h''; exact hus) ?_ ?_
      · refine ⟨by simp [Arg.get, Store.set, Ne.symm hsi], fun a ha => ?_⟩
        rw [Ctx.shOf_var hl, hs] at ha; cases ha; simpa [Arg.get] using hb
      · intro j hj
        have h1 : j ≠ i := fun e => hj (.inl e)
        have h2 : j ≠ s := fun e => hj (.inr ⟨w', by rw [hs, e]⟩)
        rw [Store.set_other _ _ h2, Store.set_other _ _ h1]
    have h1 := RelX.setAllX ht hput
    refine ⟨h1.1, fun j hj => ?_⟩
    rw [h1.2 j hj, Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]
  | _, _, R, σ, @UpdX.shInd _ n i w s w' u b _ _ hl hu hs hus hlo hslo hb ht, hR => by
    simp only [SRegs.setAll, Store.setAll]
    have hput : RelX c (R.put n .ind) ((σ.set i u).set s b) := by
      refine RelX.put hR hl hu (fun s'' w'' h'' => by rw [hs] at h''; cases h''; exact hus) ?_ ?_
      · refine ⟨.v s w', by rw [Ctx.shOf_var hl, hs], by simpa [Arg.get] using hb⟩
      · intro j hj
        have h1 : j ≠ i := fun e => hj (.inl e)
        have h2 : j ≠ s := fun e => hj (.inr ⟨w', by rw [hs, e]⟩)
        rw [Store.set_other _ _ h2, Store.set_other _ _ h1]
    have h1 := RelX.setAllX ht hput
    refine ⟨h1.1, fun j hj => ?_⟩
    rw [h1.2 j hj, Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]

theorem hasShadowedInputX_false {c : Ctx} {inc : List (Opnd × String)}
    (h : hasShadowedInputX c inc = false) {n pr : String} (hm : (Opnd.reg n, pr) ∈ inc) :
    c.shOf n = none := by
  unfold hasShadowedInputX at h
  rw [List.any_eq_false] at h
  have := h _ hm
  simpa using this

theorem phisX_sim {G : XFunc} {c : Ctx} {tails : List Nat} (htl : nodupNat tails = true)
    {R : SRegs} {σ : Store} (hR : RelX c R σ) (prev prevP : Option Nat)
    (hprev : ∀ p, prev = some p → ∃ q, tails[p]? = some q ∧ prevP = some q) :
    ∀ (ps : List PhiI) (qs : List PPhi), trPhisX G c tails ps = .ok qs →
    (sPhis G.shape R prev ps = .ub → False) ∧
    (∀ upd, sPhis G.shape R prev ps = .ok upd → UpdX c upd (phiUpd σ prevP qs))
  | [], qs, h => by
    simp only [trPhisX, Except.ok.injEq] at h; subst h
    simp [sPhis, phiUpd, UpdX.nil]
  | p :: ps, qs, h => by
    simp only [trPhisX] at h
    obtain ⟨q, hq, h⟩ := Except.bind_ok h
    obtain ⟨qs', hqs, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq] at h; subst h
    have ih := phisX_sim htl hR prev prevP hprev ps qs' hqs
    simp only [trPhiX] at hq
    obtain ⟨u, _, hq⟩ := Except.bind_ok hq
    obtain ⟨i, hi, hq⟩ := Except.bind_ok hq
    obtain ⟨L, hL, hq⟩ := Except.bind_ok hq
    obtain ⟨hl, hlo, _, _, hu⟩ := dstX_ok hi
    cases prev with
    | none => simp [sPhis]
    | some pv =>
      obtain ⟨qp, hqp, rfl⟩ := hprev pv rfl
      cases hpk : phiPick G.shape pv p.inc with
      | none => simp [sPhis, hpk]
      | some o =>
        obtain ⟨a, hpa, hoa⟩ := trIncX_pick htl hqp hL hpk
        obtain ⟨pr, hmem⟩ := phiPick_mem hpk
        rw [phiUpd_append]
        rcases hoa with ⟨b, rfl, rfl⟩ | ⟨n, rfl, hln⟩
        · -- a constant: defined, no shadow
          have hx : sPhiOpnd R p.w (.const b) = .ok (.val (b % 2 ^ p.w)) := rfl
          simp only [sPhis, hpk, hx, Res.bind]
          constructor
          · intro hub
            cases hs : sPhis G.shape R (some pv) ps <;> simp [hs] at hub
            exact ih.1 hs
          · intro upd hupd
            cases hs : sPhis G.shape R (some pv) ps <;> simp [hs] at hupd
            subst hupd
            have ih2 := ih.2 _ hs
            split at hq
            · rename_i sv w' hsv
              obtain ⟨u2, hu2, hq⟩ := Except.bind_ok hq
              have hc2 := need_ok hu2
              simp only [Bool.and_eq_true, decide_eq_true_eq] at hc2
              simp only [pure, Except.pure, Except.ok.injEq] at hq; subst hq
              simp only [phiUpd, Option.bind, hpa, trIncShX_pick (c := c) htl hqp hpk, List.cons_append,
                List.nil_append, incShadowX]
              exact .shVal hl hu hsv hc2.2 hlo hc2.1.1 rfl ih2
            · simp at hq
            · rename_i hnone
              obtain ⟨_, _, hq⟩ := Except.bind_ok hq
              simp only [pure, Except.pure, Except.ok.injEq] at hq; subst hq
              simp only [phiUpd, Option.bind, hpa, List.cons_append, List.nil_append]
              exact .plain hl hu hnone hlo ih2
        · cases hn : R n with
          | none => simp [sPhis, hpk, sPhiOpnd, hn, Res.bind]
          | some x =>
            obtain ⟨a', hl', hx⟩ := hR n x hn
            rw [hln] at hl'; cases hl'
            have hxo : sPhiOpnd R p.w (.reg n) = .ok x := by simp [sPhiOpnd, hn]
            simp only [sPhis, hpk, hxo, Res.bind]
            constructor
            · intro hub
              cases hs : sPhis G.shape R (some pv) ps <;> simp [hs] at hub
              exact ih.1 hs
            · intro upd hupd
              cases hs : sPhis G.shape R (some pv) ps <;> simp [hs] at hupd
              subst hupd
              have ih2 := ih.2 _ hs
              split at hq
              · rename_i sv w'' hsv
                obtain ⟨u2, hu2, hq⟩ := Except.bind_ok hq
                have hc2 := need_ok hu2
                simp only [Bool.and_eq_true, decide_eq_true_eq] at hc2
                simp only [pure, Except.pure, Except.ok.injEq] at hq; subst hq
                simp only [phiUpd, Option.bind, hpa, trIncShX_pick (c := c) htl hqp hpk, List.cons_append,
                  List.nil_append]
                cases x with
                | val v =>
                  obtain ⟨hv, hsh⟩ := hx
                  have hb : truthN ((incShadowX c (.reg n)).get σ) = false := by
                    simp only [incShadowX]
                    cases hs' : c.shOf n with
                    | none => rfl
                    | some a => simpa using hsh a hs'
                  rw [hv]
                  exact .shVal hl hu hsv hc2.2 hlo hc2.1.1 hb ih2
                | ind =>
                  obtain ⟨a, ha, ht⟩ := hx
                  have hb : truthN ((incShadowX c (.reg n)).get σ) = true := by
                    simp only [incShadowX, ha]; simpa using ht
                  exact .shInd hl hu hsv hc2.2 hlo hc2.1.1 hb ih2
              · simp at hq
              · rename_i hnone
                obtain ⟨_, hsi, hq⟩ := Except.bind_ok hq
                simp only [pure, Except.pure, Except.ok.injEq] at hq; subst hq
                simp only [phiUpd, Option.bind, hpa, List.cons_append, List.nil_append]
                cases x with
                | val v =>
                  obtain ⟨hv, _⟩ := hx
                  rw [hv]
                  exact .plain hl hu hnone hlo ih2
                | ind =>
                  exfalso
                  obtain ⟨a, ha, _⟩ := hx
                  have := hasShadowedInputX_false (by simpa using need_ok hsi) hmem
                  rw [this] at ha; cases ha

/-! ## Phi updates only write their destinations -/

theorem setAll_phiUpd_other (σ : Store) (prev : Option Nat) {j : Nat} :
    ∀ (qs : List PPhi) (σ0 : Store), (∀ q ∈ qs, q.dst ≠ j) → σ0.setAll (phiUpd σ prev qs) j = σ0 j
  | [], σ0, _ => rfl
  | q :: qs, σ0, h => by
    simp only [phiUpd]
    split
    · simp only [Store.setAll]
      rw [setAll_phiUpd_other σ prev qs _ (fun q' hq' => h q' (List.mem_cons_of_mem _ hq'))]
      exact Store.set_other _ _ (fun e => h q List.mem_cons_self e.symm)
    · exact setAll_phiUpd_other σ prev qs σ0 (fun q' hq' => h q' (List.mem_cons_of_mem _ hq'))

end PrismRefine
