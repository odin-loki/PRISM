/-
PRISM techniques: the bit-blaster's expression language, encoder, Tseitin
CNF and main theorems (roadmap 3.2, 5.4, 8.2 "Bit-blaster").

`BVExpr` is PRISM's bit-blastable QF_BV fragment: every operator the PIR
verification conditions use (`add sub neg mul`, bitwise `not and or xor`,
shifts by a constant and by a variable, `udiv urem sdiv srem` with SMT-LIB
division by zero, `zext sext extract concat`, `ite`, the comparisons
`eq ult ule slt sle`, and the overflow predicates `uaddo saddo usubo ssubo
umulo` plus the two halves of `smulo`), at any width per variable.

`Dag` adds sharing: a list of definitions `v_i := e_i` (each `e_i` reading
only inputs and earlier definitions) and a top formula.  This is how the C++
serializer writes a Z3 term DAG without blowing it up into a tree.

Proved (no unfinished proofs, standard axioms only):
* `encode_spec`: the circuit computes every operator exactly;
* `toCNF_equisat`: `toCNF φ` is satisfiable iff `φ` is;
* `certified_unsat`: a certificate accepted by core Lean's verified LRAT
  checker for `toCNF φ` proves `φ` unsatisfiable;
* `dag_sat_imp`, `certified_dag_unsat`, `checkDag_sound`: the same for a DAG,
  whose semantics (`Dag.eval`) evaluates the definitions in order.
-/
import PrismTechniques.BitblastDiv

namespace PrismTechniques.Bitblast

open Std.Sat

/-! ## Bitvector expressions -/

/-- PRISM's bit-blastable bitvector fragment. -/
inductive BVExpr : Nat → Type
  | var {w : Nat} (base : Nat) : BVExpr w
  | const {w : Nat} (v : BitVec w) : BVExpr w
  | not {w : Nat} (e : BVExpr w) : BVExpr w
  | and {w : Nat} (a b : BVExpr w) : BVExpr w
  | or {w : Nat} (a b : BVExpr w) : BVExpr w
  | xor {w : Nat} (a b : BVExpr w) : BVExpr w
  | add {w : Nat} (a b : BVExpr w) : BVExpr w
  | ite {w : Nat} (c : BVExpr 1) (a b : BVExpr w) : BVExpr w
  | eq {w : Nat} (a b : BVExpr w) : BVExpr 1
  | ult {w : Nat} (a b : BVExpr w) : BVExpr 1
  | slt {w : Nat} (a b : BVExpr w) : BVExpr 1
  | mul {w : Nat} (a b : BVExpr w) : BVExpr w
  | sub {w : Nat} (a b : BVExpr w) : BVExpr w
  | neg {w : Nat} (e : BVExpr w) : BVExpr w
  | ule {w : Nat} (a b : BVExpr w) : BVExpr 1
  | sle {w : Nat} (a b : BVExpr w) : BVExpr 1
  | shl {w : Nat} (a b : BVExpr w) : BVExpr w
  | lshr {w : Nat} (a b : BVExpr w) : BVExpr w
  | ashr {w : Nat} (a b : BVExpr w) : BVExpr w
  | shlC {w : Nat} (k : Nat) (a : BVExpr w) : BVExpr w
  | lshrC {w : Nat} (k : Nat) (a : BVExpr w) : BVExpr w
  | ashrC {w : Nat} (k : Nat) (a : BVExpr w) : BVExpr w
  | zext {w : Nat} (n : Nat) (a : BVExpr w) : BVExpr n
  | sext {w : Nat} (n : Nat) (a : BVExpr w) : BVExpr n
  | extract {w : Nat} (lo len : Nat) (a : BVExpr w) : BVExpr len
  | concat {w v : Nat} (a : BVExpr w) (b : BVExpr v) : BVExpr (w + v)
  | uaddo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | saddo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | usubo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | ssubo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | umulo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | smulHi {w : Nat} (a b : BVExpr w) : BVExpr 1
  | smulLo {w : Nat} (a b : BVExpr w) : BVExpr 1
  | udiv {w : Nat} (a b : BVExpr w) : BVExpr w
  | urem {w : Nat} (a b : BVExpr w) : BVExpr w
  | sdiv {w : Nat} (a b : BVExpr w) : BVExpr w
  | srem {w : Nat} (a b : BVExpr w) : BVExpr w

/-- Semantics under an assignment `ρ` of the input bits; each operator is the
core Lean `BitVec` operation (SMT-LIB's, for division by zero). -/
def BVExpr.denote (ρ : Nat → Bool) : {w : Nat} → BVExpr w → BitVec w
  | w, .var base => bvOf w (fun i => ρ (base + i))
  | _, .const v => v
  | _, .not e => ~~~(e.denote ρ)
  | _, .and a b => a.denote ρ &&& b.denote ρ
  | _, .or a b => a.denote ρ ||| b.denote ρ
  | _, .xor a b => a.denote ρ ^^^ b.denote ρ
  | _, .add a b => a.denote ρ + b.denote ρ
  | _, .ite c a b => if (c.denote ρ).getLsbD 0 then a.denote ρ else b.denote ρ
  | _, .eq a b => BitVec.ofBool (a.denote ρ == b.denote ρ)
  | _, .ult a b => BitVec.ofBool ((a.denote ρ).ult (b.denote ρ))
  | _, .slt a b => BitVec.ofBool ((a.denote ρ).slt (b.denote ρ))
  | _, .mul a b => a.denote ρ * b.denote ρ
  | _, .sub a b => a.denote ρ - b.denote ρ
  | _, .neg e => -(e.denote ρ)
  | _, .ule a b => BitVec.ofBool ((a.denote ρ).ule (b.denote ρ))
  | _, .sle a b => BitVec.ofBool ((a.denote ρ).sle (b.denote ρ))
  | _, .shl a b => a.denote ρ <<< b.denote ρ
  | _, .lshr a b => a.denote ρ >>> b.denote ρ
  | _, .ashr a b => (a.denote ρ).sshiftRight' (b.denote ρ)
  | _, .shlC k a => a.denote ρ <<< k
  | _, .lshrC k a => a.denote ρ >>> k
  | _, .ashrC k a => (a.denote ρ).sshiftRight k
  | _, .zext n a => (a.denote ρ).setWidth n
  | _, .sext n a => (a.denote ρ).signExtend n
  | _, .extract lo len a => (a.denote ρ).extractLsb' lo len
  | _, .concat a b => a.denote ρ ++ b.denote ρ
  | _, .uaddo a b => BitVec.ofBool ((a.denote ρ).uaddOverflow (b.denote ρ))
  | _, .saddo a b => BitVec.ofBool ((a.denote ρ).saddOverflow (b.denote ρ))
  | _, .usubo a b => BitVec.ofBool ((a.denote ρ).usubOverflow (b.denote ρ))
  | _, .ssubo a b => BitVec.ofBool ((a.denote ρ).ssubOverflow (b.denote ρ))
  | _, .umulo a b => BitVec.ofBool ((a.denote ρ).umulOverflow (b.denote ρ))
  | _, .smulHi a b => BitVec.ofBool (Bitblast.smulHi (a.denote ρ) (b.denote ρ))
  | _, .smulLo a b => BitVec.ofBool (Bitblast.smulLo (a.denote ρ) (b.denote ρ))
  | _, .udiv a b => smtUdiv (a.denote ρ) (b.denote ρ)
  | _, .urem a b => a.denote ρ % b.denote ρ
  | _, .sdiv a b => smtSdiv (a.denote ρ) (b.denote ρ)
  | _, .srem a b => smtSrem (a.denote ρ) (b.denote ρ)

/-- A 1-bit formula is satisfiable when some input makes it `1`. -/
def FSat (φ : BVExpr 1) : Prop := ∃ ρ, φ.denote ρ = 1#1

/-- Signed multiplication overflow is the disjunction of the two halves. -/
theorem smulOverflow_expr {w : Nat} (a b : BVExpr w) (ρ : Nat → Bool) :
    BitVec.ofBool ((a.denote ρ).smulOverflow (b.denote ρ)) =
      (BVExpr.or (.smulHi a b) (.smulLo a b)).denote ρ := by
  simp only [BVExpr.denote, smulOverflow_hi_lo]
  cases smulHi (a.denote ρ) (b.denote ρ) <;> cases smulLo (a.denote ρ) (b.denote ρ) <;> rfl

/-! ## The encoder -/

def varLits (w base : Nat) : List Lit := (List.range w).map fun i => (2 * (base + i), true)

def wireOp (f : List Lit → List Lit) (ls : List Lit) (c : Circuit) : List Lit × Circuit := (f ls, c)

/-- Bit-blast an expression: output literals (least significant first) and
the extended circuit. -/
def encode : {w : Nat} → BVExpr w → Circuit → List Lit × Circuit
  | w, .var base, c => (varLits w base, c)
  | _, .const v, c => (constLits v, c)
  | _, .not e, c => let r := encode e c; notOp r.1 r.2
  | _, .and a b, c => let r1 := encode a c; let r2 := encode b r1.2; andOp r1.1 r2.1 r2.2
  | _, .or a b, c => let r1 := encode a c; let r2 := encode b r1.2; orOp r1.1 r2.1 r2.2
  | _, .xor a b, c => let r1 := encode a c; let r2 := encode b r1.2; xorOp r1.1 r2.1 r2.2
  | _, .add a b, c => let r1 := encode a c; let r2 := encode b r1.2; addOp r1.1 r2.1 r2.2
  | _, .ite s a b, c =>
    let r0 := encode s c
    let r1 := encode a r0.2
    let r2 := encode b r1.2
    muxEnc (r0.1.headD FF) r1.1 r2.1 r2.2
  | _, .eq a b, c => let r1 := encode a c; let r2 := encode b r1.2; eqOp r1.1 r2.1 r2.2
  | _, .ult a b, c => let r1 := encode a c; let r2 := encode b r1.2; ultOp r1.1 r2.1 r2.2
  | _, .slt a b, c => let r1 := encode a c; let r2 := encode b r1.2; sltOp r1.1 r2.1 r2.2
  | w, .mul a b, c => let r1 := encode a c; let r2 := encode b r1.2; mulOp w r1.1 r2.1 r2.2
  | _, .sub a b, c => let r1 := encode a c; let r2 := encode b r1.2; subOp r1.1 r2.1 r2.2
  | w, .neg e, c => let r := encode e c; negOp w r.1 r.2
  | _, .ule a b, c => let r1 := encode a c; let r2 := encode b r1.2; uleOp r1.1 r2.1 r2.2
  | _, .sle a b, c => let r1 := encode a c; let r2 := encode b r1.2; sleOp r1.1 r2.1 r2.2
  | w, .shl a b, c => let r1 := encode a c; let r2 := encode b r1.2; shlOp w r1.1 r2.1 r2.2
  | w, .lshr a b, c => let r1 := encode a c; let r2 := encode b r1.2; lshrOp w r1.1 r2.1 r2.2
  | w, .ashr a b, c => let r1 := encode a c; let r2 := encode b r1.2; ashrOp w r1.1 r2.1 r2.2
  | w, .shlC k a, c => let r := encode a c; wireOp (shlW w k) r.1 r.2
  | w, .lshrC k a, c => let r := encode a c; wireOp (lshrW w k) r.1 r.2
  | w, .ashrC k a, c => let r := encode a c; wireOp (ashrW w k) r.1 r.2
  | _, .zext n a, c => let r := encode a c; wireOp (zextW n) r.1 r.2
  | _, @BVExpr.sext w n a, c => let r := encode a c; wireOp (sextW w n) r.1 r.2
  | _, .extract lo len a, c => let r := encode a c; wireOp (extractW lo len) r.1 r.2
  | _, .concat a b, c => let r1 := encode a c; let r2 := encode b r1.2; concatOp r1.1 r2.1 r2.2
  | _, .uaddo a b, c => let r1 := encode a c; let r2 := encode b r1.2; uaddoOp r1.1 r2.1 r2.2
  | _, .saddo a b, c => let r1 := encode a c; let r2 := encode b r1.2; saddoOp r1.1 r2.1 r2.2
  | _, .usubo a b, c => let r1 := encode a c; let r2 := encode b r1.2; ultOp r1.1 r2.1 r2.2
  | _, .ssubo a b, c => let r1 := encode a c; let r2 := encode b r1.2; ssuboOp r1.1 r2.1 r2.2
  | _, @BVExpr.umulo w a b, c =>
    let r1 := encode a c; let r2 := encode b r1.2; umuloOp w r1.1 r2.1 r2.2
  | _, @BVExpr.smulHi w a b, c =>
    let r1 := encode a c; let r2 := encode b r1.2; smulHiOp w r1.1 r2.1 r2.2
  | _, @BVExpr.smulLo w a b, c =>
    let r1 := encode a c; let r2 := encode b r1.2; smulLoOp w r1.1 r2.1 r2.2
  | w, .udiv a b, c => let r1 := encode a c; let r2 := encode b r1.2; udivOp w r1.1 r2.1 r2.2
  | w, .urem a b, c => let r1 := encode a c; let r2 := encode b r1.2; uremOp w r1.1 r2.1 r2.2
  | w, .sdiv a b, c => let r1 := encode a c; let r2 := encode b r1.2; sdivOp w r1.1 r2.1 r2.2
  | w, .srem a b, c => let r1 := encode a c; let r2 := encode b r1.2; sremOp w r1.1 r2.1 r2.2

/-- The input assignment read back from a CNF assignment. -/
def inputOf (α : Nat → Bool) : Nat → Bool := fun j => α (2 * j)

/-- The encoder's statement for one expression. -/
abbrev ESpec {w : Nat} (e : BVExpr w) (c : Circuit) : Prop :=
  Spec c (encode e c) (fun α => toBits (e.denote (inputOf α)))

theorem case1 {w u : Nat} {op : List Lit → Circuit → List Lit × Circuit}
    {F : BitVec w → BitVec u} (hop : Op1 op F) {a : BVExpr w} {c : Circuit} (ha : ESpec a c) :
    Spec c (op (encode a c).1 (encode a c).2) (fun α => toBits (F (a.denote (inputOf α)))) :=
  ha.trans (hop _ _ _ ha.rep)

theorem case2 {w v u : Nat} {op : List Lit → List Lit → Circuit → List Lit × Circuit}
    {F : BitVec w → BitVec v → BitVec u} (hop : Op2 op F) {a : BVExpr w} {b : BVExpr v}
    {c : Circuit} (ha : ESpec a c) (hb : ESpec b (encode a c).2) :
    Spec c (op (encode a c).1 (encode b (encode a c).2).1 (encode b (encode a c).2).2)
      (fun α => toBits (F (a.denote (inputOf α)) (b.denote (inputOf α)))) :=
  ha.trans (hb.trans (hop _ _ _ _ _ (ha.rep.mono hb.suffix hb.wf) hb.rep))

theorem wireOp_spec {w u : Nat} (f : List Lit → List Lit) (F : BitVec w → BitVec u)
    (hf : ∀ (c : Circuit) (ls : List Lit) (X : (Nat → Bool) → BitVec w), Rep c ls X →
      Rep c (f ls) (fun α => F (X α))) : Op1 (wireOp f) F :=
  fun c ls X h => (hf c ls X h).spec

theorem Rep.head {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) {α : Nat → Bool} (hc : Consistent α c) :
    (ls.headD FF).val α = (X α).getLsbD 0 := by
  have : ls.headD FF = ls.getD 0 FF := by cases ls <;> rfl
  rw [this, h.bit hc]

/-- **Encoder correctness.**  Under every assignment consistent with the
produced circuit, the output literals are the bits of the expression's value
on the input bits read from that assignment. -/
theorem encode_spec : ∀ {w : Nat} (e : BVExpr w) (c : Circuit), c.WF → ESpec e c
  | w, .var base, c, hwf =>
    wire_spec (fun i => ((2 * (base + i), true) : Lit)) _ hwf (fun i _ => by left; simp)
      (fun α _ i hi => by
        show _ = (bvOf w (fun i => inputOf α (base + i))).getLsbD i
        rw [getLsbD_bvOf]
        simp [Lit.val, inputOf, hi])
  | _, .const v, c, hwf => (rep_const hwf v).spec
  | _, .not e, c, hwf => case1 notOp_spec (encode_spec e c hwf)
  | _, .and a b, c, hwf =>
    case2 andOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .or a b, c, hwf =>
    case2 orOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .xor a b, c, hwf =>
    case2 xorOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .add a b, c, hwf =>
    case2 addOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .ite s a b, c, hwf => by
    have hs := encode_spec s c hwf
    have ha := encode_spec a _ hs.wf
    have hb := encode_spec b _ ha.wf
    have hs' := (hs.rep.mono ha.suffix ha.wf).mono hb.suffix hb.wf
    have hm := mux_rep (good_headD hs'.good) (B := fun α => (s.denote (inputOf α)).getLsbD 0)
      (fun α hc => hs'.head hc) (ha.rep.mono hb.suffix hb.wf) hb.rep
    exact (hs.trans (ha.trans (hb.trans hm))).congr fun α _ => by
      simp only [BVExpr.denote]
  | _, .eq a b, c, hwf =>
    case2 eqOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .ult a b, c, hwf =>
    case2 ultOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .slt a b, c, hwf =>
    case2 sltOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .mul a b, c, hwf =>
    case2 mulOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .sub a b, c, hwf =>
    case2 subOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .neg e, c, hwf => case1 negOp_spec (encode_spec e c hwf)
  | _, .ule a b, c, hwf =>
    case2 uleOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .sle a b, c, hwf =>
    case2 sleOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .shl a b, c, hwf =>
    case2 shlOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .lshr a b, c, hwf =>
    case2 lshrOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .ashr a b, c, hwf =>
    case2 ashrOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | w, .shlC k a, c, hwf =>
    case1 (wireOp_spec (shlW w k) (F := fun x => x <<< k) (fun _ _ _ h => shlW_rep h k)) (encode_spec a c hwf)
  | w, .lshrC k a, c, hwf =>
    case1 (wireOp_spec (lshrW w k) (F := fun x => x >>> k) (fun _ _ _ h => lshrW_rep h k)) (encode_spec a c hwf)
  | w, .ashrC k a, c, hwf =>
    case1 (wireOp_spec (ashrW w k) (F := fun x => x.sshiftRight k) (fun _ _ _ h => ashrW_rep h k)) (encode_spec a c hwf)
  | _, .zext n a, c, hwf =>
    case1 (wireOp_spec (zextW n) (F := fun x => x.setWidth n) (fun _ _ _ h => zextW_rep h n))
      (encode_spec a c hwf)
  | _, @BVExpr.sext w n a, c, hwf =>
    case1 (wireOp_spec (sextW w n) (F := fun x => x.signExtend n) (fun _ _ _ h => sextW_rep h n))
      (encode_spec a c hwf)
  | _, .extract lo len a, c, hwf =>
    case1 (wireOp_spec (extractW lo len) (F := fun x => x.extractLsb' lo len)
      (fun _ _ _ h => extractW_rep h lo len)) (encode_spec a c hwf)
  | _, .concat a b, c, hwf =>
    case2 concatOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .uaddo a b, c, hwf =>
    case2 uaddoOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .saddo a b, c, hwf =>
    case2 saddoOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .usubo a b, c, hwf =>
    case2 usuboOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .ssubo a b, c, hwf =>
    case2 ssuboOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .umulo a b, c, hwf =>
    case2 umuloOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .smulHi a b, c, hwf =>
    case2 smulHiOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .smulLo a b, c, hwf =>
    case2 smulLoOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .udiv a b, c, hwf =>
    case2 udivOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .urem a b, c, hwf =>
    case2 uremOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .sdiv a b, c, hwf =>
    case2 sdivOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)
  | _, .srem a b, c, hwf =>
    case2 sremOp_spec (encode_spec a c hwf) (encode_spec b _ (encode_spec a c hwf).wf)

/-! ## CNF and the main theorems -/

/-- The Tseitin CNF of a 1-bit formula: all gate definitions, the constant,
and a unit clause asserting the output bit. -/
def toCNF (φ : BVExpr 1) : CNF Nat :=
  let r := encode φ Circuit.empty
  ⟨(clausesAcc r.2.len r.2.gates #[]).push [r.1.headD FF]⟩

theorem encode_wf (φ : BVExpr 1) : (encode φ Circuit.empty).2.WF :=
  (encode_spec φ _ Circuit.wf_empty).wf

theorem toCNF_eval (φ : BVExpr 1) (α : Nat → Bool) :
    (toCNF φ).eval α = true ↔
      Consistent α (encode φ Circuit.empty).2 ∧
        ((encode φ Circuit.empty).1.headD FF).val α = true := by
  have hw := encode_wf φ
  simp only [toCNF, CNF.eval]
  rw [← Array.all_toList, Array.toList_push, hw.1, clausesAcc_toList]
  simp only [List.nil_append, List.all_append, Bool.and_eq_true,
    clausesL_all_iff, List.all_cons, List.all_nil, Bool.and_true, CNF.Clause.eval_cons,
    CNF.Clause.eval_nil, Bool.or_false]
  rfl

theorem encode_top (φ : BVExpr 1) (α : Nat → Bool) (hc : Consistent α (encode φ Circuit.empty).2) :
    ((encode φ Circuit.empty).1.headD FF).val α = (φ.denote (inputOf α)).getLsbD 0 :=
  (encode_spec φ _ Circuit.wf_empty).rep.head hc

/-- **Completeness of the encoding**: a satisfying CNF assignment yields a
satisfying input for the formula. -/
theorem sat_of_cnf_sat (φ : BVExpr 1) (α : Nat → Bool) (h : (toCNF φ).Sat α) :
    φ.denote (inputOf α) = 1#1 := by
  obtain ⟨hc, hv⟩ := (toCNF_eval φ α).1 h
  rw [bv1_eq_one, ← encode_top φ α hc, hv]

/-- The canonical extension of an input assignment to the whole circuit. -/
def extend (ρ : Nat → Bool) (c : Circuit) : Nat → Bool := extendL ρ c.gates

/-- **Soundness of the encoding**: a satisfying input for the formula extends
to a satisfying CNF assignment. -/
theorem cnf_sat_of_sat (φ : BVExpr 1) (ρ : Nat → Bool) (h : φ.denote ρ = 1#1) :
    (toCNF φ).Sat (extend ρ (encode φ Circuit.empty).2) := by
  have hwf := encode_wf φ
  have hc : Consistent (extend ρ (encode φ Circuit.empty).2) (encode φ Circuit.empty).2 :=
    extendL_consistent ρ _ hwf.2
  refine (toCNF_eval φ _).2 ⟨hc, ?_⟩
  rw [encode_top φ _ hc]
  have : inputOf (extend ρ (encode φ Circuit.empty).2) = ρ := by
    funext j; exact extendL_input ρ j _
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

/-! ## Sharing: definitions and DAG formulas -/

/-- An upper bound on the input bits an expression reads. -/
def BVExpr.reads : {w : Nat} → BVExpr w → Nat
  | w, .var base => base + w
  | _, .const _ => 0
  | _, .not e | _, .neg e | _, .shlC _ e | _, .lshrC _ e | _, .ashrC _ e
  | _, .zext _ e | _, .sext _ e | _, .extract _ _ e => e.reads
  | _, .ite s a b => max s.reads (max a.reads b.reads)
  | _, .and a b | _, .or a b | _, .xor a b | _, .add a b | _, .eq a b | _, .ult a b
  | _, .slt a b | _, .mul a b | _, .sub a b | _, .ule a b | _, .sle a b | _, .shl a b
  | _, .lshr a b | _, .ashr a b | _, .concat a b | _, .uaddo a b | _, .saddo a b
  | _, .usubo a b | _, .ssubo a b | _, .umulo a b | _, .smulHi a b | _, .smulLo a b
  | _, .udiv a b | _, .urem a b | _, .sdiv a b | _, .srem a b => max a.reads b.reads

theorem agree_l {ρ ρ' : Nat → Bool} {m n : Nat} (h : ∀ j, j < max m n → ρ j = ρ' j) :
    ∀ j, j < m → ρ j = ρ' j := fun j hj => h j (by omega)

theorem agree_r {ρ ρ' : Nat → Bool} {m n : Nat} (h : ∀ j, j < max m n → ρ j = ρ' j) :
    ∀ j, j < n → ρ j = ρ' j := fun j hj => h j (by omega)

/-- An expression's value depends only on the bits below `reads`. -/
theorem BVExpr.denote_congr : ∀ {w : Nat} (e : BVExpr w) (ρ ρ' : Nat → Bool),
    (∀ j, j < e.reads → ρ j = ρ' j) → e.denote ρ = e.denote ρ'
  | w, .var base, ρ, ρ', h => by
    simp only [BVExpr.denote]
    apply BitVec.eq_of_getLsbD_eq
    intro i hi
    rw [getLsbD_bvOf, getLsbD_bvOf, h (base + i) (by simp only [BVExpr.reads]; omega)]
  | _, .const _, _, _, _ => rfl
  | _, .not e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .neg e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .shlC _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .lshrC _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .ashrC _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .zext _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .sext _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .extract _ _ e, ρ, ρ', h => by simp only [BVExpr.denote]; rw [denote_congr e ρ ρ' h]
  | _, .ite s a b, ρ, ρ', h => by
    simp only [BVExpr.denote]
    rw [denote_congr s ρ ρ' (agree_l h), denote_congr a ρ ρ' (agree_l (agree_r h)),
      denote_congr b ρ ρ' (agree_r (agree_r h))]
  | _, .and a b, ρ, ρ', h | _, .or a b, ρ, ρ', h | _, .xor a b, ρ, ρ', h
  | _, .add a b, ρ, ρ', h | _, .eq a b, ρ, ρ', h | _, .ult a b, ρ, ρ', h
  | _, .slt a b, ρ, ρ', h | _, .mul a b, ρ, ρ', h | _, .sub a b, ρ, ρ', h
  | _, .ule a b, ρ, ρ', h | _, .sle a b, ρ, ρ', h | _, .shl a b, ρ, ρ', h
  | _, .lshr a b, ρ, ρ', h | _, .ashr a b, ρ, ρ', h | _, .concat a b, ρ, ρ', h
  | _, .uaddo a b, ρ, ρ', h | _, .saddo a b, ρ, ρ', h | _, .usubo a b, ρ, ρ', h
  | _, .ssubo a b, ρ, ρ', h | _, .umulo a b, ρ, ρ', h | _, .smulHi a b, ρ, ρ', h
  | _, .smulLo a b, ρ, ρ', h | _, .udiv a b, ρ, ρ', h | _, .urem a b, ρ, ρ', h
  | _, .sdiv a b, ρ, ρ', h | _, .srem a b, ρ, ρ', h => by
    simp only [BVExpr.denote]
    rw [denote_congr a ρ ρ' (agree_l h), denote_congr b ρ ρ' (agree_r h)]

/-- A definition `v := e`: input bits `base … base+w-1` name the value of `e`. -/
structure Def where
  w : Nat
  base : Nat
  e : BVExpr w

/-- A formula with sharing: definitions (in order) and a top formula. -/
structure Dag where
  defs : List Def
  top : BVExpr 1

/-- `ρ` with bits `b … b+w-1` replaced by the bits of `v`. -/
def update (ρ : Nat → Bool) (b w : Nat) (v : BitVec w) : Nat → Bool :=
  fun j => if b ≤ j ∧ j < b + w then v.getLsbD (j - b) else ρ j

/-- Evaluate the definitions in order, each on the assignment so far. -/
def evalDefs : List Def → (Nat → Bool) → (Nat → Bool)
  | [], ρ => ρ
  | d :: ds, ρ => evalDefs ds (update ρ d.base d.w (d.e.denote ρ))

/-- The meaning of a DAG formula: the inputs are free, every defined variable
is the value of its definition, and the top formula is evaluated on that. -/
def Dag.eval (g : Dag) (ρ : Nat → Bool) : BitVec 1 := g.top.denote (evalDefs g.defs ρ)

/-- Each definition reads only bits below its own base, and the definitions'
ranges are laid out upwards in order (checked at run time). -/
def defsOK : Nat → List Def → Bool
  | _, [] => true
  | lo, d :: ds => decide (lo ≤ d.base) && decide (d.e.reads ≤ d.base) && defsOK (d.base + d.w) ds

/-- The DAG as one flat formula: `(v₁ = e₁) ∧ … ∧ (vₙ = eₙ) ∧ top`. -/
def defsExpr : List Def → BVExpr 1 → BVExpr 1
  | [], top => top
  | d :: ds, top => .and (.eq (.var d.base : BVExpr d.w) d.e) (defsExpr ds top)

def Dag.toExpr (g : Dag) : BVExpr 1 := defsExpr g.defs g.top

theorem evalDefs_below : ∀ (ds : List Def) (lo : Nat) (ρ : Nat → Bool), defsOK lo ds = true →
    ∀ j, j < lo → evalDefs ds ρ j = ρ j
  | [], _, _, _, _, _ => rfl
  | d :: ds, lo, ρ, h, j, hj => by
    simp only [defsOK, Bool.and_eq_true, decide_eq_true_eq] at h
    obtain ⟨⟨h1, _⟩, h3⟩ := h
    simp only [evalDefs]
    rw [evalDefs_below ds _ _ h3 j (by omega)]
    simp only [update]
    rw [ite_eq_right_iff]
    intro h
    omega

theorem one_and (x : BitVec 1) : 1#1 &&& x = x := by
  apply BitVec.eq_of_getLsbD_eq
  intro i hi
  have : i = 0 := by omega
  subst this
  simp

theorem defsExpr_eval : ∀ (ds : List Def) (top : BVExpr 1) (lo : Nat) (ρ : Nat → Bool),
    defsOK lo ds = true →
    (defsExpr ds top).denote (evalDefs ds ρ) = top.denote (evalDefs ds ρ)
  | [], _, _, _, _ => rfl
  | d :: ds, top, lo, ρ, h => by
    have h' := h
    simp only [defsOK, Bool.and_eq_true, decide_eq_true_eq] at h'
    obtain ⟨⟨_, hr⟩, h3⟩ := h'
    let ρ1 := update ρ d.base d.w (d.e.denote ρ)
    have hbelow := evalDefs_below ds _ ρ1 h3
    have hvar : (BVExpr.var d.base : BVExpr d.w).denote (evalDefs ds ρ1) = d.e.denote ρ := by
      simp only [BVExpr.denote]
      apply BitVec.eq_of_getLsbD_eq
      intro i hi
      rw [getLsbD_bvOf, hbelow (d.base + i) (by omega)]
      simp only [ρ1, update]
      have hin : d.base ≤ d.base + i ∧ d.base + i < d.base + d.w := by omega
      simp [hin, hi]
    have he : d.e.denote (evalDefs ds ρ1) = d.e.denote ρ := by
      apply BVExpr.denote_congr
      intro j hj
      rw [hbelow j (by omega)]
      simp only [ρ1, update]
      rw [ite_eq_right_iff]
      intro h
      omega
    simp only [evalDefs, defsExpr, BVExpr.denote]
    rw [defsExpr_eval ds top _ ρ1 h3]
    change BitVec.ofBool ((BVExpr.var d.base : BVExpr d.w).denote (evalDefs ds ρ1) ==
      d.e.denote (evalDefs ds ρ1)) &&& _ = _
    rw [hvar, he]
    simp [one_and]
    rfl

/-- A DAG that some input satisfies makes its flat formula satisfiable. -/
theorem dag_sat_imp (g : Dag) (hok : defsOK 0 g.defs = true) (ρ : Nat → Bool)
    (h : g.eval ρ = 1#1) : FSat g.toExpr :=
  ⟨evalDefs g.defs ρ, by
    simp only [Dag.toExpr]
    rw [defsExpr_eval g.defs g.top 0 ρ hok]
    exact h⟩

/-- **Certified mode for DAG formulas.** -/
theorem certified_dag_unsat (g : Dag) (cert : Array Std.Tactic.BVDecide.LRAT.IntAction)
    (hok : defsOK 0 g.defs = true)
    (h : Std.Tactic.BVDecide.LRAT.check cert (toCNF g.toExpr) = true) : ∀ ρ, g.eval ρ ≠ 1#1 :=
  fun ρ hρ => by
    obtain ⟨ρ', h'⟩ := dag_sat_imp g hok ρ hρ
    exact certified_unsat g.toExpr cert h ρ' h'

/-- The CNF `prism-bitblast` writes for a DAG. -/
def dagCNF (g : Dag) : CNF Nat := toCNF g.toExpr

/-- Exactly what `prism-lrat-check --dag` evaluates. -/
def checkDag (g : Dag) (cert : Array Std.Tactic.BVDecide.LRAT.IntAction) : Bool :=
  defsOK 0 g.defs && Std.Tactic.BVDecide.LRAT.check cert (dagCNF g)

theorem checkDag_sound (g : Dag) (cert : Array Std.Tactic.BVDecide.LRAT.IntAction)
    (h : checkDag g cert = true) : ∀ ρ, g.eval ρ ≠ 1#1 := by
  simp only [checkDag, Bool.and_eq_true] at h
  exact certified_dag_unsat g cert h.1 h.2

end PrismTechniques.Bitblast
