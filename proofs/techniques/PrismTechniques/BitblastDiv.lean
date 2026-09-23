/-
PRISM techniques: the bit-blaster's divider (roadmap 5.4 / 8.2).

A restoring divider that follows core Lean's division recurrence
`BitVec.divRec` / `BitVec.divSubtractShift` step for step, so its correctness
reduces to core's `BitVec.udiv_eq_divRec` and `BitVec.umod_eq_divRec`.  Each
step shifts the next dividend bit into the partial remainder `r'`, runs one
subtractor `r' + ~~~d + 1` whose carry out is `d ≤ᵤ r'` (the next quotient
bit) and whose sum is `r' - d`, and keeps `r' - d` or `r'` with a multiplexer.

Division by zero follows SMT-LIB (the semantics Z3 and the PIR encoder use):
`bvudiv x 0 = ~0` and `bvurem x 0 = x` (`smtUdiv`, and core's `x % 0 = x`).
Signed division and remainder are SMT-LIB's `bvsdiv` / `bvsrem`, defined by
cases on the operand signs (`smtSdiv`, `smtSrem`).
-/
import PrismTechniques.BitblastOps

namespace PrismTechniques.Bitblast

open Std.Sat

/-! ## SMT-LIB division semantics -/

/-- SMT-LIB `bvudiv`: division by zero yields all ones. -/
def smtUdiv {w : Nat} (x y : BitVec w) : BitVec w := if y = 0#w then BitVec.allOnes w else x / y

/-- SMT-LIB `bvsdiv`, by the standard's four sign cases. -/
def smtSdiv {w : Nat} (x y : BitVec w) : BitVec w :=
  match x.msb, y.msb with
  | false, false => smtUdiv x y
  | true, false => -(smtUdiv (-x) y)
  | false, true => -(smtUdiv x (-y))
  | true, true => smtUdiv (-x) (-y)

/-- SMT-LIB `bvsrem`, by the standard's four sign cases (`bvurem` is core's
`%`, which already maps `x % 0` to `x`). -/
def smtSrem {w : Nat} (x y : BitVec w) : BitVec w :=
  match x.msb, y.msb with
  | false, false => x % y
  | true, false => -((-x) % y)
  | false, true => x % (-y)
  | true, true => -((-x) % (-y))

theorem smtSdiv_eq {w : Nat} (x y : BitVec w) :
    smtSdiv x y =
      let a := if x.msb then -x else x
      let b := if y.msb then -y else y
      if x.msb ^^ y.msb then -(smtUdiv a b) else smtUdiv a b := by
  simp only [smtSdiv]
  cases x.msb <;> cases y.msb <;> rfl

theorem smtSrem_eq {w : Nat} (x y : BitVec w) :
    smtSrem x y =
      let a := if x.msb then -x else x
      let b := if y.msb then -y else y
      if x.msb then -(a % b) else a % b := by
  simp only [smtSrem]
  cases x.msb <;> cases y.msb <;> rfl

/-! ## One shift-subtract step -/

def shiftConcatW (w : Nat) (b : Lit) (ls : List Lit) : List Lit :=
  (List.range w).map fun i => if i = 0 then b else ls.getD (i - 1) FF

theorem shiftConcatW_rep {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    {b : Lit} {B : (Nat → Bool) → Bool} (h : Rep c ls X) (hb : Scoped c.len b)
    (hB : ∀ α, Consistent α c → b.val α = B α) :
    Rep c (shiftConcatW w b ls) (fun α => (X α).shiftConcat (B α)) :=
  (wire_spec _ _ h.wf (fun i _ => by split; exact hb; exact good_getD h.good _)
    (fun α hc i hi => by
      rw [BitVec.getLsbD_shiftConcat]
      by_cases h0 : i = 0
      · subst h0; simp only [↓reduceIte]; rw [hB α hc]; simp [hi]
      · simp only [h0, ↓reduceIte]; rw [h.bit hc]; simp [hi])).rep

theorem divSubtractShift_eq {w : Nat} (n d : BitVec w) (qr : BitVec.DivModState w) :
    BitVec.divSubtractShift ⟨n, d⟩ qr =
      { wn := qr.wn - 1, wr := qr.wr + 1,
        q := qr.q.shiftConcat (d.ule (qr.r.shiftConcat (n.getLsbD (qr.wn - 1)))),
        r := if d.ule (qr.r.shiftConcat (n.getLsbD (qr.wn - 1)))
          then qr.r.shiftConcat (n.getLsbD (qr.wn - 1)) - d
          else qr.r.shiftConcat (n.getLsbD (qr.wn - 1)) } := by
  simp only [BitVec.divSubtractShift, BitVec.ule_eq_decide_le]
  by_cases h : qr.r.shiftConcat (n.getLsbD (qr.wn - 1)) < d
  · have : ¬ d ≤ qr.r.shiftConcat (n.getLsbD (qr.wn - 1)) := BitVec.not_le.2 h
    simp [h, this]
  · have : d ≤ qr.r.shiftConcat (n.getLsbD (qr.wn - 1)) := BitVec.not_lt.1 h
    simp [h, this]

def divStep (w wn : Nat) (ns ds qs rs : List Lit) (c : Circuit) :
    (List Lit × List Lit) × Circuit :=
  let r' := shiftConcatW w (ns.getD (wn - 1) FF) rs
  let rr := ripple r' (ds.map Lit.neg) TT c
  let cy := rr.1.getLastD FF
  let m := muxEnc cy rr.1.dropLast r' rr.2
  ((shiftConcatW w cy qs, m.1), m.2)

/-- What a two-output builder guarantees. -/
structure Spec2 {w : Nat} (c : Circuit) (o : (List Lit × List Lit) × Circuit)
    (Q R : (Nat → Bool) → BitVec w) : Prop where
  suffix : c.gates <:+ o.2.gates
  q : Rep o.2 o.1.1 Q
  r : Rep o.2 o.1.2 R

theorem divStep_spec {w : Nat} (wn : Nat) (ns ds qs rs : List Lit) (c : Circuit)
    (N D : (Nat → Bool) → BitVec w) (S : (Nat → Bool) → BitVec.DivModState w)
    (hwn : ∀ α, (S α).wn = wn) (hn : Rep c ns N) (hd : Rep c ds D)
    (hq : Rep c qs (fun α => (S α).q)) (hr : Rep c rs (fun α => (S α).r)) :
    Spec2 c (divStep w wn ns ds qs rs c)
      (fun α => (BitVec.divSubtractShift ⟨N α, D α⟩ (S α)).q)
      (fun α => (BitVec.divSubtractShift ⟨N α, D α⟩ (S α)).r) := by
  have hr' := shiftConcatW_rep (w := w) hr (good_getD hn.good (wn - 1))
    (B := fun α => (N α).getLsbD ((S α).wn - 1)) (fun α hc => by rw [hn.bit hc, hwn])
  have hrr := ripple_rep hr' (notOp_rep hd) (scoped_TT _) (C := fun _ => true)
    (fun α hc => val_TT hc)
  have hcy : ∀ α, Consistent α (ripple (shiftConcatW w (ns.getD (wn - 1) FF) rs)
      (ds.map Lit.neg) TT c).2 →
      ((ripple (shiftConcatW w (ns.getD (wn - 1) FF) rs) (ds.map Lit.neg) TT c).1.getLastD FF).val α
        = (D α).ule (((S α).r).shiftConcat ((N α).getLsbD ((S α).wn - 1))) := by
    intro α hc
    rw [val_getLastD hc, hrr.sem α hc, ule_bits]
  have hdiff : Rep (ripple (shiftConcatW w (ns.getD (wn - 1) FF) rs) (ds.map Lit.neg) TT c).2
      (ripple (shiftConcatW w (ns.getD (wn - 1) FF) rs) (ds.map Lit.neg) TT c).1.dropLast
      (fun α => ((S α).r).shiftConcat ((N α).getLsbD ((S α).wn - 1)) - D α) :=
    ⟨hrr.wf, good_dropLast hrr.good, fun α hc => by
      rw [List.map_dropLast, hrr.sem α hc, toBits_sub]⟩
  have hr'' := hr'.mono hrr.suffix hrr.wf
  have hm := mux_rep (good_getLastD hrr.good) hcy hdiff hr''
  have hq' := shiftConcatW_rep (w := w) ((hq.mono hrr.suffix hrr.wf).mono hm.suffix hm.wf)
    ((good_getLastD hrr.good).mono (Circuit.len_le hm.suffix hrr.wf hm.wf))
    (B := fun α => (D α).ule (((S α).r).shiftConcat ((N α).getLsbD ((S α).wn - 1))))
    (fun α hc => hcy α (consistent_of_suffix hm.suffix hc))
  refine ⟨hrr.suffix.trans hm.suffix, ?_, ?_⟩
  · refine ⟨hq'.wf, hq'.good, fun α hc => ?_⟩
    simp only [divStep]
    rw [hq'.sem α hc, divSubtractShift_eq]
  · refine ⟨hm.wf, hm.good, fun α hc => ?_⟩
    simp only [divStep]
    rw [hm.sem α hc, divSubtractShift_eq]

def divLoop (w : Nat) (ns ds : List Lit) :
    Nat → Nat → List Lit → List Lit → Circuit → (List Lit × List Lit) × Circuit
  | 0, _, qs, rs, c => ((qs, rs), c)
  | m + 1, wn, qs, rs, c =>
    let st := divStep w wn ns ds qs rs c
    divLoop w ns ds m (wn - 1) st.1.1 st.1.2 st.2

theorem divLoop_spec {w : Nat} (ns ds : List Lit) (N D : (Nat → Bool) → BitVec w) :
    ∀ (m wn : Nat) (qs rs : List Lit) (c : Circuit) (S : (Nat → Bool) → BitVec.DivModState w),
      (∀ α, (S α).wn = wn) → Rep c ns N → Rep c ds D →
      Rep c qs (fun α => (S α).q) → Rep c rs (fun α => (S α).r) →
      Spec2 c (divLoop w ns ds m wn qs rs c)
        (fun α => (BitVec.divRec m ⟨N α, D α⟩ (S α)).q)
        (fun α => (BitVec.divRec m ⟨N α, D α⟩ (S α)).r)
  | 0, wn, qs, rs, c, S, _, _, _, hq, hr => ⟨List.suffix_refl _, hq, hr⟩
  | m + 1, wn, qs, rs, c, S, hwn, hn, hd, hq, hr => by
    have st := divStep_spec wn ns ds qs rs c N D S hwn hn hd hq hr
    have ih := divLoop_spec ns ds N D m (wn - 1) _ _ _
      (fun α => BitVec.divSubtractShift ⟨N α, D α⟩ (S α))
      (fun α => by rw [divSubtractShift_eq]; simp [hwn α])
      (hn.mono st.suffix st.r.wf) (hd.mono st.suffix st.r.wf) st.q st.r
    exact ⟨st.suffix.trans ih.suffix, ih.q, ih.r⟩

/-! ## Unsigned division and remainder -/

/-- The raw divider: `w` steps of `divStep` from `q = r = 0`, after testing `d = 0`. -/
def divCore (w : Nat) (ns ds : List Lit) (c : Circuit) :
    Lit × (List Lit × List Lit) × Circuit :=
  let z := eqOp ds (constLits (0#w)) c
  let st := divLoop w ns ds w w (constLits (0#w)) (constLits (0#w)) z.2
  (z.1.headD FF, st)

def udivOp (w : Nat) (ns ds : List Lit) (c : Circuit) : List Lit × Circuit :=
  let k := divCore w ns ds c
  muxEnc k.1 (constLits (BitVec.allOnes w)) k.2.1.1 k.2.2

def uremOp (w : Nat) (ns ds : List Lit) (c : Circuit) : List Lit × Circuit :=
  let k := divCore w ns ds c
  muxEnc k.1 ns k.2.1.2 k.2.2

theorem divCore_facts {w : Nat} (c : Circuit) (ns ds : List Lit) (N D : (Nat → Bool) → BitVec w)
    (hn : Rep c ns N) (hd : Rep c ds D) :
    let k := divCore w ns ds c
    c.gates <:+ k.2.2.gates ∧ Scoped k.2.2.len k.1 ∧
    (∀ α, Consistent α k.2.2 → k.1.val α = (D α == 0#w)) ∧
    Rep k.2.2 k.2.1.1 (fun α => (BitVec.divRec w ⟨N α, D α⟩ (BitVec.DivModState.init w)).q) ∧
    Rep k.2.2 k.2.1.2 (fun α => (BitVec.divRec w ⟨N α, D α⟩ (BitVec.DivModState.init w)).r) := by
  intro k
  have hz := eqOp_spec c ds (constLits (0#w)) D (fun _ => 0#w) hd (rep_const hd.wf _)
  have hz1 : ∀ α, Consistent α (eqOp ds (constLits (0#w)) c).2 →
      ((eqOp ds (constLits (0#w)) c).1.headD FF).val α = (D α == 0#w) := by
    intro α hc
    rw [val_headD hc, hz.sem α hc, toBits_ofBool]
    rfl
  have hl := divLoop_spec ns ds N D w w (constLits (0#w)) (constLits (0#w)) _
    (fun _ => BitVec.DivModState.init w) (fun _ => rfl) (hn.mono hz.suffix hz.wf)
    (hd.mono hz.suffix hz.wf) (rep_const hz.wf _) (rep_const hz.wf _)
  refine ⟨hz.suffix.trans hl.suffix, (good_headD hz.good).mono
    (Circuit.len_le hl.suffix hz.wf hl.q.wf), fun α hc => hz1 α (consistent_of_suffix hl.suffix hc),
    hl.q, hl.r⟩

theorem udiv_divRec {w : Nat} (n d : BitVec w) :
    smtUdiv n d = if (d == 0#w) = true then BitVec.allOnes w
      else (BitVec.divRec w ⟨n, d⟩ (BitVec.DivModState.init w)).q := by
  simp only [smtUdiv, beq_iff_eq]
  split
  · rfl
  · next h =>
    have hd : 0#w < d := by
      rw [BitVec.lt_def]
      have : d.toNat ≠ 0 := fun h' => h (BitVec.eq_of_toNat_eq (by simpa using h'))
      simp; omega
    exact BitVec.udiv_eq_divRec hd

theorem umod_divRec {w : Nat} (n d : BitVec w) :
    n % d = if (d == 0#w) = true then n
      else (BitVec.divRec w ⟨n, d⟩ (BitVec.DivModState.init w)).r := by
  simp only [beq_iff_eq]
  split
  · next h => subst h; exact BitVec.umod_zero
  · next h =>
    have hd : 0#w < d := by
      rw [BitVec.lt_def]
      have : d.toNat ≠ 0 := fun h' => h (BitVec.eq_of_toNat_eq (by simpa using h'))
      simp; omega
    exact BitVec.umod_eq_divRec hd

theorem udivOp_spec {w : Nat} : Op2 (udivOp w) (fun x y : BitVec w => smtUdiv x y) := by
  intro c ns ds N D hn hd
  obtain ⟨hsuf, hsc, hz, hq, _⟩ := divCore_facts c ns ds N D hn hd
  have hm := mux_rep hsc hz (rep_const hq.wf (BitVec.allOnes w)) hq
  refine ⟨hsuf.trans hm.suffix, hm.wf, hm.good, fun α hc => ?_⟩
  simp only [udivOp]
  rw [hm.sem α hc, udiv_divRec]

theorem uremOp_spec {w : Nat} : Op2 (uremOp w) (fun x y : BitVec w => x % y) := by
  intro c ns ds N D hn hd
  obtain ⟨hsuf, hsc, hz, _, hr⟩ := divCore_facts c ns ds N D hn hd
  have hm := mux_rep hsc hz (hn.mono hsuf hr.wf) hr
  refine ⟨hsuf.trans hm.suffix, hm.wf, hm.good, fun α hc => ?_⟩
  simp only [uremOp]
  rw [hm.sem α hc, umod_divRec]

/-! ## Signed division and remainder -/

/-- `|x|` as SMT-LIB's sign cases use it: `if msb then -x else x`. -/
def absOp (w : Nat) (xs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let n := negOp w xs c
  muxEnc (xs.getLastD FF) n.1 xs n.2

theorem absOp_spec {w : Nat} : Op1 (absOp w) (fun x : BitVec w => if x.msb then -x else x) := by
  intro c xs X hx
  have hn := negOp_spec c xs X hx
  have hx' := hx.mono hn.suffix hn.wf
  have hm := mux_rep (good_getLastD hx'.good) (fun α hc => hx'.msb hc) hn.rep hx'
  exact hn.trans hm

/-- Conditional negation: `if s then -x else x`. -/
def condNegOp (w : Nat) (s : Lit) (xs : List Lit) (c : Circuit) : List Lit × Circuit :=
  let n := negOp w xs c
  muxEnc s n.1 xs n.2

theorem condNegOp_spec {w : Nat} {c : Circuit} {s : Lit} {B : (Nat → Bool) → Bool}
    {xs : List Lit} {X : (Nat → Bool) → BitVec w} (hs : Scoped c.len s)
    (hB : ∀ α, Consistent α c → s.val α = B α) (hx : Rep c xs X) :
    Spec c (condNegOp w s xs c) (fun α => toBits (if B α then -X α else X α)) := by
  have hn := negOp_spec c xs X hx
  have hl := Circuit.len_le hn.suffix hx.wf hn.wf
  have hm := mux_rep (hs.mono hl) (fun α hc => hB α (consistent_of_suffix hn.suffix hc)) hn.rep
    (hx.mono hn.suffix hn.wf)
  exact hn.trans hm

def sdivOp (w : Nat) (xs ys : List Lit) (c : Circuit) : List Lit × Circuit :=
  let a := absOp w xs c
  let b := absOp w ys a.2
  let q := udivOp w a.1 b.1 b.2
  let s := mkGate (.xor (xs.getLastD FF) (ys.getLastD FF)) q.2
  condNegOp w s.1 q.1 s.2

def sremOp (w : Nat) (xs ys : List Lit) (c : Circuit) : List Lit × Circuit :=
  let a := absOp w xs c
  let b := absOp w ys a.2
  let r := uremOp w a.1 b.1 b.2
  condNegOp w (xs.getLastD FF) r.1 r.2

theorem sdivOp_spec {w : Nat} : Op2 (sdivOp w) (fun x y : BitVec w => smtSdiv x y) := by
  intro c xs ys X Y hx hy
  have ha := absOp_spec c xs X hx
  have hb := absOp_spec _ ys Y (hy.mono ha.suffix ha.wf)
  have hq := udivOp_spec _ _ _ _ _ (ha.rep.mono hb.suffix hb.wf) hb.rep
  have hsuf := ha.suffix.trans (hb.suffix.trans hq.suffix)
  have hx' := hx.mono hsuf hq.wf
  have hy' := hy.mono hsuf hq.wf
  have hs := mkGate_rep (g := .xor (xs.getLastD FF) (ys.getLastD FF))
    (B := fun α => (X α).msb ^^ (Y α).msb) hq.wf ⟨good_getLastD hx'.good, good_getLastD hy'.good⟩
    (fun α hc => by
      have hc0 := consistent_of_suffix (mkGate_suffix _ _) hc
      simp only [Gate.eval]; rw [hx'.msb hc0, hy'.msb hc0])
  have hn := condNegOp_spec (w := w) (mkGate_scoped _ _)
    (B := fun α => (X α).msb ^^ (Y α).msb)
    (fun α hc => by simpa [toBits_ofBool] using hs.sem α hc)
    (hq.rep.mono (mkGate_suffix _ _) hs.wf)
  refine Spec.congr (s := fun α => toBits (if (X α).msb ^^ (Y α).msb
      then -(smtUdiv (if (X α).msb then -X α else X α) (if (Y α).msb then -Y α else Y α))
      else smtUdiv (if (X α).msb then -X α else X α) (if (Y α).msb then -Y α else Y α)))
    (ha.trans (hb.trans (hq.trans ?_))) fun α _ => ?_
  · exact ⟨(mkGate_suffix _ _).trans hn.suffix, hn.wf, hn.good, hn.sem⟩
  · simp only [smtSdiv_eq]

theorem sremOp_spec {w : Nat} : Op2 (sremOp w) (fun x y : BitVec w => smtSrem x y) := by
  intro c xs ys X Y hx hy
  have ha := absOp_spec c xs X hx
  have hb := absOp_spec _ ys Y (hy.mono ha.suffix ha.wf)
  have hr := uremOp_spec _ _ _ _ _ (ha.rep.mono hb.suffix hb.wf) hb.rep
  have hsuf := ha.suffix.trans (hb.suffix.trans hr.suffix)
  have hx' := hx.mono hsuf hr.wf
  have hn := condNegOp_spec (w := w) (good_getLastD hx'.good) (B := fun α => (X α).msb)
    (fun α hc => hx'.msb hc) hr.rep
  refine Spec.congr (s := fun α => toBits (if (X α).msb
      then -((if (X α).msb then -X α else X α) % (if (Y α).msb then -Y α else Y α))
      else (if (X α).msb then -X α else X α) % (if (Y α).msb then -Y α else Y α)))
    (ha.trans (hb.trans (hr.trans hn))) fun α _ => ?_
  simp only [smtSrem_eq]

end PrismTechniques.Bitblast
