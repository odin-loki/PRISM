/-
PRISM techniques: the bit-blaster's operator circuits (roadmap 5.4 / 8.2).

Every builder here works on little-endian literal lists and is proved
against `Rep`/`Spec` (see `PrismTechniques/Bitblast.lean`): if its inputs
represent bitvectors `X`, `Y`, its output represents `F X Y` for the core Lean
operation `F`.  `Op1`/`Op2` package that statement so the expression encoder
(`PrismTechniques/BitblastEncode.lean`) uses one lemma per operator.
-/
import PrismTechniques.Bitblast

namespace PrismTechniques.Bitblast

open Std.Sat

/-- A unary builder that computes `F`. -/
def Op1 {w u : Nat} (op : List Lit → Circuit → List Lit × Circuit) (F : BitVec w → BitVec u) :
    Prop :=
  ∀ (c : Circuit) (ls : List Lit) (X : (Nat → Bool) → BitVec w), Rep c ls X →
    Spec c (op ls c) (fun α => toBits (F (X α)))

/-- A binary builder that computes `F`. -/
def Op2 {w v u : Nat} (op : List Lit → List Lit → Circuit → List Lit × Circuit)
    (F : BitVec w → BitVec v → BitVec u) : Prop :=
  ∀ (c : Circuit) (as bs : List Lit) (X : (Nat → Bool) → BitVec w)
    (Y : (Nat → Bool) → BitVec v),
    Rep c as X → Rep c bs Y → Spec c (op as bs c) (fun α => toBits (F (X α) (Y α)))

theorem single_val {c c' : Circuit} {l : Lit} {b : (Nat → Bool) → Bool}
    (h : Spec c ([l], c') (fun α => [b α])) {α : Nat → Bool} (hc : Consistent α c') :
    l.val α = b α := by
  simpa using h.sem α hc

/-! ## Constants -/

def constLits {w : Nat} (v : BitVec w) : List Lit := (List.range w).map fun i => (1, v.getLsbD i)

theorem rep_const {c : Circuit} (hwf : c.WF) {w : Nat} (v : BitVec w) :
    Rep c (constLits v) (fun _ => v) :=
  (wire_spec (fun i => ((1, v.getLsbD i) : Lit)) (fun _ => v) hwf
    (fun i _ => by right; omega)
    (fun α hc i _ => by simp [Lit.val, consistent_true hc])).rep

/-! ## Bitwise gates -/

/-- Apply a two-input gate bitwise. -/
def zipGate (mk : Lit → Lit → Gate) : List Lit → List Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, c =>
    let r := mkGate (mk a b) c
    let rs := zipGate mk as bs r.2
    (r.1 :: rs.1, rs.2)
  | _, _, c => ([], c)

theorem zipGate_spec (mk : Lit → Lit → Gate) (f : Bool → Bool → Bool)
    (hmk : ∀ α a b, (mk a b).eval α = f (a.val α) (b.val α))
    (hsc : ∀ n a b, Scoped n a → Scoped n b → (mk a b).Scoped n) :
    ∀ (as bs : List Lit) (c : Circuit), c.WF → Good c.len as → Good c.len bs →
      Spec c (zipGate mk as bs c)
        (fun α => List.zipWith f (as.map (Lit.val α)) (bs.map (Lit.val α)))
  | a :: as, b :: bs, c, hwf, ha, hb => by
    let r := mkGate (mk a b) c
    have hwf1 : r.2.WF := mkGate_wf hwf
      (hsc _ a b (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..)))
    have ih := zipGate_spec mk f hmk hsc as bs r.2 hwf1
      ((fun l hl => ha l (List.mem_cons_of_mem _ hl)) |> fun h => Good.mono h (by simp [r]))
      ((fun l hl => hb l (List.mem_cons_of_mem _ hl)) |> fun h => Good.mono h (by simp [r]))
    refine ⟨(mkGate_suffix _ c).trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro l hl
      simp only [zipGate, List.mem_cons] at hl
      rcases hl with rfl | hl
      · exact (mkGate_scoped _ c).mono (Circuit.len_le ih.suffix hwf1 ih.wf)
      · exact ih.good l hl
    · intro α hc
      simp only [zipGate, List.map_cons, List.zipWith_cons_cons]
      rw [ih.sem α hc, mkGate_val hwf (consistent_of_suffix ih.suffix hc), hmk]
  | [], _, c, hwf, _, _ => ⟨List.suffix_refl _, hwf, by simp [zipGate, Good],
      by simp [zipGate]⟩
  | _ :: _, [], c, hwf, _, _ => ⟨List.suffix_refl _, hwf, by simp [zipGate, Good],
      by simp [zipGate]⟩

theorem zipGate_op (mk : Lit → Lit → Gate) (f : Bool → Bool → Bool) {w : Nat}
    (F : BitVec w → BitVec w → BitVec w)
    (hmk : ∀ α a b, (mk a b).eval α = f (a.val α) (b.val α))
    (hsc : ∀ n a b, Scoped n a → Scoped n b → (mk a b).Scoped n)
    (hF : ∀ x y, toBits (F x y) = List.zipWith f (toBits x) (toBits y)) :
    Op2 (zipGate mk) F := by
  intro c as bs X Y ha hb
  refine (zipGate_spec mk f hmk hsc as bs c ha.wf ha.good hb.good).congr ?_
  intro α hc
  have hs := (zipGate_spec mk f hmk hsc as bs c ha.wf ha.good hb.good).suffix
  rw [ha.sem α (consistent_of_suffix hs hc), hb.sem α (consistent_of_suffix hs hc), hF]

def andOp := zipGate Gate.and
def orOp := zipGate Gate.or
def xorOp := zipGate Gate.xor

theorem andOp_spec {w : Nat} : Op2 andOp (fun x y : BitVec w => x &&& y) :=
  zipGate_op _ (· && ·) _ (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) toBits_and
theorem orOp_spec {w : Nat} : Op2 orOp (fun x y : BitVec w => x ||| y) :=
  zipGate_op _ (· || ·) _ (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) toBits_or
theorem xorOp_spec {w : Nat} : Op2 xorOp (fun x y : BitVec w => x ^^^ y) :=
  zipGate_op _ (· ^^ ·) _ (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩) toBits_xor

def notOp (ls : List Lit) (c : Circuit) : List Lit × Circuit := (ls.map Lit.neg, c)

theorem notOp_spec {w : Nat} : Op1 notOp (fun x : BitVec w => ~~~x) := by
  intro c ls X h
  refine ⟨List.suffix_refl _, h.wf, good_map_neg h.good, fun α hc => ?_⟩
  simp only [notOp]
  rw [map_neg_val, h.sem α hc, toBits_not]

/-! ## Adder, subtractor, comparators -/

/-- Full adder: sum `(a ⊕ b) ⊕ c`, carry `(a ∧ b) ∨ ((a ⊕ b) ∧ c)`. -/
def fullAdd (a b ci : Lit) (c : Circuit) : (Lit × Lit) × Circuit :=
  let t := mkGate (.xor a b) c
  let s := mkGate (.xor t.1 ci) t.2
  let u := mkGate (.and a b) s.2
  let v := mkGate (.and t.1 ci) u.2
  let co := mkGate (.or u.1 v.1) v.2
  ((s.1, co.1), co.2)

theorem fullAdd_facts (a b ci : Lit) (c : Circuit) (hwf : c.WF) (ha : Scoped c.len a)
    (hb : Scoped c.len b) (hc : Scoped c.len ci) :
    let r := fullAdd a b ci c
    c.gates <:+ r.2.gates ∧ r.2.len = c.len + 5 ∧ r.2.WF ∧
    Scoped r.2.len r.1.1 ∧ Scoped r.2.len r.1.2 ∧
    ∀ α, Consistent α r.2 →
      r.1.1.val α = ((a.val α ^^ b.val α) ^^ ci.val α) ∧
      r.1.2.val α = ((a.val α && b.val α) || ((a.val α ^^ b.val α) && ci.val α)) := by
  obtain ⟨gs, n⟩ := c
  obtain ⟨hn, hw⟩ := hwf
  simp only at hn
  subst hn
  simp only at ha hb hc
  unfold Scoped at ha hb hc
  refine ⟨⟨[_, _, _, _, _], rfl⟩, rfl, ⟨?_, ?_⟩, ?_, ?_, ?_⟩ <;>
    simp only [fullAdd, mkGate, List.length_cons, outVar]
  · simp only [WFL, Gate.Scoped, Scoped, List.length_cons]
    refine ⟨⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, hw⟩ <;> omega
  · simp only [Scoped]; omega
  · simp only [Scoped]; omega
  · intro α hcons
    simp only [Consistent, ConsL, List.length_cons, Gate.eval, outVar] at hcons
    obtain ⟨h5, h4, h3, h2, h1, _⟩ := hcons
    simp only [Lit.val, beq_true] at h5 h4 h3 h2 h1 ⊢
    rw [h2, h1, h5, h4, h3, h1]
    exact ⟨rfl, rfl⟩

/-- Ripple-carry adder; the final carry is appended as the last literal. -/
def ripple : List Lit → List Lit → Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, ci, c =>
    let fa := fullAdd a b ci c
    let rs := ripple as bs fa.1.2 fa.2
    (fa.1.1 :: rs.1, rs.2)
  | _, _, ci, c => ([ci], c)

theorem ripple_spec : ∀ (as bs : List Lit) (ci : Lit) (c : Circuit), c.WF →
    Good c.len as → Good c.len bs → Scoped c.len ci →
    Spec c (ripple as bs ci c)
      (fun α => rippleSem (as.map (Lit.val α)) (bs.map (Lit.val α)) (ci.val α))
  | a :: as, b :: bs, ci, c, hwf, ha, hb, hc => by
    obtain ⟨hsuf, hlen, hwf1, hs, hco, hsem⟩ :=
      fullAdd_facts a b ci c hwf (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..)) hc
    have ih := ripple_spec as bs (fullAdd a b ci c).1.2 (fullAdd a b ci c).2 hwf1
      (Good.mono (fun l hl => ha l (List.mem_cons_of_mem _ hl)) (by omega))
      (Good.mono (fun l hl => hb l (List.mem_cons_of_mem _ hl)) (by omega)) hco
    refine ⟨hsuf.trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro x hx
      simp only [ripple, List.mem_cons] at hx
      rcases hx with rfl | hx
      · exact hs.mono (Circuit.len_le ih.suffix hwf1 ih.wf)
      · exact ih.good x hx
    · intro α hcons
      have h1 := hsem α (consistent_of_suffix ih.suffix hcons)
      simp only [ripple, List.map_cons, rippleSem]
      rw [ih.sem α hcons, h1.1, h1.2]
  | [], _, ci, c, hwf, _, _, hc => ⟨List.suffix_refl _, hwf,
      good_cons (by simpa [ripple] using hc) (good_nil _), by intro α _; simp [ripple, rippleSem]⟩
  | _ :: _, [], ci, c, hwf, _, _, hc => ⟨List.suffix_refl _, hwf,
      good_cons (by simpa [ripple] using hc) (good_nil _), by intro α _; simp [ripple, rippleSem]⟩

/-- The adder run on represented inputs. -/
theorem ripple_rep {c : Circuit} {as bs : List Lit} {ci : Lit} {w : Nat}
    {X Y : (Nat → Bool) → BitVec w} {C : (Nat → Bool) → Bool}
    (ha : Rep c as X) (hb : Rep c bs Y) (hci : Scoped c.len ci)
    (hC : ∀ α, Consistent α c → ci.val α = C α) :
    Spec c (ripple as bs ci c) (fun α => rippleSem (toBits (X α)) (toBits (Y α)) (C α)) := by
  have h := ripple_spec as bs ci c ha.wf ha.good hb.good hci
  refine h.congr fun α hc => ?_
  have hc0 := consistent_of_suffix h.suffix hc
  rw [ha.sem α hc0, hb.sem α hc0, hC α hc0]

def addOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple as bs FF c
  (r.1.dropLast, r.2)

theorem addOp_spec {w : Nat} : Op2 addOp (fun x y : BitVec w => x + y) := by
  intro c as bs X Y ha hb
  have h := ripple_rep ha hb (scoped_FF _) (C := fun _ => false) (fun α hc => val_FF hc)
  refine ⟨h.suffix, h.wf, good_dropLast h.good, fun α hc => ?_⟩
  simp only [addOp]
  rw [List.map_dropLast, h.sem α hc, toBits_add]

def subOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple as (bs.map Lit.neg) TT c
  (r.1.dropLast, r.2)

theorem notOp_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) : Rep c (ls.map Lit.neg) (fun α => ~~~X α) :=
  (notOp_spec c ls X h).rep

theorem subOp_spec {w : Nat} : Op2 subOp (fun x y : BitVec w => x - y) := by
  intro c as bs X Y ha hb
  have h := ripple_rep ha (notOp_rep hb) (scoped_TT _) (C := fun _ => true)
    (fun α hc => val_TT hc)
  refine ⟨h.suffix, h.wf, good_dropLast h.good, fun α hc => ?_⟩
  simp only [subOp]
  rw [List.map_dropLast, h.sem α hc, toBits_sub]

def negOp (w : Nat) (ls : List Lit) (c : Circuit) : List Lit × Circuit :=
  subOp (constLits (0#w)) ls c

theorem negOp_spec {w : Nat} : Op1 (negOp w) (fun x : BitVec w => -x) := by
  intro c ls X h
  have := subOp_spec c _ ls _ X (rep_const h.wf (0#w)) h
  simp only [negOp]
  simpa only [BitVec.zero_sub] using this

/-- `x <ᵤ y`: no carry out of `x + ~~~y + 1`. -/
def ultOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple as (bs.map Lit.neg) TT c
  ([(r.1.getLastD FF).neg], r.2)

theorem ultOp_spec {w : Nat} : Op2 ultOp (fun x y : BitVec w => BitVec.ofBool (x.ult y)) := by
  intro c as bs X Y ha hb
  have h := ripple_rep ha (notOp_rep hb) (scoped_TT _) (C := fun _ => true)
    (fun α hc => val_TT hc)
  show Spec c ([((ripple as (bs.map Lit.neg) TT c).1.getLastD FF).neg],
    (ripple as (bs.map Lit.neg) TT c).2) _
  refine h.trans (rep_single (l := ((ripple as (bs.map Lit.neg) TT c).1.getLastD FF).neg)
    h.wf (good_getLastD h.good) (B := fun α => (X α).ult (Y α)) (fun α hc => ?_)).spec
  rw [Lit.val_neg, val_getLastD hc, h.sem α hc, ult_bits']

/-- `x ≤ᵤ y`: a carry out of `y + ~~~x + 1`. -/
def uleOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple bs (as.map Lit.neg) TT c
  ([r.1.getLastD FF], r.2)

theorem uleOp_spec {w : Nat} : Op2 uleOp (fun x y : BitVec w => BitVec.ofBool (x.ule y)) := by
  intro c as bs X Y ha hb
  have h := ripple_rep hb (notOp_rep ha) (scoped_TT _) (C := fun _ => true)
    (fun α hc => val_TT hc)
  refine h.trans (rep_single h.wf (good_getLastD h.good) (B := fun α => (X α).ule (Y α))
    (fun α hc => ?_)).spec
  rw [val_getLastD hc, h.sem α hc, ule_bits]

/-- `x <ₛ y`: the unsigned comparison corrected by the sign bits. -/
def sltOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple as (bs.map Lit.neg) TT c
  let d := mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) r.2
  let e := mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2
  ([e.1], e.2)

theorem sltOp_spec {w : Nat} : Op2 sltOp (fun x y : BitVec w => BitVec.ofBool (x.slt y)) := by
  intro c as bs X Y ha hb
  have h := ripple_rep ha (notOp_rep hb) (scoped_TT _) (C := fun _ => true)
    (fun α hc => val_TT hc)
  have hR := h
  generalize hr : ripple as (bs.map Lit.neg) TT c = r at hR
  have ha' := ha.mono hR.suffix hR.wf
  have hb' := hb.mono hR.suffix hR.wf
  have h1 : Spec r.2 ([(mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) r.2).1],
      (mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) r.2).2)
      (fun α => [(as.getLastD FF).val α ^^ (bs.getLastD FF).val α]) :=
    mkGate_spec hR.wf ⟨good_getLastD ha'.good, good_getLastD hb'.good⟩
  generalize hd : mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) r.2 = d at h1
  have hl1 := Circuit.len_le h1.suffix hR.wf h1.wf
  have h2 : Spec d.2 ([(mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).1],
      (mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).2)
      (fun α => [d.1.val α ^^ !(r.1.getLastD FF).val α]) :=
    (mkGate_spec (g := .xor d.1 (r.1.getLastD FF).neg) h1.wf ⟨h1.good _ (List.mem_singleton_self _),
      (good_getLastD hR.good).mono hl1⟩).congr fun α _ => by simp [Gate.eval]
  have hunf : sltOp as bs c = ([(mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).1],
      (mkGate (.xor d.1 (r.1.getLastD FF).neg) d.2).2) := by
    rw [← hd, ← hr]; rfl
  rw [hunf]
  refine (hR.trans (h1.trans h2)).congr fun α hc => ?_
  have hc1 := consistent_of_suffix h2.suffix hc
  have hc0 := consistent_of_suffix h1.suffix hc1
  rw [single_val h1 hc1, ha'.msb hc0, hb'.msb hc0, val_getLastD hc0,
    hR.sem α hc0]
  show _ = toBits (BitVec.ofBool ((X α).slt (Y α)))
  rw [BitVec.slt_eq_ult, ult_bits', toBits_ofBool]

def sleOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := sltOp bs as c
  (r.1.map Lit.neg, r.2)

theorem sleOp_spec {w : Nat} : Op2 sleOp (fun x y : BitVec w => BitVec.ofBool (x.sle y)) := by
  intro c as bs X Y ha hb
  have h := sltOp_spec c bs as Y X hb ha
  refine ⟨h.suffix, h.wf, good_map_neg h.good, fun α hc => ?_⟩
  simp only [sleOp]
  rw [map_neg_val, h.sem α hc, BitVec.sle_eq_not_slt]
  simp [toBits_ofBool]

/-! ## Equality -/

/-- Conjunction of a list of literals onto an accumulator; one output literal. -/
def andAll : List Lit → Lit → Circuit → List Lit × Circuit
  | [], acc, c => ([acc], c)
  | l :: ls, acc, c =>
    let r := mkGate (.and acc l) c
    andAll ls r.1 r.2

theorem andAll_spec : ∀ (ls : List Lit) (acc : Lit) (c : Circuit), c.WF →
    Good c.len ls → Scoped c.len acc →
    Spec c (andAll ls acc c) (fun α => [andSem (ls.map (Lit.val α)) (acc.val α)])
  | [], acc, c, hwf, _, hacc => ⟨List.suffix_refl _, hwf,
      good_cons (by simpa [andAll] using hacc) (good_nil _), by intro α _; simp [andAll, andSem]⟩
  | l :: ls, acc, c, hwf, hls, hacc => by
    have hwf1 := mkGate_wf (g := .and acc l) hwf ⟨hacc, hls l (List.mem_cons_self ..)⟩
    have ih := andAll_spec ls _ _ hwf1
      (Good.mono (fun x hx => hls x (List.mem_cons_of_mem _ hx)) (by simp))
      (mkGate_scoped _ _)
    refine ⟨(mkGate_suffix _ c).trans ih.suffix, ih.wf, ih.good, ?_⟩
    intro α hcons
    rw [andAll, ih.sem α hcons, mkGate_val hwf (consistent_of_suffix ih.suffix hcons)]
    simp [andSem, Gate.eval]

def eqOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let x := xorOp as bs c
  andAll (x.1.map Lit.neg) TT x.2

theorem eqOp_spec {w : Nat} : Op2 eqOp (fun x y : BitVec w => BitVec.ofBool (x == y)) := by
  intro c as bs X Y ha hb
  have hz := xorOp_spec c as bs X Y ha hb
  have hall := andAll_spec _ TT _ hz.wf (good_map_neg hz.good) (scoped_TT _)
  refine ⟨hz.suffix.trans hall.suffix, hall.wf, hall.good, fun α hc => ?_⟩
  simp only [eqOp]
  rw [hall.sem α hc, map_neg_val, hz.sem α (consistent_of_suffix hall.suffix hc), val_TT hc,
    toBits_ofBool, beq_bits, toBits_xor]

/-! ## Multiplexer -/

/-- One multiplexer bit: `b ⊕ (c ∧ (a ⊕ b))`, i.e. `if c then a else b`. -/
def muxBit (s a b : Lit) (c : Circuit) : Lit × Circuit :=
  let t1 := mkGate (.xor a b) c
  let t2 := mkGate (.and s t1.1) t1.2
  mkGate (.xor b t2.1) t2.2

theorem muxBit_facts (s a b : Lit) (c : Circuit) (hwf : c.WF) (hs : Scoped c.len s)
    (ha : Scoped c.len a) (hb : Scoped c.len b) :
    let r := muxBit s a b c
    c.gates <:+ r.2.gates ∧ r.2.len = c.len + 3 ∧ r.2.WF ∧ Scoped r.2.len r.1 ∧
    ∀ α, Consistent α r.2 → r.1.val α = if s.val α then a.val α else b.val α := by
  obtain ⟨gs, n⟩ := c
  obtain ⟨hn, hw⟩ := hwf
  simp only at hn
  subst hn
  simp only at ha hb hs
  unfold Scoped at ha hb hs
  refine ⟨⟨[_, _, _], rfl⟩, rfl, ⟨?_, ?_⟩, ?_, ?_⟩ <;>
    simp only [muxBit, mkGate, List.length_cons, outVar]
  · simp only [WFL, Gate.Scoped, Scoped, List.length_cons]
    refine ⟨⟨?_, ?_⟩, ⟨?_, ?_⟩, ⟨?_, ?_⟩, hw⟩ <;> omega
  · simp only [Scoped]; omega
  · intro α hcons
    simp only [Consistent, ConsL, List.length_cons, Gate.eval, outVar] at hcons
    obtain ⟨h3, h2, h1, _⟩ := hcons
    simp only [Lit.val, beq_true] at h3 h2 h1 ⊢
    rw [h3, h2, h1]
    by_cases hcv : (α s.1 == s.2) = true
    · simp only [hcv, ite_true, Bool.true_and]
      cases (α a.1 == a.2) <;> cases (α b.1 == b.2) <;> rfl
    · simp [hcv]

def muxEnc (s : Lit) : List Lit → List Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, c =>
    let m := muxBit s a b c
    let rs := muxEnc s as bs m.2
    (m.1 :: rs.1, rs.2)
  | _, _, c => ([], c)

theorem muxEnc_spec (s : Lit) : ∀ (as bs : List Lit) (c : Circuit), c.WF →
    Scoped c.len s → Good c.len as → Good c.len bs →
    Spec c (muxEnc s as bs c)
      (fun α => List.zipWith (fun p q => if s.val α then p else q)
        (as.map (Lit.val α)) (bs.map (Lit.val α)))
  | a :: as, b :: bs, c, hwf, hs, ha, hb => by
    obtain ⟨hsuf, hlen, hwf1, hsc, hsem⟩ :=
      muxBit_facts s a b c hwf hs (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..))
    have ih := muxEnc_spec s as bs (muxBit s a b c).2 hwf1 (hs.mono (by omega))
      (Good.mono (fun l hl => ha l (List.mem_cons_of_mem _ hl)) (by omega))
      (Good.mono (fun l hl => hb l (List.mem_cons_of_mem _ hl)) (by omega))
    refine ⟨hsuf.trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro x hx
      simp only [muxEnc, List.mem_cons] at hx
      rcases hx with rfl | hx
      · exact hsc.mono (Circuit.len_le ih.suffix hwf1 ih.wf)
      · exact ih.good x hx
    · intro α hcons
      simp only [muxEnc, List.map_cons, List.zipWith_cons_cons]
      rw [ih.sem α hcons, hsem α (consistent_of_suffix ih.suffix hcons)]
  | [], _, c, hwf, _, _, _ => ⟨List.suffix_refl _, hwf, by simp [muxEnc, Good],
      by simp [muxEnc]⟩
  | _ :: _, [], c, hwf, _, _, _ => ⟨List.suffix_refl _, hwf, by simp [muxEnc, Good],
      by simp [muxEnc]⟩

/-- The multiplexer on represented inputs. -/
theorem mux_rep {c : Circuit} {s : Lit} {B : (Nat → Bool) → Bool} {as bs : List Lit} {w : Nat}
    {X Y : (Nat → Bool) → BitVec w} (hs : Scoped c.len s)
    (hB : ∀ α, Consistent α c → s.val α = B α) (ha : Rep c as X) (hb : Rep c bs Y) :
    Spec c (muxEnc s as bs c) (fun α => toBits (if B α then X α else Y α)) := by
  have h := muxEnc_spec s as bs c ha.wf hs ha.good hb.good
  refine h.congr fun α hc => ?_
  have hc0 := consistent_of_suffix h.suffix hc
  rw [ha.sem α hc0, hb.sem α hc0, hB α hc0, toBits_ite]

/-! ## Shift-and-add multiplier (core Lean's `BitVec.mulRec`) -/

/-- Partial product `s`: bit `i` is `x[i-s] ∧ yb` for `i ≥ s`, else `0`. -/
def ppEnc (w : Nat) (xs : List Lit) (yb : Lit) (s : Nat) (c : Circuit) : List Lit × Circuit :=
  zipGate Gate.and ((List.range w).map fun i => if s ≤ i then xs.getD (i - s) FF else FF)
    ((List.range w).map fun _ => yb) c

theorem ppEnc_spec (w : Nat) (xs : List Lit) (yb : Lit) (s : Nat) (c : Circuit) (hwf : c.WF)
    (hxs : Good c.len xs) (hyb : Scoped c.len yb) :
    Spec c (ppEnc w xs yb s c) (fun α => (List.range w).map fun i =>
      (if s ≤ i then (xs.getD (i - s) FF).val α else FF.val α) && yb.val α) := by
  have hz := zipGate_spec Gate.and (· && ·) (fun _ _ _ => rfl) (fun _ _ _ h1 h2 => ⟨h1, h2⟩)
    ((List.range w).map fun i => if s ≤ i then xs.getD (i - s) FF else FF)
    ((List.range w).map fun _ => yb) c hwf
    (by
      intro l hl
      obtain ⟨i, _, rfl⟩ := List.mem_map.1 hl
      split
      · exact good_getD hxs _
      · exact scoped_FF _)
    (by
      intro l hl
      obtain ⟨i, _, rfl⟩ := List.mem_map.1 hl
      exact hyb)
  refine ⟨hz.suffix, hz.wf, hz.good, fun α hc => ?_⟩
  change List.map (Lit.val α) (zipGate Gate.and _ _ c).1 = _
  rw [hz.sem α hc, List.map_map, List.map_map, zipWith_map_same]
  apply List.map_congr_left
  intro i _
  simp only [Function.comp_def]
  split <;> rfl

/-- Accumulate partial products `s+1, …, s+n` onto `acc` (= `mulRec x y s`). -/
def mulFrom (w : Nat) (xs ys : List Lit) : Nat → Nat → List Lit → Circuit → List Lit × Circuit
  | _, 0, acc, c => (acc, c)
  | s, n + 1, acc, c =>
    let p := ppEnc w xs (ys.getD (s + 1) FF) (s + 1) c
    let r := ripple acc p.1 FF p.2
    mulFrom w xs ys (s + 1) n r.1.dropLast r.2

theorem mulFrom_spec {w : Nat} (X Y : (Nat → Bool) → BitVec w) (xs ys : List Lit) :
    ∀ (n s : Nat) (acc : List Lit) (c : Circuit), c.WF →
      Good c.len xs → Good c.len ys → Good c.len acc →
      (∀ α, Consistent α c → xs.map (Lit.val α) = toBits (X α) ∧
        ys.map (Lit.val α) = toBits (Y α) ∧
        acc.map (Lit.val α) = toBits (BitVec.mulRec (X α) (Y α) s)) →
      Spec c (mulFrom w xs ys s n acc c)
        (fun α => toBits (BitVec.mulRec (X α) (Y α) (s + n)))
  | 0, s, acc, c, hwf, _, _, hacc, hsem =>
    ⟨List.suffix_refl _, hwf, hacc, fun α hc => (hsem α hc).2.2⟩
  | n + 1, s, acc, c, hwf, hxs, hys, hacc, hsem => by
    have hp := ppEnc_spec w xs (ys.getD (s + 1) FF) (s + 1) c hwf hxs (good_getD hys _)
    have hl1 := Circuit.len_le hp.suffix hwf hp.wf
    have hr := ripple_spec acc _ FF _ hp.wf (hacc.mono hl1) hp.good (scoped_FF _)
    have hl2 := Circuit.len_le hr.suffix hp.wf hr.wf
    have ih := mulFrom_spec X Y xs ys n (s + 1) _ _ hr.wf (hxs.mono (by omega))
      (hys.mono (by omega)) (good_dropLast hr.good) (by
        intro α hc
        have hc1 : Consistent α (ppEnc w xs (ys.getD (s + 1) FF) (s + 1) c).2 :=
          consistent_of_suffix hr.suffix hc
        have hc0 : Consistent α c := consistent_of_suffix hp.suffix hc1
        obtain ⟨ex, ey, ea⟩ := hsem α hc0
        refine ⟨ex, ey, ?_⟩
        rw [List.map_dropLast, hr.sem α hc, hp.sem α hc1, ea, val_FF hc, BitVec.mulRec_succ_eq,
          toBits_add, toBits_pp]
        congr 3
        funext i
        simp only [val_getD hc1, ex, ey, toBits_getD])
    refine ⟨hp.suffix.trans (hr.suffix.trans ih.suffix), ih.wf, ih.good, fun α hc => ?_⟩
    rw [mulFrom, ih.sem α hc, Nat.add_assoc, Nat.add_comm 1 n]

def mulOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let p0 := ppEnc w as (bs.getD 0 FF) 0 c
  mulFrom w as bs 0 w p0.1 p0.2

theorem mulOp_spec {w : Nat} : Op2 (mulOp w) (fun x y : BitVec w => x * y) := by
  intro c as bs X Y ha hb
  have hp := ppEnc_spec w as (bs.getD 0 FF) 0 c ha.wf ha.good (good_getD hb.good 0)
  have hl1 := Circuit.len_le hp.suffix ha.wf hp.wf
  have hm := mulFrom_spec X Y as bs w 0 _ _ hp.wf (ha.good.mono hl1) (hb.good.mono hl1)
    hp.good (by
      intro α hc
      have hc1 := consistent_of_suffix hp.suffix hc
      refine ⟨ha.sem α hc1, hb.sem α hc1, ?_⟩
      rw [hp.sem α hc, mulRec_zero', toBits_pp]
      apply List.map_congr_left
      intro i _
      simp only [val_getD hc1, val_FF hc1, ha.sem α hc1, hb.sem α hc1, toBits_getD])
  refine ⟨hp.suffix.trans hm.suffix, hm.wf, hm.good, fun α hc => ?_⟩
  simp only [mulOp]
  rw [hm.sem α hc, Nat.zero_add, toBits_mul]

/-! ## Rewiring: constant shifts, extension, extraction, concatenation -/

def shlW (w k : Nat) (ls : List Lit) : List Lit :=
  (List.range w).map fun i => if k ≤ i then ls.getD (i - k) FF else FF

def lshrW (w k : Nat) (ls : List Lit) : List Lit := (List.range w).map fun i => ls.getD (k + i) FF

def ashrW (w k : Nat) (ls : List Lit) : List Lit :=
  (List.range w).map fun i => if k + i < w then ls.getD (k + i) FF else ls.getLastD FF

def zextW (n : Nat) (ls : List Lit) : List Lit := (List.range n).map fun i => ls.getD i FF

def sextW (w n : Nat) (ls : List Lit) : List Lit :=
  (List.range n).map fun i => if i < w then ls.getD i FF else ls.getLastD FF

def extractW (lo len : Nat) (ls : List Lit) : List Lit :=
  (List.range len).map fun i => ls.getD (lo + i) FF

theorem shlW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (k : Nat) : Rep c (shlW w k ls) (fun α => X α <<< k) :=
  (wire_spec _ _ h.wf (fun i _ => by split; exact good_getD h.good _; exact scoped_FF _)
    (fun α hc i hi => by
      rw [BitVec.getLsbD_shiftLeft]
      by_cases hk : k ≤ i
      · simp only [hk, ↓reduceIte]; rw [h.bit hc]; simp [hi, Nat.not_lt.2 hk]
      · simp only [hk, ↓reduceIte]; rw [val_FF hc]; simp [Nat.lt_of_not_le hk])).rep

theorem lshrW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (k : Nat) : Rep c (lshrW w k ls) (fun α => X α >>> k) :=
  (wire_spec _ _ h.wf (fun i _ => good_getD h.good _)
    (fun α hc i _ => by rw [BitVec.getLsbD_ushiftRight, h.bit hc])).rep

theorem ashrW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (k : Nat) : Rep c (ashrW w k ls) (fun α => (X α).sshiftRight k) :=
  (wire_spec _ _ h.wf
    (fun i _ => by split; exact good_getD h.good _; exact good_getLastD h.good)
    (fun α hc i hi => by
      rw [BitVec.getLsbD_sshiftRight]
      have : ¬ w ≤ i := by omega
      by_cases hk : k + i < w
      · simp only [hk, ↓reduceIte]; rw [h.bit hc]; simp [hk, this]
      · simp only [hk, ↓reduceIte]; rw [h.msb hc]; simp [this])).rep

theorem zextW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (n : Nat) : Rep c (zextW n ls) (fun α => (X α).setWidth n) :=
  (wire_spec _ _ h.wf (fun i _ => good_getD h.good _)
    (fun α hc i hi => by rw [h.bit hc]; simp [hi])).rep

theorem sextW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (n : Nat) : Rep c (sextW w n ls) (fun α => (X α).signExtend n) :=
  (wire_spec _ _ h.wf
    (fun i _ => by split; exact good_getD h.good _; exact good_getLastD h.good)
    (fun α hc i hi => by
      rw [BitVec.getLsbD_signExtend]
      by_cases hw : i < w
      · simp only [hw, ↓reduceIte]; rw [h.bit hc]; simp [hw, hi]
      · simp only [hw, ↓reduceIte]; rw [h.msb hc]; simp [hi])).rep

theorem extractW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (lo len : Nat) :
    Rep c (extractW lo len ls) (fun α => (X α).extractLsb' lo len) :=
  (wire_spec _ _ h.wf (fun i _ => good_getD h.good _)
    (fun α hc i hi => by rw [h.bit hc]; simp [hi])).rep

theorem toBits_append {n m : Nat} (x : BitVec n) (y : BitVec m) :
    toBits (x ++ y) = toBits y ++ toBits x := by
  apply List.ext_getElem
  · simp [toBits]; omega
  · intro i h1 h2
    by_cases hi : i < m
    · rw [List.getElem_append_left (by simpa [toBits_length] using hi)]
      simp [toBits, BitVec.getLsbD_append, hi]
    · rw [List.getElem_append_right (by simp [toBits_length]; omega)]
      simp [toBits, BitVec.getLsbD_append, hi]

def concatOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit := (bs ++ as, c)

theorem concatOp_spec {w v : Nat} : Op2 concatOp (fun (x : BitVec w) (y : BitVec v) => x ++ y) := by
  intro c as bs X Y ha hb
  refine ⟨List.suffix_refl _, ha.wf, good_append hb.good ha.good, fun α hc => ?_⟩
  simp only [concatOp, List.map_append]
  rw [ha.sem α hc, hb.sem α hc, toBits_append]

/-! ## Barrel shifters (core Lean's `shiftLeftRec`, `ushiftRightRec`, `sshiftRightRec`) -/

/-- Stage `n` shifts by `2^n` when bit `n` of the amount is set. -/
def barrel (sh : Nat → List Lit → List Lit) (ys : List Lit) :
    Nat → List Lit → Circuit → List Lit × Circuit
  | 0, xs, c => muxEnc (ys.getD 0 FF) (sh 1 xs) xs c
  | n + 1, xs, c =>
    let r := barrel sh ys n xs c
    muxEnc (ys.getD (n + 1) FF) (sh (2 ^ (n + 1)) r.1) r.1 r.2

theorem barrel_spec {w v : Nat} (sh : Nat → List Lit → List Lit) (S : BitVec w → Nat → BitVec w)
    (hsh : ∀ (c : Circuit) (ls : List Lit) (Z : (Nat → Bool) → BitVec w) (k : Nat), Rep c ls Z →
      Rep c (sh k ls) (fun α => S (Z α) k))
    (ys : List Lit) (X : (Nat → Bool) → BitVec w) (Y : (Nat → Bool) → BitVec v)
    (R : (Nat → Bool) → Nat → BitVec w)
    (hR0 : ∀ α, R α 0 = if (Y α).getLsbD 0 then S (X α) 1 else X α)
    (hRs : ∀ α n, R α (n + 1) = if (Y α).getLsbD (n + 1) then S (R α n) (2 ^ (n + 1)) else R α n) :
    ∀ (n : Nat) (c : Circuit) (xs : List Lit), Rep c xs X → Rep c ys Y →
      Spec c (barrel sh ys n xs c) (fun α => toBits (R α n))
  | 0, c, xs, hx, hy => by
    refine (mux_rep (good_getD hy.good 0) (B := fun α => (Y α).getLsbD 0)
      (fun α hc => hy.bit hc 0) (hsh c xs X 1 hx) hx).congr fun α _ => ?_
    rw [hR0]
  | n + 1, c, xs, hx, hy => by
    have ih := barrel_spec sh S hsh ys X Y R hR0 hRs n c xs hx hy
    have hr := ih.rep
    have hy' := hy.mono ih.suffix ih.wf
    have hm := mux_rep (good_getD hy'.good (n + 1)) (B := fun α => (Y α).getLsbD (n + 1))
      (fun α hc => hy'.bit hc (n + 1)) (hsh _ _ _ (2 ^ (n + 1)) hr) hr
    refine (ih.trans hm).congr fun α _ => ?_
    rw [hRs]

theorem shift_amt {v : Nat} (y : BitVec v) (n : Nat) (h : y.getLsbD n = true) :
    (BitVec.twoPow v n).toNat = 2 ^ n :=
  BitVec.toNat_twoPow_of_lt (BitVec.lt_of_getLsbD h)

def shlOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  barrel (shlW w) bs (w - 1) as c

theorem shlOp_spec {w : Nat} : Op2 (shlOp w) (fun x y : BitVec w => x <<< y) := by
  intro c as bs X Y ha hb
  refine (barrel_spec (shlW w) (fun z k => z <<< k) (fun _ _ _ k h => shlW_rep h k) bs X Y
    (fun α n => BitVec.shiftLeftRec (X α) (Y α) n) ?_ ?_ (w - 1) c as ha hb).congr ?_
  · intro α
    rw [BitVec.shiftLeftRec_zero, BitVec.and_twoPow]
    split
    · next h => rw [BitVec.shiftLeft_eq', shift_amt _ _ h]
    · rw [BitVec.shiftLeft_eq']; simp
  · intro α n
    rw [BitVec.shiftLeftRec_succ, BitVec.and_twoPow]
    split
    · next h => rw [BitVec.shiftLeft_eq', shift_amt _ _ h]
    · rw [BitVec.shiftLeft_eq']; simp
  · intro α _
    show toBits _ = toBits (X α <<< Y α)
    rw [BitVec.shiftLeft_eq_shiftLeftRec]

def lshrOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  barrel (lshrW w) bs (w - 1) as c

theorem lshrOp_spec {w : Nat} : Op2 (lshrOp w) (fun x y : BitVec w => x >>> y) := by
  intro c as bs X Y ha hb
  refine (barrel_spec (lshrW w) (fun z k => z >>> k) (fun _ _ _ k h => lshrW_rep h k) bs X Y
    (fun α n => BitVec.ushiftRightRec (X α) (Y α) n) ?_ ?_ (w - 1) c as ha hb).congr ?_
  · intro α
    rw [BitVec.ushiftRightRec_zero, BitVec.and_twoPow]
    split
    · next h => rw [BitVec.ushiftRight_eq', shift_amt _ _ h]
    · rw [BitVec.ushiftRight_eq']; simp
  · intro α n
    rw [BitVec.ushiftRightRec_succ, BitVec.and_twoPow]
    split
    · next h => rw [BitVec.ushiftRight_eq', shift_amt _ _ h]
    · rw [BitVec.ushiftRight_eq']; simp
  · intro α _
    show toBits _ = toBits (X α >>> Y α)
    rw [BitVec.shiftRight_eq_ushiftRightRec]

def ashrOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  barrel (ashrW w) bs (w - 1) as c

theorem ashrOp_spec {w : Nat} : Op2 (ashrOp w) (fun x y : BitVec w => x.sshiftRight' y) := by
  intro c as bs X Y ha hb
  refine (barrel_spec (ashrW w) (fun z k => z.sshiftRight k) (fun _ _ _ k h => ashrW_rep h k)
    bs X Y (fun α n => BitVec.sshiftRightRec (X α) (Y α) n) ?_ ?_ (w - 1) c as ha hb).congr ?_
  · intro α
    rw [BitVec.sshiftRightRec_zero_eq, BitVec.and_twoPow]
    split
    · next h => simp only [BitVec.sshiftRight']; rw [shift_amt _ _ h]
    · simp [BitVec.sshiftRight']
  · intro α n
    rw [BitVec.sshiftRightRec_succ_eq, BitVec.and_twoPow]
    split
    · next h => simp only [BitVec.sshiftRight']; rw [shift_amt _ _ h]
    · simp [BitVec.sshiftRight']
  · intro α _
    simp only [toBits]
    apply List.map_congr_left
    intro i _
    rw [BitVec.sshiftRight_eq_sshiftRightRec]

/-! ## Overflow predicates -/

def uaddoOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let r := ripple as bs FF c
  ([r.1.getLastD FF], r.2)

theorem uaddoOp_spec {w : Nat} :
    Op2 uaddoOp (fun x y : BitVec w => BitVec.ofBool (x.uaddOverflow y)) := by
  intro c as bs X Y ha hb
  have h := ripple_rep ha hb (scoped_FF _) (C := fun _ => false) (fun α hc => val_FF hc)
  refine h.trans (rep_single h.wf (good_getLastD h.good) (B := fun α => (X α).uaddOverflow (Y α))
    (fun α hc => ?_)).spec
  rw [val_getLastD hc, h.sem α hc, rippleSem_last, uaddOverflow_carry]

/-- Signed addition overflow: equal operand signs and a sum sign that differs. -/
def saddoOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let s := addOp as bs c
  let e1 := mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) s.2
  let e2 := mkGate (.xor (s.1.getLastD FF) (as.getLastD FF)) e1.2
  let o := mkGate (.and e1.1.neg e2.1) e2.2
  ([o.1], o.2)

/-- Signed subtraction overflow: different operand signs and a difference
whose sign differs from the minuend's. -/
def ssuboOp (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let s := subOp as bs c
  let e1 := mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) s.2
  let e2 := mkGate (.xor (s.1.getLastD FF) (as.getLastD FF)) e1.2
  let o := mkGate (.and e1.1 e2.1) e2.2
  ([o.1], o.2)

/-- The three-gate sign test shared by `saddoOp` and `ssuboOp`. -/
theorem signTest_spec' {w : Nat} (neg1 : Bool) (op : List Lit → List Lit → Circuit → List Lit × Circuit)
    (F : BitVec w → BitVec w → BitVec w) (hop : Op2 op F) (P : BitVec w → BitVec w → Bool)
    (hP : ∀ x y, P x y = (((x.msb ^^ y.msb) ^^ neg1) && ((F x y).msb ^^ x.msb)))
    (c : Circuit) (as bs : List Lit) (X Y : (Nat → Bool) → BitVec w)
    (ha : Rep c as X) (hb : Rep c bs Y) (s : List Lit × Circuit) (hs0 : s = op as bs c)
    (e1 : Lit × Circuit) (he1 : e1 = mkGate (.xor (as.getLastD FF) (bs.getLastD FF)) s.2)
    (e2 : Lit × Circuit) (he2 : e2 = mkGate (.xor (s.1.getLastD FF) (as.getLastD FF)) e1.2)
    (o : Lit × Circuit)
    (ho : o = mkGate (.and (if neg1 then e1.1.neg else e1.1) e2.1) e2.2) :
    Spec c ([o.1], o.2) (fun α => toBits (BitVec.ofBool (P (X α) (Y α)))) := by
  have hs : Spec c s (fun α => toBits (F (X α) (Y α))) := hs0 ▸ hop c as bs X Y ha hb
  have hS := hs.rep
  have ha' := ha.mono hs.suffix hs.wf
  have hb' := hb.mono hs.suffix hs.wf
  have h1 : Spec s.2 ([e1.1], e1.2)
      (fun α => [(as.getLastD FF).val α ^^ (bs.getLastD FF).val α]) := by
    subst he1
    exact mkGate_spec hS.wf ⟨good_getLastD ha'.good, good_getLastD hb'.good⟩
  have hl1 := Circuit.len_le h1.suffix hS.wf h1.wf
  have h2 : Spec e1.2 ([e2.1], e2.2)
      (fun α => [(s.1.getLastD FF).val α ^^ (as.getLastD FF).val α]) := by
    subst he2
    exact mkGate_spec h1.wf ⟨(good_getLastD hS.good).mono hl1, (good_getLastD ha'.good).mono hl1⟩
  have hl2 := Circuit.len_le h2.suffix h1.wf h2.wf
  have hsc1 : Scoped e2.2.len e1.1 := (h1.good _ (List.mem_singleton_self _)).mono hl2
  have h3 : Spec e2.2 ([o.1], o.2)
      (fun α => [(e1.1.val α ^^ neg1) && e2.1.val α]) := by
    subst ho
    refine (mkGate_spec (g := .and (if neg1 then e1.1.neg else e1.1) e2.1) h2.wf
      ⟨?_, h2.good _ (List.mem_singleton_self _)⟩).congr fun α _ => ?_
    · cases neg1
      · exact hsc1
      · exact hsc1
    · cases neg1 <;> simp [Gate.eval]
  refine (hs.trans (h1.trans (h2.trans h3))).congr fun α hc => ?_
  have hc2 := consistent_of_suffix h3.suffix hc
  have hc1 := consistent_of_suffix h2.suffix hc2
  have hc0 := consistent_of_suffix h1.suffix hc1
  rw [single_val h1 hc1, single_val h2 hc2, ha'.msb hc0, hb'.msb hc0, hS.msb hc0, hP,
    toBits_ofBool]

theorem saddoOp_spec {w : Nat} :
    Op2 saddoOp (fun x y : BitVec w => BitVec.ofBool (x.saddOverflow y)) := by
  intro c as bs X Y ha hb
  exact signTest_spec' true addOp (fun x y => x + y) addOp_spec
    (fun x y => x.saddOverflow y) (fun x y => by
      rw [BitVec.saddOverflow_eq]
      cases x.msb <;> cases y.msb <;> cases (x + y).msb <;> rfl) c as bs X Y ha hb
    _ rfl _ rfl _ rfl _ rfl

theorem ssuboOp_spec {w : Nat} :
    Op2 ssuboOp (fun x y : BitVec w => BitVec.ofBool (x.ssubOverflow y)) := by
  intro c as bs X Y ha hb
  exact signTest_spec' false subOp (fun x y => x - y) subOp_spec
    (fun x y => x.ssubOverflow y) (fun x y => by
      rw [BitVec.ssubOverflow_eq]
      cases x.msb <;> cases y.msb <;> cases (x - y).msb <;> rfl) c as bs X Y ha hb
    _ rfl _ rfl _ rfl _ rfl

theorem usuboOp_spec {w : Nat} :
    Op2 ultOp (fun x y : BitVec w => BitVec.ofBool (x.usubOverflow y)) := by
  intro c as bs X Y ha hb
  exact ultOp_spec c as bs X Y ha hb

/-- Unsigned multiplication overflow: the double-width product reaches `2^w`. -/
def umuloOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  if w = 0 then ([FF], c) else
  let p := mulOp (w * 2) (zextW (w * 2) as) (zextW (w * 2) bs) c
  uleOp (constLits (BitVec.twoPow (w * 2) w)) p.1 p.2

theorem umuloOp_spec {w : Nat} :
    Op2 (umuloOp w) (fun x y : BitVec w => BitVec.ofBool (x.umulOverflow y)) := by
  intro c as bs X Y ha hb
  by_cases hw : w = 0
  · subst hw
    simp only [umuloOp, ite_true]
    refine (rep_single ha.wf (scoped_FF _) (B := fun _ => false) (fun α hc => val_FF hc)).spec.congr
      fun α _ => ?_
    rw [BitVec.umulOverflow_eq]
    simp
  · simp only [umuloOp, hw, ite_false]
    have hp := mulOp_spec c _ _ _ _ (zextW_rep ha (w * 2)) (zextW_rep hb (w * 2))
    have hu := uleOp_spec _ _ _ _ _ (rep_const hp.wf (BitVec.twoPow (w * 2) w)) hp.rep
    refine (hp.trans hu).congr fun α _ => ?_
    simp only [BitVec.umulOverflow_eq, BitVec.ule_eq_decide_le, BitVec.zeroExtend,
      Nat.pos_of_ne_zero hw, decide_true, Bool.true_and]

/-- Signed multiplication overflow, upper half: `2^(w-1) ≤ x.toInt * y.toInt`. -/
def smulHi {w : Nat} (x y : BitVec w) : Bool := decide (2 ^ (w - 1) ≤ x.toInt * y.toInt)

/-- Signed multiplication overflow, lower half: `x.toInt * y.toInt < -2^(w-1)`. -/
def smulLo {w : Nat} (x y : BitVec w) : Bool := decide (x.toInt * y.toInt < - 2 ^ (w - 1))

theorem smulOverflow_hi_lo {w : Nat} (x y : BitVec w) :
    x.smulOverflow y = (smulHi x y || smulLo x y) := by
  simp [BitVec.smulOverflow, smulHi, smulLo, ge_iff_le]

def smulProd (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  mulOp (w * 2) (sextW w (w * 2) as) (sextW w (w * 2) bs) c

def smulHiOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let p := smulProd w as bs c
  sltOp (constLits ((BitVec.intMax w).signExtend (w * 2))) p.1 p.2

def smulLoOp (w : Nat) (as bs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let p := smulProd w as bs c
  sltOp p.1 (constLits ((BitVec.intMin w).signExtend (w * 2))) p.2

theorem smulHiOp_spec {w : Nat} :
    Op2 (smulHiOp w) (fun x y : BitVec w => BitVec.ofBool (smulHi x y)) := by
  intro c as bs X Y ha hb
  have hp := mulOp_spec c _ _ _ _ (sextW_rep ha (w * 2)) (sextW_rep hb (w * 2))
  have hs := sltOp_spec _ _ _ _ _ (rep_const hp.wf ((BitVec.intMax w).signExtend (w * 2))) hp.rep
  refine (hp.trans hs).congr fun α _ => ?_
  show toBits (BitVec.ofBool _) = toBits (BitVec.ofBool (smulHi (X α) (Y α)))
  simp only [smulHi, BitVec.two_pow_le_toInt_mul_toInt_iff, Bool.decide_eq_true]

theorem smulLoOp_spec {w : Nat} :
    Op2 (smulLoOp w) (fun x y : BitVec w => BitVec.ofBool (smulLo x y)) := by
  intro c as bs X Y ha hb
  have hp := mulOp_spec c _ _ _ _ (sextW_rep ha (w * 2)) (sextW_rep hb (w * 2))
  have hs := sltOp_spec _ _ _ _ _ hp.rep (rep_const hp.wf ((BitVec.intMin w).signExtend (w * 2)))
  refine (hp.trans hs).congr fun α _ => ?_
  show toBits (BitVec.ofBool _) = toBits (BitVec.ofBool (smulLo (X α) (Y α)))
  simp only [smulLo, BitVec.toInt_mul_toInt_lt_neg_two_pow_iff, Bool.decide_eq_true]

end PrismTechniques.Bitblast
