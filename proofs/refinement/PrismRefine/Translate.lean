/-
PRISM refinement — the LLVM-fragment → PIR translator, written to mirror
`src/prism/pir/translate.cpp` statement for statement on the fragment:

* variables: parameters first, then one variable per SSA result in textual
  order (phis before the other instructions of a block; `icmp` results are
  `i1`), then temporaries in the order the translator allocates them
  (`Tr::newvar`);
* block `i` of the LLVM function is PIR block `i` (no calls in the fragment,
  so `head[q] == tail[q]`);
* an operand that is the constant `poison` becomes `check 1` (UB-POISON) and
  a `havoc` temporary (`Tr::operand`);
* each instruction gets exactly the checks `Tr::binop` / `Tr::inst` insert
  for its opcode and flags, in the same order, with the same property name
  and taxonomy class, each as `t := test(a, b); check t` (`Tr::p2`);
* `unreachable` becomes `check 1` (CXX-UNREACHABLE) and `stop`;
* uninitialised locals (`Tr::enter_frame`, `Tr::operand`): the result of
  `@__prism.uninit.iN()` is a `havoc`; it and every phi that may carry it
  (least fixpoint over phi inputs) get a *shadow* — the constant `1` for the
  call result, a fresh `i1` variable (allocated after the SSA results) for
  such a phi, updated by a shadow phi — and every use of a register with a
  shadow is preceded by `check shadow` (UNINIT-READ).

Where the C++ translator would throw `UNENCODED`, or where the input is not
in the fragment this file models, `translate` returns an error; the
correspondence checker then reports the function as outside the fragment
instead of comparing it.  `translate` additionally rejects, as outside the
fragment, IR the LLVM verifier would reject anyway (duplicate SSA or block
names, flags an opcode cannot carry) — on such input `translate.cpp` has no
defined meaning to mirror — and a `poison` incoming value of a phi, which is
a gap in `translate.cpp` (it havocs the value with no check; see
docs/PROOFS_REFINEMENT.md).
-/
import PrismRefine.Pir

namespace PrismRefine

open PrismSem

/-- Index and declared width of a name (first match). -/
def findName : List (String × Nat) → String → Option (Nat × Nat)
  | [], _ => none
  | (m, w) :: t, n => if m = n then some (0, w) else (findName t n).map (fun p => (p.1 + 1, p.2))

/-- `int_width`: PIR models `i1` … `i64`. -/
def okW (w : Nat) : Bool := 1 ≤ w && w ≤ 64

/-- The uninitialised-read check of a register use (`Tr::operand`): `check
shadow` when the register has one. -/
def shChecks : Option Arg → List PStmt
  | some a => [.check a "uninit" "UNINIT-READ"]
  | none => []

/-- `Tr::operand`.  `keep`: the use does not overwrite the argument width
(branch and select conditions, return values); otherwise the argument gets
the instruction's width `w` (`a.width = w` in `Tr::binop` etc.). -/
def trOpnd (names : List (String × Nat)) (sh : String → Option Arg) (w : Nat) (keep : Bool)
    (k : Nat) : Opnd → Except String (List PStmt × List Nat × Arg)
  | .const b => .ok ([], [], .c w (b % 2 ^ w))
  | .reg n =>
    match findName names n with
    | some (i, wd) => .ok (shChecks (sh n), [], .v i (if keep then wd else w))
    | none => .error s!"UNENCODED: value %{n}"
  | .poison => .ok ([.check (.c 1 1) "poison" "UB-POISON", .havoc k], [w], .v k w)

/-- One inserted check. -/
inductive Chk where
  /-- `t := op(a, b); check t` (`Tr::p2` + `Tr::check`). -/
  | p (op : POp) (a b : Arg) (prop cls : String)
  /-- `or disjoint`: `t1 := and a b; t2 := ne t1 0; check t2`. -/
  | disj (a b : Arg) (w : Nat)
  deriving DecidableEq, Repr

def Chk.emit (k : Nat) : Chk → List PStmt × List Nat
  | .p op a b prop cls => ([.assign k op [a, b], .check (.v k 1) prop cls], [1])
  | .disj a b w =>
    ([.assign k (.bin .and) [a, b], .assign (k + 1) (.cmp .ne) [.v k w, .c w 0],
      .check (.v (k + 1) 1) "disjoint" "UB-POISON"], [w, 1])

def emitAll (k : Nat) : List Chk → List PStmt × List Nat
  | [] => ([], [])
  | c :: cs =>
    let r1 := c.emit k
    let r2 := emitAll (k + r1.2.length) cs
    (r1.1 ++ r2.1, r1.2 ++ r2.2)

def opt (b : Bool) (c : Chk) : List Chk := if b then [c] else []

/-- The checks `Tr::binop` inserts, in order. -/
def checks (op : BinOp) (fl : LFlags) (w : Nat) (A B : Arg) : List Chk :=
  let zero := Arg.c w 0
  match op with
  | .add => opt fl.nsw (.p (.ovf .sadd) A B "ovf+" "INT-SIGNED-OVF") ++
            opt fl.nuw (.p (.ovf .uadd) A B "wrap+" "UB-POISON")
  | .sub => opt fl.nsw (.p (.ovf .ssub) A B "ovf-" "INT-SIGNED-OVF") ++
            opt fl.nuw (.p (.ovf .usub) A B "wrap-" "UB-POISON")
  | .mul => opt fl.nsw (.p (.ovf .smul) A B "ovf*" "INT-SIGNED-OVF") ++
            opt fl.nuw (.p (.ovf .umul) A B "wrap*" "UB-POISON")
  | .udiv => [.p (.cmp .eq) B zero "div0" "INT-DIV-ZERO"] ++
             opt fl.exact (.p .inexactU A B "exact" "UB-POISON")
  | .urem => [.p (.cmp .eq) B zero "mod0" "INT-DIV-ZERO"]
  | .sdiv => [.p (.cmp .eq) B zero "div0" "INT-DIV-ZERO", .p .sdivOvf A B "divovf" "INT-SIGNED-OVF"] ++
             opt fl.exact (.p .inexactS A B "exact" "UB-POISON")
  | .srem => [.p (.cmp .eq) B zero "mod0" "INT-DIV-ZERO", .p .sdivOvf A B "divovf" "INT-SIGNED-OVF"]
  | .shl => [.p .shiftOob A B "shift" "INT-SHIFT-UB"] ++
            opt fl.csigned (.p .shlSOvf A B "shift-base" "INT-SHIFT-UB") ++
            opt fl.nsw (.p .shlNswOvf A B "shl-nsw" "UB-POISON") ++
            opt fl.nuw (.p .shlNuwOvf A B "shl-nuw" "UB-POISON")
  | .lshr => [.p .shiftOob A B "shift" "INT-SHIFT-UB"] ++
             opt fl.exact (.p .lostBitsL A B "exact" "UB-POISON")
  | .ashr => [.p .shiftOob A B "shift" "INT-SHIFT-UB"] ++
             opt fl.exact (.p .lostBitsA A B "exact" "UB-POISON")
  | .and => []
  | .or => opt fl.disjoint (.disj A B w)
  | .xor => []

/-- Flags an opcode can carry in valid IR (anything else: outside the
fragment). -/
def flagsOk (op : BinOp) (fl : LFlags) : Bool :=
  match op with
  | .add | .sub | .mul => !fl.exact && !fl.disjoint && !fl.csigned
  | .udiv | .sdiv => !fl.nsw && !fl.nuw && !fl.disjoint && !fl.csigned
  | .shl => !fl.exact && !fl.disjoint
  | .lshr | .ashr => !fl.nsw && !fl.nuw && !fl.disjoint && !fl.csigned
  | .or => !fl.nsw && !fl.nuw && !fl.exact && !fl.csigned
  | .urem | .srem | .and | .xor => !fl.nsw && !fl.nuw && !fl.exact && !fl.disjoint && !fl.csigned

/-- The variable of a result, which must have been declared with width `w`. -/
def dstIdx (names : List (String × Nat)) (d : String) (w : Nat) : Except String Nat :=
  match findName names d with
  | some (i, w') => if w' = w then .ok i else .error s!"width mismatch for %{d}"
  | none => .error s!"undeclared result %{d}"

def need (b : Bool) (msg : String) : Except String Unit := if b then .ok () else .error msg

def trInst (names : List (String × Nat)) (sh : String → Option Arg) (k : Nat) :
    Inst → Except String (List PStmt × List Nat)
  | .bin d op fl w a b => do
    need (okW w) "UNENCODED: width"
    need (flagsOk op fl) "outside fragment: flags"
    let (sa, ta, A) ← trOpnd names sh w false k a
    let (sb, tb, B) ← trOpnd names sh w false (k + ta.length) b
    let (sc, tc) := emitAll (k + ta.length + tb.length) (checks op fl w A B)
    let i ← dstIdx names d w
    need (sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ sc ++ [.assign i (.bin op) [A, B]], ta ++ tb ++ tc)
  | .icmp d p w a b => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpnd names sh w false k a
    let (sb, tb, B) ← trOpnd names sh w false (k + ta.length) b
    let i ← dstIdx names d 1
    need (sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ [.assign i (.cmp p) [A, B]], ta ++ tb)
  | .select d w c a b => do
    need (okW w) "UNENCODED: width"
    let (sc, tc, C) ← trOpnd names sh 1 true k c
    let (sa, ta, A) ← trOpnd names sh w false (k + tc.length) a
    let (sb, tb, B) ← trOpnd names sh w false (k + tc.length + ta.length) b
    let i ← dstIdx names d w
    need (sh d).isNone "outside fragment: result with a shadow"
    pure (sc ++ sa ++ sb ++ [.assign i .select [C, A, B]], tc ++ ta ++ tb)
  | .cast d ck nneg fw tw a => do
    need (okW fw && okW tw) "UNENCODED: width"
    need (!nneg || ck == .zext) "outside fragment: flags"
    let (sa, ta, A) ← trOpnd names sh fw false k a
    let (sc, tc) := emitAll (k + ta.length)
      (opt nneg (.p (.cmp .slt) A (.c fw 0) "nneg" "UB-POISON"))
    let i ← dstIdx names d tw
    need (sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sc ++ [.assign i (.cast ck) [A]], ta ++ tc)
  | .uninit d w => do
    need (okW w) "UNENCODED: width"
    let i ← dstIdx names d w
    need (sh d == some (.c 1 1)) "outside fragment: uninit marker without its shadow"
    pure ([.havoc i], [])

def trInsts (names : List (String × Nat)) (sh : String → Option Arg) :
    Nat → List Inst → Except String (List PStmt × List Nat)
  | _, [] => .ok ([], [])
  | k, i :: is => do
    let (s1, t1) ← trInst names sh k i
    let (s2, t2) ← trInsts names sh (k + t1.length) is
    pure (s1 ++ s2, t1 ++ t2)

/-- A phi incoming value (`Tr::operand` with `use = false`: no width
override). -/
def trIncArg (names : List (String × Nat)) (w : Nat) : Opnd → Except String Arg
  | .const b => .ok (.c w (b % 2 ^ w))
  | .reg n =>
    match findName names n with
    | some (i, wd) => .ok (.v i wd)
    | none => .error s!"UNENCODED: value %{n}"
  | .poison => .error "outside fragment: poison incoming value of a phi"

/-- Incoming entries; entries from a block that does not exist are dropped
(`Tr::resolve_phis`: predecessor never translated). -/
def trInc (F : LFunc) (names : List (String × Nat)) (w : Nat) :
    List (Opnd × String) → Except String (List (Nat × Arg))
  | [] => .ok []
  | (o, pr) :: t => do
    let a ← trIncArg names w o
    let rest ← trInc F names w t
    pure (match lookupBlock F pr with
      | some j => (j, a) :: rest
      | none => rest)

/-- The shadow of a phi incoming value (`Tr::run_frame`: `Arg::c(1, 0)`
unless the incoming register has a shadow). -/
def incShadow (sh : String → Option Arg) : Opnd → Arg
  | .reg m => (sh m).getD (.c 1 0)
  | _ => .c 1 0

def trIncSh (F : LFunc) (sh : String → Option Arg) : List (Opnd × String) → List (Nat × Arg)
  | [] => []
  | (o, pr) :: t =>
    match lookupBlock F pr with
    | some j => (j, incShadow sh o) :: trIncSh F sh t
    | none => trIncSh F sh t

def hasShadowedInput (sh : String → Option Arg) (inc : List (Opnd × String)) : Bool :=
  inc.any fun (o, _) => match o with
    | .reg m => (sh m).isSome
    | _ => false

/-- A phi, followed by its shadow phi when it may carry an uninitialised
value. -/
def trPhi (F : LFunc) (names : List (String × Nat)) (sh : String → Option Arg) (p : PhiI) :
    Except String (List PPhi) := do
  need (okW p.w) "UNENCODED: width"
  let i ← dstIdx names p.dst p.w
  let inc ← trInc F names p.w p.inc
  match sh p.dst with
  | some (.v sv _) => pure [{ dst := i, inc := inc }, { dst := sv, inc := trIncSh F sh p.inc }]
  | some (.c _ _) => throw "outside fragment: phi with a constant shadow"
  | none =>
    need (!hasShadowedInput sh p.inc) "outside fragment: uninitialised input without a shadow phi"
    pure [{ dst := i, inc := inc }]

def trPhis (F : LFunc) (names : List (String × Nat)) (sh : String → Option Arg) :
    List PhiI → Except String (List PPhi)
  | [] => .ok []
  | p :: ps => do
    let q ← trPhi F names sh p
    let qs ← trPhis F names sh ps
    pure (q ++ qs)

def target (F : LFunc) (t : String) : Except String Nat :=
  match lookupBlock F t with
  | some j => .ok j
  | none => .error s!"UNENCODED: branch to unknown block {t}"

def trTerm (F : LFunc) (names : List (String × Nat)) (sh : String → Option Arg) (k : Nat) :
    LTerm → Except String (List PStmt × List Nat × PTerm)
  | .br t => do
    let j ← target F t
    pure ([], [], .jmp j)
  | .cbr c t f => do
    let (s, tw, C) ← trOpnd names sh 1 true k c
    let tj ← target F t
    let fj ← target F f
    pure (s, tw, .br C tj fj)
  | .ret none => .ok ([], [], .ret none)
  | .ret (some o) => do
    let (s, tw, A) ← trOpnd names sh F.retw true k o
    pure (s, tw, .ret (some A))
  | .unreachable => .ok ([.check (.c 1 1) "unreachable" "CXX-UNREACHABLE"], [], .stop)

def trBlock (F : LFunc) (names : List (String × Nat)) (sh : String → Option Arg) (k : Nat)
    (B : LBlock) : Except String (PBlock × List Nat) := do
  let phis ← trPhis F names sh B.phis
  let (s1, t1) ← trInsts names sh k B.insts
  let (s2, t2, T) ← trTerm F names sh (k + t1.length) B.term
  pure ({ phis := phis, stmts := s1 ++ s2, term := T }, t1 ++ t2)

def trBlocks (F : LFunc) (names : List (String × Nat)) (sh : String → Option Arg) :
    Nat → List LBlock → Except String (List PBlock × List Nat)
  | _, [] => .ok ([], [])
  | k, B :: Bs => do
    let (pb, t1) ← trBlock F names sh k B
    let (pbs, t2) ← trBlocks F names sh (k + t1.length) Bs
    pure (pb :: pbs, t1 ++ t2)

def Inst.result : Inst → String × Nat
  | .bin d _ _ w _ _ => (d, w)
  | .icmp d _ _ _ _ => (d, 1)
  | .select d w _ _ _ => (d, w)
  | .cast d _ _ _ tw _ => (d, tw)
  | .uninit d w => (d, w)

/-- SSA results in textual order (`Tr::enter_frame`). -/
def resultNames (F : LFunc) : List (String × Nat) :=
  F.blocks.flatMap (fun B => B.phis.map (fun p => (p.dst, p.w)) ++ B.insts.map Inst.result)

def nodupB : List String → Bool
  | [] => true
  | x :: xs => !xs.contains x && nodupB xs

/-! ### Uninitialised locals (`Tr::enter_frame`) -/

def phisOf (F : LFunc) : List PhiI := F.blocks.flatMap (·.phis)

def uninitDsts (F : LFunc) : List String :=
  F.blocks.flatMap fun B => B.insts.filterMap fun
    | .uninit d _ => some d
    | _ => none

/-- One pass of the "may carry an uninitialised value" fixpoint. -/
def muStep (ps : List PhiI) (mu : List String) : List String :=
  ps.foldl (fun mu p =>
    if mu.contains p.dst then mu
    else if p.inc.any (fun (o, _) => match o with | .reg m => mu.contains m | _ => false)
      then mu ++ [p.dst] else mu) mu

def muFix (ps : List PhiI) : Nat → List String → List String
  | 0, mu => mu
  | n + 1, mu => muFix ps n (muStep ps mu)

/-- Phis that may carry an uninitialised value, in textual order: each gets a
shadow variable. -/
def shadowPhis (F : LFunc) : List (String × Nat) :=
  let mu := muFix (phisOf F) ((phisOf F).length + 1) (uninitDsts F)
  ((phisOf F).filter (fun p => mu.contains p.dst)).map (fun p => (p.dst, 1))

/-- The shadow of a register: `1` for a `__prism.uninit` result, the shadow
variable (numbered after the SSA results) for a phi that may carry one. -/
def shadowOf (F : LFunc) (nNames : Nat) (n : String) : Option Arg :=
  if (uninitDsts F).contains n then some (.c 1 1)
  else (findName (shadowPhis F) n).map fun p => .v (nNames + p.1) 1

/-- The translator (`prism::pir::translate` on the fragment). -/
def translate (F : LFunc) : Except String PFunc := do
  need (F.params.all (fun p => okW p.2)) "UNENCODED: parameter type"
  need (F.retw == 0 || okW F.retw) "UNENCODED: return type"
  need (!F.blocks.isEmpty) "UNENCODED: empty function body"
  let names := F.params ++ resultNames F
  need (nodupB (names.map Prod.fst)) "outside fragment: duplicate SSA name"
  need (nodupB (F.blocks.map LBlock.name)) "outside fragment: duplicate block name"
  let sh := shadowOf F names.length
  need (F.params.all (fun p => (sh p.1).isNone)) "outside fragment: parameter with a shadow"
  let nsh := (shadowPhis F).length
  let (bs, temps) ← trBlocks F names sh (names.length + nsh) F.blocks
  pure { vars := names.map Prod.snd ++ List.replicate nsh 1 ++ temps,
         params := List.range' 0 F.params.length, retw := F.retw, blocks := bs }

end PrismRefine
