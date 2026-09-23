/-
PRISM refinement, extended fragment — the certificate check.

`validB M F P C` checks, from the certificate `C` (one `IInfo` per inlined
instance, the analysed function first) alone, everything the refinement
proof (`XRefine.lean`) needs of a PIR function `P`:

* the analysed function's parameters are PIR variables `0 … n-1` with their
  widths, and its entry block is PIR block `0`;
* per instance: every variable its names and shadows read is below `hi`,
  the PIR blocks that end its LLVM blocks are distinct;
* per LLVM block and segment: the PIR block exists and *is* the per-segment
  translation (`XTranslate.lean`) of the segment in the instance's context,
  with temporaries from `k ≥ hi` of the declared widths: the phis (segment
  0) or the continuation phis (later segments: the `_retu` and result
  variables, both written only where the instance owns them), the
  statements, and the terminator — for a call a jump to the callee
  instance's entry, whose parameters are bound to the translated arguments
  (all below the callee's `lo`, and `hi ≤` the callee's `lo`), for a `ret`
  of an inlined instance a jump to the continuation block whose phis map the
  returning block to the returned value.

The check is executable (`pir_lean_check` runs it); the theorems assume it.
-/
import PrismRefine.XTranslate

namespace PrismRefine

open PrismSem

/-- The temporaries from `k` have the widths `tws`. -/
def tempsOKB (P : PFunc) (k : Nat) (tws : List Nat) : Bool :=
  (List.range tws.length).all fun j => P.wd (k + j) == tws.getD j 0

def allLtB (m : Nat) (l : List (String × Arg)) : Bool := l.all fun (_, a) => a.lt m

def nodupNat : List Nat → Bool
  | [] => true
  | x :: xs => !xs.contains x && nodupNat xs

/-- The continuation phis of the call `cl` (in segment `s - 1`): none, or
`_retu` (a temporary) then the result variable. -/
def contOK (c : Ctx) (cl : CallI) (phis : List PPhi) : Bool :=
  match phis with
  | [] => true
  | [pu, pr] =>
    decide (c.hi ≤ pu.dst) &&
    (match cl.dst with
     | some d => (match dstX c d cl.rw with
                  | .ok i => i == pr.dst
                  | .error _ => false) && (look c.sh d).isNone
     | none => decide (c.hi ≤ pr.dst))
  | _ => false

/-- The callee's parameters are bound to the arguments, with no shadow. -/
def paramsOK (J : IInfo) (ps : List (String × Nat)) (As : List Arg) : Bool :=
  (ps.zip As).all fun ((p, _), a) => look J.env p == some a && (look J.sh p).isNone

/-- The segment ending in the call `cl` (child instance `κ`). -/
def callOK (M : XMod) (P : PFunc) (C : List IInfo) (I : IInfo) (b s : Nat) (c : Ctx) (k : Nat)
    (s1 : List PStmt) (t1 : List Nat) (PB : PBlock) (cl : CallI) : Bool :=
  match (I.kids[b]?).bind (·[s]?) with
  | none => false
  | some κ =>
    match C[κ]? with
    | none => false
    | some J =>
      decide (M.find cl.f = some J.fn) &&
      cl.args.length == J.fn.params.length &&
      (J.fn.retw == 0) == (cl.rw == 0) && (cl.rw == 0 || cl.rw == J.fn.retw) &&
      (J.fn.retw != 0 || cl.dst.isNone) &&
      match trArgsX c (k + t1.length) ((cl.args.zip J.fn.params).map fun ((o, w), (_, wp)) => (o, w, wp)) with
      | .error _ => false
      | .ok (s2, t2, As) =>
        tempsOKB P (k + t1.length) t2 &&
        PB.stmts == s1 ++ s2 &&
        (match J.blks[0]? with
         | some (h :: _) => PB.term == .jmp h
         | _ => false) &&
        paramsOK J J.fn.params As &&
        As.all (·.lt J.lo) &&
        decide (I.hi ≤ J.lo) &&
        J.retTo == (I.blks[b]?).bind (·[s + 1]?)

/-- The last segment (the terminator). -/
def termOK (P : PFunc) (I : IInfo) (c : Ctx) (k : Nat) (s1 : List PStmt) (t1 : List Nat)
    (pb : Nat) (PB : PBlock) (t : LTerm) : Bool :=
  match trTermX I.fn c I.heads I.retTo (k + t1.length) t with
  | .error _ => false
  | .ok (s2, t2, T, A?) =>
    tempsOKB P (k + t1.length) t2 &&
    PB.stmts == s1 ++ s2 && PB.term == T &&
    match I.retTo, A? with
    | some cb, some A =>
      match P.blocks[cb]? with
      | some CB =>
        match CB.phis with
        | [pu, pr] => pickInc pb pr.inc == some A && (pickInc pb pu.inc).isSome
        | _ => false
      | none => false
    | _, _ => true

/-- Segment `s` of LLVM block `b` of instance `I`. -/
def segOK (M : XMod) (P : PFunc) (C : List IInfo) (I : IInfo) (b s : Nat) : Bool :=
  match I.fn.blocks[b]?, (I.blks[b]?).bind (·[s]?), (I.ks[b]?).bind (·[s]?) with
  | some B, some pb, some k =>
    match P.blocks[pb]? with
    | none => false
    | some PB =>
      let c := I.ctx P.vars
      decide (I.hi ≤ k) &&
      match trSInstsX c k (B.insts s) with
      | .error _ => false
      | .ok (s1, t1) =>
        tempsOKB P k t1 &&
        (if s = 0 then
          (match trPhisX I.fn c I.tails B.phis with
           | .ok qs => qs == PB.phis
           | .error _ => false)
         else
          (match B.segs[s - 1]? with
           | some (_, cl) => contOK c cl PB.phis
           | none => false)) &&
        match B.segs[s]? with
        | some (_, cl) => callOK M P C I b s c k s1 t1 PB cl
        | none => termOK P I c k s1 t1 pb PB B.term
  | _, _, _ => false

def instOK (M : XMod) (P : PFunc) (C : List IInfo) (I : IInfo) : Bool :=
  decide (I.lo ≤ I.hi) && allLtB I.hi I.env && allLtB I.hi I.sh &&
  I.blks.length == I.fn.blocks.length && I.ks.length == I.fn.blocks.length &&
  I.kids.length == I.fn.blocks.length &&
  nodupNat I.tails &&
  (List.range I.fn.blocks.length).all fun b =>
    match I.fn.blocks[b]? with
    | some B =>
      (I.blks.getD b []).length == B.segs.length + 1 &&
      (I.ks.getD b []).length == B.segs.length + 1 &&
      (I.kids.getD b []).length == B.segs.length &&
      (List.range (B.segs.length + 1)).all fun s => segOK M P C I b s
    | none => false

/-- The analysed function's parameters are its first variables. -/
def topParamsOK (P : PFunc) (I : IInfo) (ps : List (String × Nat)) : Bool :=
  ps.zipIdx.all fun ((p, w), j) => look I.env p == some (.v j w) && (look I.sh p).isNone && P.wd j == w

def validB (M : XMod) (F : XFunc) (P : PFunc) (C : List IInfo) : Bool :=
  match C[0]? with
  | some I0 =>
    decide (I0.fn = F) && I0.retTo.isNone &&
    P.params == List.range' 0 F.params.length && P.retw == F.retw &&
    (match I0.blks[0]? with
     | some (h :: _) => h == 0
     | _ => false) &&
    topParamsOK P I0 F.params &&
    C.all (instOK M P C)
  | none => false

end PrismRefine
