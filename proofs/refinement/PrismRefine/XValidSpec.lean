/-
PRISM refinement, extended fragment — what `validB` (`XValid.lean`)
guarantees, as propositions the simulation (`XStep.lean`) uses.
-/
import PrismRefine.XRefine

namespace PrismRefine

open PrismSem

theorem tempsOKB_ok {P : PFunc} {k : Nat} {tws : List Nat} (h : tempsOKB P k tws = true) :
    TempsOK P k tws := by
  intro j hj
  unfold tempsOKB at h
  rw [List.all_eq_true] at h
  have := h j (List.mem_range.mpr hj)
  simp only [beq_iff_eq] at this
  rw [this, List.getD_eq_getElem?_getD, List.getElem?_eq_getElem hj]; rfl

theorem allLtB_look {m : Nat} {l : List (String × Arg)} (h : allLtB m l = true) {n : String} {a : Arg}
    (hl : look l n = some a) : a.below m := by
  unfold allLtB at h
  rw [List.all_eq_true] at h
  exact Arg.lt_below (h _ (look_mem hl))

theorem nodupNat_inj : ∀ {l : List Nat} {i j x : Nat}, nodupNat l = true → l[i]? = some x → l[j]? = some x → i = j
  | [], _, _, _, _, h, _ => by simp at h
  | y :: ys, i, j, x, hn, hi, hj => by
    simp only [nodupNat, Bool.and_eq_true, Bool.not_eq_true'] at hn
    obtain ⟨hy0, hn⟩ := hn
    have hy : y ∉ ys := by simpa using hy0
    cases i with
    | zero =>
      cases j with
      | zero => rfl
      | succ j =>
        simp at hi hj; subst hi
        exact absurd (List.mem_of_getElem? hj) (by simpa using hy)
    | succ i =>
      cases j with
      | zero =>
        simp at hi hj; subst hj
        exact absurd (List.mem_of_getElem? hi) (by simpa using hy)
      | succ j =>
        simp at hi hj
        exact congrArg (· + 1) (nodupNat_inj hn hi hj)

/-- Facts about one instance. -/
structure InstFacts (M : XMod) (P : PFunc) (C : List IInfo) (I : IInfo) : Prop where
  ctx : CtxOK P (I.ctx P.vars)
  lohi : I.lo ≤ I.hi
  tails : nodupNat I.tails = true
  blks : I.blks.length = I.fn.blocks.length
  seg : ∀ (b : Nat) (bl : List Nat) (s pb : Nat), I.blks[b]? = some bl → bl[s]? = some pb →
    ∃ B, I.fn.blocks[b]? = some B ∧ bl.length = B.segs.length + 1 ∧ s ≤ B.segs.length ∧
      segOK M P C I b s = true
  blen : ∀ (b : Nat) (B : XBlock), I.fn.blocks[b]? = some B → ∃ bl, I.blks[b]? = some bl ∧ bl.length = B.segs.length + 1

theorem instOK_facts {M : XMod} {P : PFunc} {C : List IInfo} {I : IInfo} (h : instOK M P C I = true) :
    InstFacts M P C I := by
  unfold instOK at h
  simp only [Bool.and_eq_true, decide_eq_true_eq, beq_iff_eq, List.all_eq_true, List.mem_range] at h
  obtain ⟨⟨⟨⟨⟨⟨⟨hlo, henv⟩, hsh⟩, hbl⟩, hks⟩, hkids⟩, htl⟩, hall⟩ := h
  refine ⟨⟨rfl, fun n a hl => allLtB_look henv hl, fun n s hl => allLtB_look hsh hl⟩, hlo, htl, hbl, ?_, ?_⟩
  · intro b bl s pb hb hs
    have hbb : b < I.fn.blocks.length := by
      rw [← hbl]; exact (List.getElem?_eq_some_iff.mp hb).1
    have := hall b hbb
    obtain ⟨B, hB⟩ : ∃ B, I.fn.blocks[b]? = some B := ⟨_, List.getElem?_eq_getElem hbb⟩
    rw [hB] at this
    simp only [Bool.and_eq_true, beq_iff_eq, List.all_eq_true, List.mem_range] at this
    obtain ⟨⟨⟨h1, _⟩, _⟩, h4⟩ := this
    rw [List.getD_eq_getElem?_getD, hb, Option.getD_some] at h1
    have hsl : s < bl.length := (List.getElem?_eq_some_iff.mp hs).1
    exact ⟨B, hB, h1, by omega, h4 s (by omega)⟩
  · intro b B hB
    have hbb : b < I.fn.blocks.length := (List.getElem?_eq_some_iff.mp hB).1
    have := hall b hbb
    rw [hB] at this
    simp only [Bool.and_eq_true, beq_iff_eq, List.all_eq_true, List.mem_range] at this
    obtain ⟨⟨⟨h1, _⟩, _⟩, _⟩ := this
    have hbb' : b < I.blks.length := by rw [hbl]; exact hbb
    refine ⟨I.blks[b], List.getElem?_eq_getElem hbb', ?_⟩
    rw [List.getD_eq_getElem?_getD, List.getElem?_eq_getElem hbb', Option.getD_some] at h1
    exact h1

/-- Facts about the whole certificate. -/
structure ValidFacts (M : XMod) (F : XFunc) (P : PFunc) (C : List IInfo) : Prop where
  inst : ∀ (ι : Nat) (I : IInfo), C[ι]? = some I → InstFacts M P C I
  top : ∃ I0, C[0]? = some I0 ∧ I0.fn = F ∧ I0.retTo = none ∧
    (∃ rest, I0.blks[0]? = some (0 :: rest)) ∧ topParamsOK P I0 F.params = true
  params : P.params = List.range' 0 F.params.length
  retw : P.retw = F.retw

theorem validB_facts {M : XMod} {F : XFunc} {P : PFunc} {C : List IInfo} (h : validB M F P C = true) :
    ValidFacts M F P C := by
  unfold validB at h
  split at h
  · rename_i I0 h0
    simp only [Bool.and_eq_true, decide_eq_true_eq, beq_iff_eq, Option.isNone_iff_eq_none,
      List.all_eq_true] at h
    obtain ⟨⟨⟨⟨⟨⟨hf, hr⟩, hp⟩, hw⟩, hb⟩, htp⟩, hall⟩ := h
    refine ⟨fun ι I hI => instOK_facts (hall I (List.mem_of_getElem? hI)), ⟨I0, h0, hf, hr, ?_, htp⟩, hp, hw⟩
    split at hb
    · rename_i h' r hbl
      simp only [beq_iff_eq] at hb; subst hb
      exact ⟨r, hbl⟩
    · simp at hb
  · simp at h

/-! ### Segments -/

theorem segOK_spec {M : XMod} {P : PFunc} {C : List IInfo} {I : IInfo} {b s : Nat}
    (h : segOK M P C I b s = true) {B : XBlock} (hB : I.fn.blocks[b]? = some B) {pb : Nat}
    (hpb : (I.blks[b]?).bind (·[s]?) = some pb) :
    ∃ k PB s1 t1, (I.ks[b]?).bind (·[s]?) = some k ∧ P.blocks[pb]? = some PB ∧ I.hi ≤ k ∧
      trSInstsX (I.ctx P.vars) k (B.insts s) = .ok (s1, t1) ∧ TempsOK P k t1 ∧
      (s = 0 → trPhisX I.fn (I.ctx P.vars) I.tails B.phis = .ok PB.phis) ∧
      (0 < s → ∃ is cl, B.segs[s - 1]? = some (is, cl) ∧ contOK (I.ctx P.vars) cl PB.phis = true) ∧
      (∀ is cl, B.segs[s]? = some (is, cl) → callOK M P C I b s (I.ctx P.vars) k s1 t1 PB cl = true) ∧
      (B.segs[s]? = none → termOK P I (I.ctx P.vars) k s1 t1 pb PB B.term = true) := by
  unfold segOK at h
  rw [hB, hpb] at h
  split at h
  · rename_i B' pb' k hB' hpb' hk
    simp only [Option.some.injEq] at hB' hpb'
    subst hB' hpb'
    split at h
    · simp at h
    · rename_i PB hPB
      simp only [Bool.and_eq_true, decide_eq_true_eq] at h
      obtain ⟨hhk, h⟩ := h
      split at h
      · simp at h
      · rename_i s1 t1 hins
        simp only [Bool.and_eq_true] at h
        obtain ⟨⟨hT, hph⟩, hend⟩ := h
        refine ⟨k, PB, s1, t1, hk, hPB, hhk, hins, tempsOKB_ok hT, ?_, ?_, ?_, ?_⟩
        · intro hs0; subst hs0
          simp only [ite_true] at hph
          split at hph
          · rename_i qs hq; simp only [beq_iff_eq] at hph; rw [hq, hph]
          · simp at hph
        · intro hs0
          have hne : s ≠ 0 := by omega
          simp only [hne, ite_false] at hph
          split at hph
          · rename_i is cl hsg; exact ⟨is, cl, hsg, hph⟩
          · simp at hph
        · intro is cl hsg
          rw [hsg] at hend; exact hend
        · intro hsg
          rw [hsg] at hend; exact hend
  · simp at h

theorem contOK_spec {c : Ctx} {cl : CallI} {phis : List PPhi} (h : contOK c cl phis = true) :
    phis = [] ∨ ∃ pu pr, phis = [pu, pr] ∧ c.hi ≤ pu.dst ∧
      (∀ d, cl.dst = some d → dstX c d cl.rw = .ok pr.dst ∧ look c.sh d = none) ∧
      (cl.dst = none → c.hi ≤ pr.dst) := by
  unfold contOK at h
  split at h
  · exact .inl rfl
  · rename_i pu pr
    simp only [Bool.and_eq_true, decide_eq_true_eq] at h
    obtain ⟨hu, h⟩ := h
    refine .inr ⟨pu, pr, rfl, hu, ?_, ?_⟩
    · intro d hd
      rw [hd] at h
      simp only [Bool.and_eq_true, Option.isNone_iff_eq_none] at h
      obtain ⟨h1, h2⟩ := h
      split at h1
      · rename_i i hi; simp only [beq_iff_eq] at h1; subst h1; exact ⟨hi, h2⟩
      · simp at h1
    · intro hd; rw [hd] at h; simpa using h
  · simp at h

theorem callOK_spec {M : XMod} {P : PFunc} {C : List IInfo} {I : IInfo} {b s : Nat} {c : Ctx} {k : Nat}
    {s1 : List PStmt} {t1 : List Nat} {PB : PBlock} {cl : CallI}
    (h : callOK M P C I b s c k s1 t1 PB cl = true) :
    ∃ κ J s2 t2 As h0 rest0, (I.kids[b]?).bind (·[s]?) = some κ ∧ C[κ]? = some J ∧
      M.find cl.f = some J.fn ∧ cl.args.length = J.fn.params.length ∧
      (J.fn.retw = 0 ↔ cl.rw = 0) ∧ (J.fn.retw ≠ 0 ∨ cl.dst = none) ∧
      trArgsX c (k + t1.length) ((cl.args.zip J.fn.params).map fun ((o, w), (_, wp)) => (o, w, wp)) =
        .ok (s2, t2, As) ∧
      TempsOK P (k + t1.length) t2 ∧ PB.stmts = s1 ++ s2 ∧
      J.blks[0]? = some (h0 :: rest0) ∧ PB.term = .jmp h0 ∧ paramsOK J J.fn.params As = true ∧
      (∀ a ∈ As, a.below J.lo) ∧ I.hi ≤ J.lo ∧ J.retTo = (I.blks[b]?).bind (·[s + 1]?) := by
  unfold callOK at h
  split at h
  · simp at h
  · rename_i κ hκ
    split at h
    · simp at h
    · rename_i J hJ
      simp only [Bool.and_eq_true, decide_eq_true_eq, beq_iff_eq, Bool.or_eq_true, bne_iff_ne, ne_eq,
        Option.isNone_iff_eq_none] at h
      obtain ⟨⟨⟨⟨⟨hf, hlen⟩, hrw⟩, _⟩, hdst⟩, h⟩ := h
      split at h
      · simp at h
      · rename_i s2 t2 As hargs
        simp only [Bool.and_eq_true, decide_eq_true_eq, beq_iff_eq, List.all_eq_true] at h
        obtain ⟨⟨⟨⟨⟨⟨hT, hst⟩, hterm⟩, hpar⟩, hlt⟩, hhi⟩, hret⟩ := h
        split at hterm
        · rename_i h0 r0 hb0
          simp only [beq_iff_eq] at hterm
          refine ⟨κ, J, s2, t2, As, h0, r0, hκ, hJ, hf, hlen, ?_, ?_, hargs, tempsOKB_ok hT, hst, hb0, hterm,
            hpar, fun a ha => Arg.lt_below (hlt a ha), hhi, hret⟩
          · constructor
            · intro h0'; rw [h0'] at hrw; simpa using hrw
            · intro h0'; rw [h0'] at hrw; simpa using hrw.symm
          · rcases hdst with h1 | h1
            · exact .inl h1
            · exact .inr h1
        · simp at hterm

theorem termOK_spec {P : PFunc} {I : IInfo} {c : Ctx} {k : Nat} {s1 : List PStmt} {t1 : List Nat}
    {pb : Nat} {PB : PBlock} {t : LTerm} (h : termOK P I c k s1 t1 pb PB t = true) :
    ∃ s2 t2 T A?, trTermX I.fn c I.heads I.retTo (k + t1.length) t = .ok (s2, t2, T, A?) ∧
      TempsOK P (k + t1.length) t2 ∧ PB.stmts = s1 ++ s2 ∧ PB.term = T ∧
      (∀ cb A, I.retTo = some cb → A? = some A → ∃ CB pu pr u, P.blocks[cb]? = some CB ∧
        CB.phis = [pu, pr] ∧ pickInc pb pr.inc = some A ∧ pickInc pb pu.inc = some u) := by
  unfold termOK at h
  split at h
  · simp at h
  · rename_i s2 t2 T A? htr
    simp only [Bool.and_eq_true, beq_iff_eq] at h
    obtain ⟨⟨⟨hT, hst⟩, hterm⟩, hret⟩ := h
    refine ⟨s2, t2, T, A?, htr, tempsOKB_ok hT, hst, hterm, ?_⟩
    intro cb A hcb hA
    rw [hcb, hA] at hret
    simp only at hret
    split at hret
    · rename_i CB hCB
      split at hret
      · rename_i pu pr hph
        simp only [Bool.and_eq_true, beq_iff_eq, Option.isSome_iff_exists] at hret
        obtain ⟨h1, u, h2⟩ := hret
        exact ⟨CB, pu, pr, u, hCB, hph, h1, h2⟩
      · simp at hret
    · simp at hret

end PrismRefine
