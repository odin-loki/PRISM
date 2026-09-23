/-
PRISM refinement, extended fragment — `freeze`, `undef` and direct calls
(roadmap 8.2, milestone M9).

The base fragment (`Llvm.lean`) is one call-free function with a
deterministic semantics.  This file extends it with

* `freeze` of an operand, a register, `poison` or `undef` (LangRef: "if the
  argument is `undef` or `poison`, `freeze` returns an arbitrary, but fixed,
  value of the type"; otherwise the argument);
* `undef`, **only as the operand of `freeze`**.  Anywhere else an `undef`
  value is a *set* of values that each use may pick from independently
  (LangRef "Undefined Values"; `%y = xor %x, %x` with `%x = undef` need not be
  0).  `translate.cpp` havocs such an operand once, which fixes one value for
  every use, so it is not a sound translation in general; those uses stay
  outside the fragment (the exporter writes them, the checker refuses them);
* direct calls `%r = call iN @f(args)` to functions of the same module, with
  a stack of frames.

Nondeterminism is an *oracle* `ω : Nat → Nat` read in execution order: the
`t`-th arbitrary value of a run is `ω t` (masked to the width).  `freeze` of
`undef` (and, in the LangRef semantics, of a poison value) draws one value;
so does the uninitialised-local marker `@__prism.uninit` (its value is never
used: the register is indeterminate; the draw only keeps the count in step
with PIR, whose `havoc` draws).  Quantifying over every `ω` gives every
behaviour.

Control state is a stack of frames.  A block is split at its calls into
*segments*: `segs = [(insts₀, call₀), …]` then `last` and the terminator.
One step of the machine executes one segment: on entry to a block the phis,
on entry to segment `s > 0` the result of the call that ended segment `s-1`
is written to its register (`pend`), then the instructions, then the call
(push a frame for the callee) or the terminator (`br`: same frame, next
block; `ret`: pop, the caller gets the value).  Fuel counts steps, i.e.
executed block segments; every theorem holds for every fuel, so the bound is
not a limitation of the results.

As in `Llvm.lean` there are two semantics: `sRunX` (strict: creating poison
is UB — what PRISM's instrumentation implements) and `lRunX` (LangRef, lazy
poison, reporting whether poison was created).  Returning poison (from any
function) is UB, PRISM's rule matching clang's `noundef` on C return values;
passing poison as an argument is a use of it (clang marks C parameters
`noundef`).
-/
import PrismRefine.Llvm

namespace PrismRefine

open PrismSem

/-! ## Syntax -/

/-- The operand of `freeze`: an ordinary operand, or `undef`. -/
inductive FOpnd where
  | o (x : Opnd)
  | undef
  deriving DecidableEq, Repr, Inhabited

/-- A non-call instruction. -/
inductive SInst where
  | i (x : Inst)
  /-- `%dst = freeze iw a` -/
  | freeze (dst : String) (w : Nat) (a : FOpnd)
  deriving DecidableEq, Repr, Inhabited

/-- `%dst = call i<rw> @f(a₁ w₁, …)` (`rw = 0`: `void`; `dst = none`: result
unused).  `wᵢ` is the width of the argument operand. -/
structure CallI where
  dst : Option String
  rw : Nat
  f : String
  args : List (Opnd × Nat)
  deriving DecidableEq, Repr, Inhabited

/-- A block split at its calls: phis, then segments each ending in a call,
then the last instructions and the terminator. -/
structure XBlock where
  name : String
  phis : List PhiI
  segs : List (List SInst × CallI)
  last : List SInst
  term : LTerm
  deriving DecidableEq, Repr, Inhabited

structure XFunc where
  name : String
  params : List (String × Nat)
  retw : Nat
  blocks : List XBlock
  deriving DecidableEq, Repr, Inhabited

/-- A module: the functions calls can reach (first match by name). -/
abbrev XMod := List XFunc

def XMod.find (M : XMod) (n : String) : Option XFunc := M.find? (·.name == n)

/-- The instructions of segment `s`. -/
def XBlock.insts (B : XBlock) (s : Nat) : List SInst :=
  match B.segs[s]? with
  | some (is, _) => is
  | none => B.last

/-- The block names of `G`, as an `LFunc` (for `lookupBlock` / `phiPick`). -/
def XFunc.shape (G : XFunc) : LFunc :=
  { name := G.name, params := G.params, retw := G.retw,
    blocks := G.blocks.map fun B => { name := B.name, phis := [], insts := [], term := .unreachable } }

/-- Draws of an instruction of the base fragment: the uninitialised-local
marker draws one (ignored) value, in step with PIR's `havoc`. -/
def Inst.draws : Inst → Nat
  | .uninit _ _ => 1
  | _ => 0

/-! ## Strict semantics -/

/-- One non-call instruction; `t` is the number of values drawn so far. -/
def sSInst (ω : Nat → Nat) (R : SRegs) (t : Nat) : SInst → Res (SRegs × Nat)
  | .i x => (sInst R x).bind fun R' => .ok (R', t + x.draws)
  | .freeze d w a =>
    match a with
    | .undef => .ok (R.set d (ω t % 2 ^ w), t + 1)
    | .o o => (sOpnd R w o).bind fun v => .ok (R.set d (v % 2 ^ w), t)

def sSInsts (ω : Nat → Nat) : SRegs → Nat → List SInst → Res (SRegs × Nat)
  | R, t, [] => .ok (R, t)
  | R, t, i :: is => (sSInst ω R t i).bind fun (R', t') => sSInsts ω R' t' is

/-- Call arguments, left to right (each a use). -/
def sArgs (R : SRegs) : List (Opnd × Nat) → Res (List Nat)
  | [] => .ok []
  | (o, w) :: t => (sOpnd R w o).bind fun v => (sArgs R t).bind fun vs => .ok (v :: vs)

/-- The callee's registers: parameter `i` is argument `i` (first match). -/
def bindArgs : List (String × Nat) → List Nat → SRegs
  | (p, _) :: ps, v :: vs => fun n => if n = p then some (.val v) else bindArgs ps vs n
  | _, _ => fun _ => none

structure Frame where
  F : XFunc
  prev : Option Nat
  cur : Nat
  seg : Nat
  R : SRegs
  /-- the value returned by the call that ended segment `seg - 1` -/
  pend : Option Nat

inductive Step where
  | next (st : List Frame) (t : Nat)
  | ret (v : Option Nat)
  | ub
  | stuck

def Res.step {α : Type} : Res α → (α → Step) → Step
  | .ok a, f => f a
  | .ub, _ => .ub
  | .stuck, _ => .stuck

/-- Registers on entry to the current segment: the phis (segment 0) or the
result of the call that ended the previous segment. -/
def sEnter (fr : Frame) (B : XBlock) : Res SRegs :=
  if fr.seg = 0 then (sPhis fr.F.shape fr.R fr.prev B.phis).bind fun upd => .ok (fr.R.setAll upd)
  else
    match B.segs[fr.seg - 1]? with
    | some (_, c) =>
      match c.dst with
      | some d =>
        match fr.pend with
        | some v => .ok (fr.R.set d v)
        | none => .stuck
      | none => .ok fr.R
    | none => .stuck

/-- Return `v` to the caller (or out of the run). -/
def retTo : List Frame → Option Nat → Nat → Step
  | [], v, _ => .ret v
  | c :: cs, v, t => .next ({ c with pend := v } :: cs) t

def sEnd (M : XMod) (fr : Frame) (rest : List Frame) (B : XBlock) (R : SRegs) (t : Nat) : Step :=
  match B.segs[fr.seg]? with
  | some (_, c) =>
    (sArgs R c.args).step fun vs =>
      match M.find c.f with
      | some G =>
        if vs.length = G.params.length then
          .next ({ F := G, prev := none, cur := 0, seg := 0, R := bindArgs G.params vs, pend := none } ::
            { fr with R := R, seg := fr.seg + 1, pend := none } :: rest) t
        else .stuck
      | none => .stuck
  | none =>
    match B.term with
    | .br tn =>
      match lookupBlock fr.F.shape tn with
      | some j => .next ({ fr with prev := some fr.cur, cur := j, seg := 0, R := R, pend := none } :: rest) t
      | none => .stuck
    | .cbr c tn fn =>
      (sOpnd R 1 c).step fun v =>
        match lookupBlock fr.F.shape (if truthN v then tn else fn) with
        | some j => .next ({ fr with prev := some fr.cur, cur := j, seg := 0, R := R, pend := none } :: rest) t
        | none => .stuck
    | .ret none => retTo rest none t
    | .ret (some o) => (sOpnd R fr.F.retw o).step fun v => retTo rest (some v) t
    | .unreachable => .ub

/-- One step: the current segment of the top frame. -/
def sStep (M : XMod) (ω : Nat → Nat) (t : Nat) : List Frame → Step
  | [] => .stuck
  | fr :: rest =>
    match fr.F.blocks[fr.cur]? with
    | none => .stuck
    | some B =>
      (sEnter fr B).step fun R0 =>
        (sSInsts ω R0 t (B.insts fr.seg)).step fun (R, t') => sEnd M fr rest B R t'

def sRunX (M : XMod) (ω : Nat → Nat) : Nat → Nat → List Frame → Out
  | 0, _, _ => .fuel
  | n + 1, t, st =>
    match sStep M ω t st with
    | .next st' t' => sRunX M ω n t' st'
    | .ret v => .ret v
    | .ub => .ub
    | .stuck => .stuck

def initFrame (F : XFunc) (args : List Nat) : Frame :=
  { F := F, prev := none, cur := 0, seg := 0, R := initRegs F.params args, pend := none }

def sRunXF (M : XMod) (F : XFunc) (args : List Nat) (ω : Nat → Nat) (fuel : Nat) : Out :=
  sRunX M ω fuel 0 [initFrame F args]

/-! ## LangRef semantics (lazy poison) -/

def lSInst (ω : Nat → Nat) (S : LSt) (t : Nat) : SInst → Res (LSt × Nat)
  | .i x => (lInst S x).bind fun S' => .ok (S', t + x.draws)
  | .freeze d w a =>
    match a with
    | .undef => .ok (⟨S.R.set d (.val (ω t % 2 ^ w)), S.c⟩, t + 1)
    | .o o =>
      (lOpnd S.R w o).bind fun (x, cx) =>
        match x with
        | .val v => .ok (⟨S.R.set d (.val (v % 2 ^ w)), S.c || cx⟩, t)
        | .poison => .ok (⟨S.R.set d (.val (ω t % 2 ^ w)), S.c || cx⟩, t + 1)
        | .ind => .ub

def lSInsts (ω : Nat → Nat) : LSt → Nat → List SInst → Res (LSt × Nat)
  | S, t, [] => .ok (S, t)
  | S, t, i :: is => (lSInst ω S t i).bind fun (S', t') => lSInsts ω S' t' is

def lArgs (R : LRegs) : List (Opnd × Nat) → Res (List LV × Bool)
  | [] => .ok ([], false)
  | (o, w) :: t => (lOpnd R w o).bind fun (v, c1) => (lArgs R t).bind fun (vs, c2) => .ok (v :: vs, c1 || c2)

def lBindArgs : List (String × Nat) → List LV → LRegs
  | (p, _) :: ps, v :: vs => fun n => if n = p then some v else lBindArgs ps vs n
  | _, _ => fun _ => none

structure LFrame where
  F : XFunc
  prev : Option Nat
  cur : Nat
  seg : Nat
  R : LRegs
  pend : Option Nat

inductive LStep where
  | next (st : List LFrame) (t : Nat) (c : Bool)
  | ret (v : Option Nat) (c : Bool)
  | ub
  | stuck

def Res.lstep {α : Type} : Res α → (α → LStep) → LStep
  | .ok a, f => f a
  | .ub, _ => .ub
  | .stuck, _ => .stuck

def lEnter (fr : LFrame) (B : XBlock) (c : Bool) : Res LSt :=
  if fr.seg = 0 then
    (lPhis fr.F.shape fr.R fr.prev B.phis).bind fun (upd, c1) => .ok ⟨fr.R.setAll upd, c || c1⟩
  else
    match B.segs[fr.seg - 1]? with
    | some (_, cl) =>
      match cl.dst with
      | some d =>
        match fr.pend with
        | some v => .ok ⟨fr.R.set d (.val v), c⟩
        | none => .stuck
      | none => .ok ⟨fr.R, c⟩
    | none => .stuck

def lRetTo : List LFrame → Option Nat → Nat → Bool → LStep
  | [], v, _, c => .ret v c
  | f :: fs, v, t, c => .next ({ f with pend := v } :: fs) t c

def lEnd (M : XMod) (fr : LFrame) (rest : List LFrame) (B : XBlock) (S : LSt) (t : Nat) : LStep :=
  match B.segs[fr.seg]? with
  | some (_, cl) =>
    (lArgs S.R cl.args).lstep fun (vs, ca) =>
      match M.find cl.f with
      | some G =>
        if vs.length = G.params.length then
          .next ({ F := G, prev := none, cur := 0, seg := 0, R := lBindArgs G.params vs, pend := none } ::
            { fr with R := S.R, seg := fr.seg + 1, pend := none } :: rest) t (S.c || ca)
        else .stuck
      | none => .stuck
  | none =>
    match B.term with
    | .br tn =>
      match lookupBlock fr.F.shape tn with
      | some j => .next ({ fr with prev := some fr.cur, cur := j, seg := 0, R := S.R, pend := none } :: rest) t S.c
      | none => .stuck
    | .cbr c tn fn =>
      match lOpnd S.R 1 c with
      | .ok (.val v, _) =>
        match lookupBlock fr.F.shape (if truthN v then tn else fn) with
        | some j => .next ({ fr with prev := some fr.cur, cur := j, seg := 0, R := S.R, pend := none } :: rest) t S.c
        | none => .stuck
      | .ok (.poison, _) => .ub
      | .ok (.ind, _) => .ub
      | .ub => .ub
      | .stuck => .stuck
    | .ret none => lRetTo rest none t S.c
    | .ret (some o) =>
      match lOpnd S.R fr.F.retw o with
      | .ok (.val v, cv) => lRetTo rest (some v) t (S.c || cv)
      | .ok (.poison, _) => .ub
      | .ok (.ind, _) => .ub
      | .ub => .ub
      | .stuck => .stuck
    | .unreachable => .ub

def lStep (M : XMod) (ω : Nat → Nat) (t : Nat) (c : Bool) : List LFrame → LStep
  | [] => .stuck
  | fr :: rest =>
    match fr.F.blocks[fr.cur]? with
    | none => .stuck
    | some B =>
      (lEnter fr B c).lstep fun S0 =>
        (lSInsts ω S0 t (B.insts fr.seg)).lstep fun (S, t') => lEnd M fr rest B S t'

def lRunX (M : XMod) (ω : Nat → Nat) : Nat → Nat → Bool → List LFrame → LOut
  | 0, _, c, _ => .fuel c
  | n + 1, t, c, st =>
    match lStep M ω t c st with
    | .next st' t' c' => lRunX M ω n t' c' st'
    | .ret v c' => .ret v c'
    | .ub => .ub
    | .stuck => .stuck

def lInitFrame (F : XFunc) (args : List Nat) : LFrame :=
  { F := F, prev := none, cur := 0, seg := 0, R := lInit F.params args, pend := none }

def lRunXF (M : XMod) (F : XFunc) (args : List Nat) (ω : Nat → Nat) (fuel : Nat) : LOut :=
  lRunX M ω fuel 0 false [lInitFrame F args]

end PrismRefine
