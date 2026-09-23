/-
PRISM techniques: the bit-blaster's expression encoder, the Tseitin CNF, and
the main theorems (`toCNF_equisat`, `toCNF_unsat_imp`, `certified_unsat`).
See `PrismTechniques/Bitblast.lean` for the circuit layer.
-/
import PrismTechniques.Bitblast

namespace PrismTechniques.Bitblast

open Std.Sat

theorem map_neg_val (α : Nat → Bool) (ls : List Lit) :
    (ls.map Lit.neg).map (Lit.val α) = (ls.map (Lit.val α)).map (!·) := by
  simp [List.map_map, Function.comp_def]

theorem good_map_neg {n : Nat} {ls : List Lit} (h : Good n ls) : Good n (ls.map Lit.neg) := by
  intro l hl
  obtain ⟨l', hl', rfl⟩ := List.mem_map.1 hl
  exact h l' hl'

/-! ### Multi-gate building blocks -/

/-- Full adder: sum `(a ⊕ b) ⊕ c`, carry `(a ∧ b) ∨ ((a ⊕ b) ∧ c)`. -/
def fullAdd (a b c : Lit) (gs : Circuit) : (Lit × Lit) × Circuit :=
  let t := mkGate (.xor a b) gs
  let s := mkGate (.xor t.1 c) t.2
  let u := mkGate (.and a b) s.2
  let v := mkGate (.and t.1 c) u.2
  let co := mkGate (.or u.1 v.1) v.2
  ((s.1, co.1), co.2)

theorem fullAdd_facts (a b c : Lit) (gs : Circuit) (hwf : WF gs) (ha : Scoped gs.length a)
    (hb : Scoped gs.length b) (hc : Scoped gs.length c) :
    let r := fullAdd a b c gs
    (∃ l, r.2 = l ++ gs) ∧ r.2.length = gs.length + 5 ∧ WF r.2 ∧
    Scoped r.2.length r.1.1 ∧ Scoped r.2.length r.1.2 ∧
    ∀ α, Consistent α r.2 →
      r.1.1.val α = ((a.val α ^^ b.val α) ^^ c.val α) ∧
      r.1.2.val α = ((a.val α && b.val α) || ((a.val α ^^ b.val α) && c.val α)) := by
  unfold Scoped at ha hb hc
  refine ⟨⟨[_, _, _, _, _], rfl⟩, rfl, ?_, ?_, ?_, ?_⟩ <;> simp only [fullAdd, mkGate, List.length_cons, outVar]
  · simp only [WF, Gate.Scoped, Scoped, List.length_cons]
    refine ⟨⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, hwf⟩ <;> omega
  · simp only [Scoped]; omega
  · simp only [Scoped]; omega
  · intro α hcons
    simp only [Consistent, List.length_cons, Gate.eval, outVar] at hcons
    obtain ⟨h5, h4, h3, h2, h1, _⟩ := hcons
    simp only [Lit.val, beq_true] at h5 h4 h3 h2 h1 ⊢
    rw [h2, h1, h5, h4, h3, h1]
    exact ⟨rfl, rfl⟩

/-- Ripple-carry adder; the final carry is appended as the last literal. -/
def ripple : List Lit → List Lit → Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, c, gs =>
    let fa := fullAdd a b c gs
    let rs := ripple as bs fa.1.2 fa.2
    (fa.1.1 :: rs.1, rs.2)
  | _, _, c, gs => ([c], gs)

theorem ripple_spec : ∀ (as bs : List Lit) (c : Lit) (gs : Circuit), WF gs →
    Good gs.length as → Good gs.length bs → Scoped gs.length c →
    Spec gs (ripple as bs c gs)
      (fun α => rippleSem (as.map (Lit.val α)) (bs.map (Lit.val α)) (c.val α))
  | a :: as, b :: bs, c, gs, hwf, ha, hb, hc => by
    obtain ⟨⟨l, hl⟩, hlen, hwf1, hs, hco, hsem⟩ :=
      fullAdd_facts a b c gs hwf (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..)) hc
    have ih := ripple_spec as bs (fullAdd a b c gs).1.2 (fullAdd a b c gs).2 hwf1
      (Good.mono (fun l hl => ha l (List.mem_cons_of_mem _ hl)) (by omega))
      (Good.mono (fun l hl => hb l (List.mem_cons_of_mem _ hl)) (by omega)) hco
    have hsuf : gs <:+ (fullAdd a b c gs).2 := ⟨l, hl.symm⟩
    refine ⟨hsuf.trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro x hx
      simp only [ripple, List.mem_cons] at hx
      rcases hx with rfl | hx
      · exact hs.mono ih.suffix.length_le
      · exact ih.good x hx
    · intro α hcons
      have h1 := hsem α (consistent_of_suffix ih.suffix hcons)
      simp only [ripple, List.map_cons, rippleSem]
      rw [ih.sem α hcons, h1.1, h1.2]
  | [], _, c, gs, hwf, _, _, hc => ⟨List.suffix_refl gs, hwf,
      by simpa [ripple] using hc, by intro α _; simp [ripple, rippleSem]⟩
  | _ :: _, [], c, gs, hwf, _, _, hc => ⟨List.suffix_refl gs, hwf,
      by simpa [ripple] using hc, by intro α _; simp [ripple, rippleSem]⟩

/-- One multiplexer bit: `b ⊕ (c ∧ (a ⊕ b))`, i.e. `if c then a else b`. -/
def muxBit (c a b : Lit) (gs : Circuit) : Lit × Circuit :=
  let t1 := mkGate (.xor a b) gs
  let t2 := mkGate (.and c t1.1) t1.2
  mkGate (.xor b t2.1) t2.2

theorem muxBit_facts (c a b : Lit) (gs : Circuit) (hwf : WF gs) (hc : Scoped gs.length c)
    (ha : Scoped gs.length a) (hb : Scoped gs.length b) :
    let r := muxBit c a b gs
    (∃ l, r.2 = l ++ gs) ∧ r.2.length = gs.length + 3 ∧ WF r.2 ∧ Scoped r.2.length r.1 ∧
    ∀ α, Consistent α r.2 → r.1.val α = if c.val α then a.val α else b.val α := by
  unfold Scoped at ha hb hc
  refine ⟨⟨[_, _, _], rfl⟩, rfl, ?_, ?_, ?_⟩ <;> simp only [muxBit, mkGate, List.length_cons, outVar]
  · simp only [WF, Gate.Scoped, Scoped, List.length_cons]
    refine ⟨⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, hwf⟩ <;> omega
  · simp only [Scoped]; omega
  · intro α hcons
    simp only [Consistent, List.length_cons, Gate.eval, outVar] at hcons
    obtain ⟨h3, h2, h1, _⟩ := hcons
    simp only [Lit.val, beq_true] at h3 h2 h1 ⊢
    rw [h3, h2, h1]
    by_cases hcv : (α c.1 == c.2) = true
    · simp only [hcv, ite_true, Bool.true_and]
      cases (α a.1 == a.2) <;> cases (α b.1 == b.2) <;> rfl
    · simp [hcv]

def muxEnc (c : Lit) : List Lit → List Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, gs =>
    let m := muxBit c a b gs
    let rs := muxEnc c as bs m.2
    (m.1 :: rs.1, rs.2)
  | _, _, gs => ([], gs)

theorem muxEnc_spec (c : Lit) : ∀ (as bs : List Lit) (gs : Circuit), WF gs →
    Scoped gs.length c → Good gs.length as → Good gs.length bs →
    Spec gs (muxEnc c as bs gs)
      (fun α => List.zipWith (fun p q => if c.val α then p else q)
        (as.map (Lit.val α)) (bs.map (Lit.val α)))
  | a :: as, b :: bs, gs, hwf, hc, ha, hb => by
    obtain ⟨⟨l, hl⟩, hlen, hwf1, hs, hsem⟩ :=
      muxBit_facts c a b gs hwf hc (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..))
    have ih := muxEnc_spec c as bs (muxBit c a b gs).2 hwf1 (hc.mono (by omega))
      (Good.mono (fun l hl => ha l (List.mem_cons_of_mem _ hl)) (by omega))
      (Good.mono (fun l hl => hb l (List.mem_cons_of_mem _ hl)) (by omega))
    have hsuf : gs <:+ (muxBit c a b gs).2 := ⟨l, hl.symm⟩
    refine ⟨hsuf.trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro x hx
      simp only [muxEnc, List.mem_cons] at hx
      rcases hx with rfl | hx
      · exact hs.mono ih.suffix.length_le
      · exact ih.good x hx
    · intro α hcons
      simp only [muxEnc, List.map_cons, List.zipWith_cons_cons]
      rw [ih.sem α hcons, hsem α (consistent_of_suffix ih.suffix hcons)]
  | [], _, gs, hwf, _, _, _ => ⟨List.suffix_refl gs, hwf, by simp [muxEnc], by simp [muxEnc]⟩
  | _ :: _, [], gs, hwf, _, _, _ => ⟨List.suffix_refl gs, hwf, by simp [muxEnc], by simp [muxEnc]⟩

/-- Conjunction of a list of literals onto an accumulator; one output literal. -/
def andAll : List Lit → Lit → Circuit → List Lit × Circuit
  | [], acc, gs => ([acc], gs)
  | l :: ls, acc, gs =>
    let r := mkGate (.and acc l) gs
    andAll ls r.1 r.2

theorem andAll_spec : ∀ (ls : List Lit) (acc : Lit) (gs : Circuit), WF gs →
    Good gs.length ls → Scoped gs.length acc →
    Spec gs (andAll ls acc gs) (fun α => [andSem (ls.map (Lit.val α)) (acc.val α)])
  | [], acc, gs, hwf, _, hacc => ⟨List.suffix_refl gs, hwf, by simpa [andAll] using hacc,
      by intro α _; simp [andAll, andSem]⟩
  | l :: ls, acc, gs, hwf, hls, hacc => by
    have hwf1 := mkGate_wf (g := .and acc l) hwf ⟨hacc, hls l (List.mem_cons_self ..)⟩
    have ih := andAll_spec ls _ _ hwf1
      (Good.mono (fun x hx => hls x (List.mem_cons_of_mem _ hx)) (by simp))
      (mkGate_scoped _ _)
    refine ⟨(mkGate_suffix _ gs).trans ih.suffix, ih.wf, ih.good, ?_⟩
    intro α hcons
    rw [andAll, ih.sem α hcons, mkGate_val (consistent_of_suffix ih.suffix hcons)]
    simp [andSem, Gate.eval]

/-! ### The expression encoder -/

def varLits (w base : Nat) : List Lit := (List.range w).map fun i => (2 * (base + i), true)

def constLits {w : Nat} (v : BitVec w) : List Lit := (List.range w).map fun i => (1, v.getLsbD i)

/-- Bit-blast an expression: output literals (least significant first) and
the extended circuit. -/
def encode : {w : Nat} → BVExpr w → Circuit → List Lit × Circuit
  | w, .var base, gs => (varLits w base, gs)
  | _, .const v, gs => (constLits v, gs)
  | _, .not e, gs =>
    let r := encode e gs
    (r.1.map Lit.neg, r.2)
  | _, .and a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    zipGate Gate.and r1.1 r2.1 r2.2
  | _, .or a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    zipGate Gate.or r1.1 r2.1 r2.2
  | _, .xor a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    zipGate Gate.xor r1.1 r2.1 r2.2
  | _, .add a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    let r := ripple r1.1 r2.1 FF r2.2
    (r.1.dropLast, r.2)
  | _, .ite c a b, gs =>
    let r0 := encode c gs
    let r1 := encode a r0.2
    let r2 := encode b r1.2
    muxEnc (r0.1.headD FF) r1.1 r2.1 r2.2
  | _, .eq a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    let x := zipGate Gate.xor r1.1 r2.1 r2.2
    andAll (x.1.map Lit.neg) TT x.2
  | _, .ult a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    let r := ripple r1.1 (r2.1.map Lit.neg) TT r2.2
    ([(r.1.getLastD FF).neg], r.2)
  | _, .slt a b, gs =>
    let r1 := encode a gs
    let r2 := encode b r1.2
    let r := ripple r1.1 (r2.1.map Lit.neg) TT r2.2
    let d := mkGate (.xor (r1.1.getLastD FF) (r2.1.getLastD FF)) r.2
    let e := mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2
    ([e.1], e.2)

/-- The input assignment read back from a CNF assignment. -/
def inputOf (α : Nat → Bool) : Nat → Bool := fun j => α (2 * j)

theorem good_headD {n : Nat} {ls : List Lit} (h : Good n ls) : Scoped n (ls.headD FF) := by
  cases ls with
  | nil => exact scoped_FF n
  | cons l ls => exact h l (List.mem_cons_self ..)

theorem good_getLastD {n : Nat} {ls : List Lit} (h : Good n ls) : Scoped n (ls.getLastD FF) := by
  rw [List.getLastD_eq_getLast?]
  cases hl : ls.getLast? with
  | none => exact scoped_FF n
  | some l => exact h l (List.mem_of_getLast? hl)

theorem val_headD {α : Nat → Bool} {gs : Circuit} (hc : Consistent α gs) (ls : List Lit) :
    (ls.headD FF).val α = (ls.map (Lit.val α)).headD false := by
  rw [← val_FF hc, List.headD_map]

theorem val_getLastD {α : Nat → Bool} {gs : Circuit} (hc : Consistent α gs) (ls : List Lit) :
    (ls.getLastD FF).val α = (ls.map (Lit.val α)).getLastD false := by
  rw [← val_FF hc, List.getLastD_map]

/-- A binary step: encode `a`, then `b`; both results stay valid in any
extension of the final circuit. -/
theorem spec_pair {w : Nat} {gs : Circuit} {a b : BVExpr w}
    (ha : Spec gs (encode a gs) (fun α => toBits (a.denote (inputOf α))))
    (hb : Spec (encode a gs).2 (encode b (encode a gs).2)
      (fun α => toBits (b.denote (inputOf α)))) :
    gs <:+ (encode b (encode a gs).2).2 ∧ WF (encode b (encode a gs).2).2 ∧
    Good (encode b (encode a gs).2).2.length (encode a gs).1 ∧
    Good (encode b (encode a gs).2).2.length (encode b (encode a gs).2).1 ∧
    (∀ gs', (encode b (encode a gs).2).2 <:+ gs' → ∀ α, Consistent α gs' →
      (encode a gs).1.map (Lit.val α) = toBits (a.denote (inputOf α)) ∧
      (encode b (encode a gs).2).1.map (Lit.val α) = toBits (b.denote (inputOf α))) :=
  ⟨ha.suffix.trans hb.suffix, hb.wf, fun l hl => (ha.good l hl).mono hb.suffix.length_le,
    hb.good, fun _ hs α hc =>
      ⟨ha.sem α (consistent_of_suffix (hb.suffix.trans hs) hc),
       hb.sem α (consistent_of_suffix hs hc)⟩⟩

theorem zip_case {w : Nat} {gs : Circuit} {a b : BVExpr w} (mk : Lit → Lit → Gate)
    (f : Bool → Bool → Bool)
    (hmk : ∀ α a b, (mk a b).eval α = f (a.val α) (b.val α))
    (hsc : ∀ n a b, Scoped n a → Scoped n b → (mk a b).Scoped n)
    (ha : Spec gs (encode a gs) (fun α => toBits (a.denote (inputOf α))))
    (hb : Spec (encode a gs).2 (encode b (encode a gs).2)
      (fun α => toBits (b.denote (inputOf α)))) :
    Spec gs (zipGate mk (encode a gs).1 (encode b (encode a gs).2).1 (encode b (encode a gs).2).2)
      (fun α => List.zipWith f (toBits (a.denote (inputOf α))) (toBits (b.denote (inputOf α)))) := by
  obtain ⟨hsuf, hwf, hga, hgb, hsem⟩ := spec_pair ha hb
  have hz := zipGate_spec mk f hmk hsc _ _ _ hwf hga hgb
  refine ⟨hsuf.trans hz.suffix, hz.wf, hz.good, fun α hc => ?_⟩
  obtain ⟨e1, e2⟩ := hsem _ hz.suffix α hc
  rw [hz.sem α hc, e1, e2]

/-- **Encoder correctness.**  Under every assignment consistent with the
produced circuit, the output literals are the bits of the expression's value
on the input bits read from that assignment. -/
theorem encode_spec : ∀ {w : Nat} (e : BVExpr w) (gs : Circuit), WF gs →
    Spec gs (encode e gs) (fun α => toBits (e.denote (inputOf α)))
  | w, .var base, gs, hwf => by
    refine ⟨List.suffix_refl gs, hwf, ?_, ?_⟩
    · intro l hl
      simp only [encode, varLits, List.mem_map] at hl
      obtain ⟨i, _, rfl⟩ := hl
      left; simp
    · intro α _
      simp only [encode, varLits, BVExpr.denote, toBits_bvOf, List.map_map]
      apply List.map_congr_left
      intro i _
      simp [Lit.val, inputOf]
  | _, .const v, gs, hwf => by
    refine ⟨List.suffix_refl gs, hwf, ?_, ?_⟩
    · intro l hl
      simp only [encode, constLits, List.mem_map] at hl
      obtain ⟨i, _, rfl⟩ := hl
      right; simp
    · intro α hc
      simp only [encode, constLits, BVExpr.denote, toBits, List.map_map]
      apply List.map_congr_left
      intro i _
      simp [Lit.val, consistent_true hc]
  | _, .not e, gs, hwf => by
    have h := encode_spec e gs hwf
    refine ⟨h.suffix, h.wf, good_map_neg h.good, fun α hc => ?_⟩
    simp only [encode, BVExpr.denote]
    rw [map_neg_val, h.sem α hc, toBits_not]
  | _, .and a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    have := zip_case Gate.and (· && ·) (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) ha hb
    simpa only [encode, BVExpr.denote, toBits_and] using this
  | _, .or a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    have := zip_case Gate.or (· || ·) (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) ha hb
    simpa only [encode, BVExpr.denote, toBits_or] using this
  | _, .xor a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    have := zip_case Gate.xor (· ^^ ·) (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) ha hb
    simpa only [encode, BVExpr.denote, toBits_xor] using this
  | _, .add a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    obtain ⟨hsuf, hwf2, hga, hgb, hsem⟩ := spec_pair ha hb
    have hr := ripple_spec _ _ FF _ hwf2 hga hgb (scoped_FF _)
    refine ⟨hsuf.trans hr.suffix, hr.wf, fun l hl => hr.good l (List.dropLast_subset _ hl),
      fun α hc => ?_⟩
    obtain ⟨e1, e2⟩ := hsem _ hr.suffix α hc
    simp only [encode, BVExpr.denote]
    rw [List.map_dropLast, hr.sem α hc, e1, e2, val_FF hc, toBits_add]
  | _, .ite c a b, gs, hwf => by
    have hc0 := encode_spec c gs hwf
    have ha := encode_spec a _ hc0.wf
    have hb := encode_spec b _ ha.wf
    obtain ⟨hsuf, hwf2, hga, hgb, hsem⟩ := spec_pair ha hb
    have hcg : Scoped (encode b (encode a (encode c gs).2).2).2.length ((encode c gs).1.headD FF) :=
      (good_headD hc0.good).mono (hsuf.length_le)
    have hm := muxEnc_spec _ _ _ _ hwf2 hcg hga hgb
    refine ⟨hc0.suffix.trans (hsuf.trans hm.suffix), hm.wf, hm.good, fun α hc => ?_⟩
    obtain ⟨e1, e2⟩ := hsem _ hm.suffix α hc
    have hc1 : Consistent α (encode c gs).2 :=
      consistent_of_suffix (hsuf.trans hm.suffix) hc
    simp only [encode, BVExpr.denote]
    rw [hm.sem α hc, e1, e2, val_headD hc, hc0.sem α hc1, toBits_ite]
    simp [toBits, List.range_succ]
  | _, .eq a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    have hz := zip_case Gate.xor (· ^^ ·) (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) ha hb
    have hall := andAll_spec _ TT _ hz.wf (good_map_neg hz.good) (scoped_TT _)
    refine ⟨hz.suffix.trans hall.suffix, hall.wf, hall.good, fun α hc => ?_⟩
    simp only [encode, BVExpr.denote]
    rw [hall.sem α hc, map_neg_val, hz.sem α (consistent_of_suffix hall.suffix hc), val_TT hc,
      toBits_ofBool, beq_bits]
  | _, .ult a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    obtain ⟨hsuf, hwf2, hga, hgb, hsem⟩ := spec_pair ha hb
    have hr := ripple_spec _ _ TT _ hwf2 hga (good_map_neg hgb) (scoped_TT _)
    refine ⟨hsuf.trans hr.suffix, hr.wf, ?_, fun α hc => ?_⟩
    · intro l hl
      simp only [encode, List.mem_singleton] at hl
      subst hl
      exact good_getLastD hr.good
    · obtain ⟨e1, e2⟩ := hsem _ hr.suffix α hc
      simp only [encode, BVExpr.denote, List.map_cons, List.map_nil, Lit.val_neg]
      rw [val_getLastD hc, hr.sem α hc, map_neg_val, e1, e2, val_TT hc, toBits_ofBool,
        ← toBits_not, ← ult_bits]
  | _, .slt a b, gs, hwf => by
    have ha := encode_spec a gs hwf
    have hb := encode_spec b _ ha.wf
    obtain ⟨hsuf, hwf2, hga, hgb, hsem⟩ := spec_pair ha hb
    have hr := ripple_spec _ _ TT _ hwf2 hga (good_map_neg hgb) (scoped_TT _)
    generalize hR : ripple (encode a gs).1 ((encode b (encode a gs).2).1.map Lit.neg) TT
      (encode b (encode a gs).2).2 = r at hr
    have hlen := hr.suffix.length_le
    generalize hD : mkGate (.xor ((encode a gs).1.getLastD FF)
      ((encode b (encode a gs).2).1.getLastD FF)) r.2 = d
    have hwfd : WF d.2 := hD ▸ mkGate_wf hr.wf
      ⟨(good_getLastD hga).mono hlen, (good_getLastD hgb).mono hlen⟩
    have hdlen : r.2.length + 1 = d.2.length := hD ▸ rfl
    have hsd : r.2 <:+ d.2 := hD ▸ mkGate_suffix _ _
    have hwfe : WF (mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).2 :=
      mkGate_wf hwfd ⟨hD ▸ mkGate_scoped _ _, (good_getLastD hr.good).mono (by omega)⟩
    have hunf : encode (.slt a b) gs = ([(mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).1],
        (mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).2) := by
      rw [← hD, ← hR]; rfl
    rw [hunf]
    refine ⟨hsuf.trans (hr.suffix.trans (hsd.trans (mkGate_suffix _ _))),
      hwfe, ?_, fun α hc => ?_⟩
    · intro l hl
      simp only [List.mem_singleton] at hl
      subst hl
      exact mkGate_scoped _ _
    · have hcd : Consistent α d.2 := consistent_of_suffix (mkGate_suffix _ _) hc
      have hcr : Consistent α r.2 := consistent_of_suffix hsd hcd
      obtain ⟨e1, e2⟩ := hsem _ hr.suffix α hcr
      have hdv : d.1.val α = (((encode a gs).1.getLastD FF).val α ^^
          ((encode b (encode a gs).2).1.getLastD FF).val α) := by
        subst hD; exact mkGate_val hcd
      simp only [BVExpr.denote, List.map_cons, List.map_nil]
      rw [mkGate_val hc]
      simp only [Gate.eval, Lit.val_neg]
      rw [hdv, val_getLastD hcr, val_getLastD hcr, val_getLastD hcr, hr.sem α hcr, map_neg_val,
        e1, e2, val_TT hcr, ← toBits_not, toBits_ofBool, slt_bits]

/-! ## CNF and the main theorems -/

/-- The Tseitin CNF of a 1-bit formula: all gate definitions, the constant,
and a unit clause asserting the output bit. -/
def toCNF (φ : BVExpr 1) : CNF Nat :=
  let r := encode φ []
  ⟨(Circuit.clauses r.2 ++ [[r.1.headD FF]]).toArray⟩

theorem toCNF_eval (φ : BVExpr 1) (α : Nat → Bool) :
    (toCNF φ).eval α = true ↔
      Consistent α (encode φ []).2 ∧ ((encode φ []).1.headD FF).val α = true := by
  simp only [toCNF, CNF.eval, List.all_toArray, List.all_append, Bool.and_eq_true,
    clauses_all_iff, List.all_cons, List.all_nil, Bool.and_true, CNF.Clause.eval_cons,
    CNF.Clause.eval_nil, Bool.or_false]
  rfl

theorem encode_top (φ : BVExpr 1) (α : Nat → Bool) (hc : Consistent α (encode φ []).2) :
    ((encode φ []).1.headD FF).val α = (φ.denote (inputOf α)).getLsbD 0 := by
  rw [val_headD hc, (encode_spec φ [] trivial).sem α hc]
  simp [toBits, List.range_succ]

/-- **Completeness of the encoding**: a satisfying CNF assignment yields a
satisfying input for the formula. -/
theorem sat_of_cnf_sat (φ : BVExpr 1) (α : Nat → Bool) (h : (toCNF φ).Sat α) :
    φ.denote (inputOf α) = 1#1 := by
  obtain ⟨hc, hv⟩ := (toCNF_eval φ α).1 h
  rw [bv1_eq_one, ← encode_top φ α hc, hv]

/-- **Soundness of the encoding**: a satisfying input for the formula extends
to a satisfying CNF assignment. -/
theorem cnf_sat_of_sat (φ : BVExpr 1) (ρ : Nat → Bool) (h : φ.denote ρ = 1#1) :
    (toCNF φ).Sat (extend ρ (encode φ []).2) := by
  have hwf := (encode_spec φ [] trivial).wf
  have hc := extend_consistent ρ _ hwf
  refine (toCNF_eval φ _).2 ⟨hc, ?_⟩
  rw [encode_top φ _ hc]
  have : inputOf (extend ρ (encode φ []).2) = ρ := by
    funext j; exact extend_input ρ j _
  rw [this]
  exact (bv1_eq_one _).1 h

/-- **Equisatisfiability** of the bit-blasted CNF and the bitvector formula. -/
theorem toCNF_equisat (φ : BVExpr 1) : (∃ α, (toCNF φ).Sat α) ↔ FSat φ :=
  ⟨fun ⟨α, h⟩ => ⟨inputOf α, sat_of_cnf_sat φ α h⟩,
   fun ⟨ρ, h⟩ => ⟨_, cnf_sat_of_sat φ ρ h⟩⟩

/-- The direction certified mode relies on: an unsatisfiable CNF means no
input makes the formula true. -/
theorem toCNF_unsat_imp (φ : BVExpr 1) (h : (toCNF φ).Unsat) : ∀ ρ, φ.denote ρ ≠ 1#1 := by
  intro ρ hρ
  have := cnf_sat_of_sat φ ρ hρ
  rw [CNF.sat_def, h] at this
  exact Bool.false_ne_true this

/-- **Certified mode, composed.**  If core Lean's verified LRAT checker
(`Std.Tactic.BVDecide.LRAT.check`, soundness `LRAT.check_sound`) accepts a
certificate for `toCNF φ`, then `φ` is unsatisfiable. -/
theorem certified_unsat (φ : BVExpr 1) (cert : Array Std.Tactic.BVDecide.LRAT.IntAction)
    (h : Std.Tactic.BVDecide.LRAT.check cert (toCNF φ) = true) : ∀ ρ, φ.denote ρ ≠ 1#1 :=
  toCNF_unsat_imp φ (Std.Tactic.BVDecide.LRAT.check_sound cert (toCNF φ) h)

end PrismTechniques.Bitblast
