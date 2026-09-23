/-
PRISM refinement, extended fragment — `freeze`, `undef`, direct calls and stack memory
(roadmap 8.2, milestone M9).

The base fragment (`Llvm.lean`) is one call-free function with a
deterministic semantics.  This file extends it with

* `freeze` of an operand, a register, `poison` or `undef` (LangRef: "if the
  argument is `undef` or `poison`, `freeze` returns an arbitrary, but fixed,
  value of the type"; otherwise the argument);
* `undef`, **only as the operand of `freeze` or the value of a `store`**.
  Anywhere else an `undef` value is a *set* of values that each use may pick
  from independently (LangRef "Undefined Values"; `%y = xor %x, %x` with
  `%x = xor i32 undef, 0` need not be 0), and branching on it or returning
  it from a `noundef` function is UB.  `translate.cpp` havocs such an
  operand, which fixes one value for every use of what is computed from it
  and hides that UB, so it is not a sound translation in general; those uses
  stay outside the fragment (the exporter writes them, the checker refuses
  them);
* direct calls `%r = call iN @f(args)` to functions of the same module, with
  a stack of frames;
* stack memory: `alloca` of a constant size, integer `load` / `store` (the
  stored value may also be `undef`: its bytes are uninitialised), and
  `getelementptr`, on the byte memory of `XMem.lean`, with the undefined
  behaviour PRISM checks for (null, wild, freed, out of bounds, misaligned,
  read-only, uninitialised read; pointer arithmetic that overflows, leaves
  its array or its object).

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
import PrismRefine.XMem

namespace PrismRefine

open PrismSem

/-! ## Syntax -/

/-- The operand of `freeze`: an ordinary operand, or `undef`. -/
inductive FOpnd where
  | o (x : Opnd)
  | undef
  deriving DecidableEq, Repr, Inhabited

/-- One index of a `getelementptr`, with what the data layout says of it
(the exporter computes allocation sizes, field offsets and array lengths
with the translator's own `Layout`, `translate_mem.cpp`). -/
inductive GIdx where
  /-- the first index, scaled by the allocation size of the source element type -/
  | first (o : Opnd) (w scale : Nat)
  /-- a struct field: a constant byte offset -/
  | field (off : Nat)
  /-- an index into an array of `n` elements of `scale` bytes; `n > 0`: the C
  array bound is checked (C17 6.5.6p8: the index stays in its array, also in a
  nested array), `use` says what the result is for (0 an address, 1 a load,
  2 a store, 3 a further `getelementptr`) -/
  | arr (o : Opnd) (w scale n use : Nat)
  deriving DecidableEq, Repr, Inhabited

/-- A non-call instruction. -/
inductive SInst where
  | i (x : Inst)
  /-- `%dst = freeze iw a` -/
  | freeze (dst : String) (w : Nat) (a : FOpnd)
  /-- `%dst = alloca` of `size` bytes, `align` -/
  | alloca (dst : String) (size align : Nat)
  /-- `%dst = load iw, ptr p, align al` -/
  | load (dst : String) (w : Nat) (p : Opnd) (al : Nat)
  /-- `store iw v, ptr p, align al` (`v` may be `undef`) -/
  | store (w : Nat) (v : FOpnd) (p : Opnd) (al : Nat)
  /-- `%dst = getelementptr [inbounds] T, ptr base, ix…` -/
  | gep (dst : String) (inb : Bool) (base : Opnd) (ix : List GIdx)
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

/-! ## Memory operations (shared by both semantics) -/

/-- `w`-bit value `v` sign-extended to 64 bits. -/
def sext64 (w v : Nat) : Nat := ((bv w v).signExtend 64).toNat

/-- Pointer arithmetic, as the translator computes it (`MemTr::gep`): the
variable part of the offset is accumulated left to right in signed 64 bits,
every scaling and every addition checked for overflow (C17 6.5.6p8: the
result of pointer arithmetic must be representable), constant indices are
folded into `cst`. -/
structure GAcc where
  cst : Int
  var : Option Nat
  deriving DecidableEq, Repr, Inhabited

def gAddVar (acc : GAcc) (x : Nat) : Res GAcc :=
  match acc.var with
  | none => .ok { acc with var := some x }
  | some y => if ovfTest .sadd 64 y x then .ub else .ok { acc with var := some ((y + x) % 2 ^ 64) }

/-- One scaled index term (value `v` of the `w`-bit operand `o`). -/
def gTerm (acc : GAcc) (o : Opnd) (w scale v : Nat) : Res GAcc :=
  match o with
  | .const _ => .ok { acc with cst := acc.cst + (bv 64 (sext64 w v)).toInt * scale }
  | _ =>
    let s := if w < 64 then sext64 w v else v
    if scale = 0 then .ok acc
    else if scale = 1 then gAddVar acc s
    else if ovfTest .smul 64 s scale then .ub
    else gAddVar acc ((bv 64 s * bv 64 scale).toNat)

/-- The C array bound: the index (sign-extended) stays below `n` (`≤ n` for
an address that is not dereferenced). -/
def gBoundBad (w n use v : Nat) : Bool :=
  let s := if w < 64 then sext64 w v % 2 ^ 64 else v % 2 ^ 64
  decide (0 < n) && (if use = 0 then decide (n < s) else decide (n ≤ s))

def gFold : GAcc → List (GIdx × Nat) → Res GAcc
  | acc, [] => .ok acc
  | acc, (.first o w scale, v) :: t => (gTerm acc o w scale v).bind fun a => gFold a t
  | acc, (.field off, _) :: t => gFold { acc with cst := acc.cst + off } t
  | acc, (.arr o w scale n use, v) :: t =>
    if gBoundBad w n use v then .ub else (gTerm acc o w scale v).bind fun a => gFold a t

/-- The result of `getelementptr` from base `b` and the accumulated offset;
UB (C17 6.5.6p8, and LLVM `inbounds` poison) if it leaves its object:
arithmetic on null, beyond one past the end or before the start
(`inbounds`), or out of the object's address range. -/
def gFin (m : Mem) (inb : Bool) (b delta : Nat) : Res Nat :=
  let r := (b % 2 ^ 64 + delta) % 2 ^ 64
  let b' := b % 2 ^ 64
  if inb then
    if (ptrObj b' == 0 && delta % 2 ^ 64 != 0) ||
       (m.kind b' != 0 && (ptrObj r != ptrObj b' || decide (m.size b' < ptrOff r))) then .ub
    else .ok r
  else if ptrObj b' != 0 && ptrObj r != ptrObj b' then .ub else .ok r

def gFinish (m : Mem) (inb : Bool) (b : Nat) (acc : GAcc) : Res Nat :=
  let dc := (acc.cst % 2 ^ 64).toNat
  match acc.var with
  | none => if acc.cst = 0 then .ok (b % 2 ^ 64) else gFin m inb b dc
  | some y =>
    if acc.cst = 0 then gFin m inb b y
    else (gAddVar acc dc).bind fun a => gFin m inb b (a.var.getD 0)

/-- `getelementptr` on register values: base `b`, the index values `vs`. -/
def gepVal (m : Mem) (inb : Bool) (b : Nat) (ix : List GIdx) (vs : List Nat) : Res Nat :=
  (gFold { cst := 0, var := none } (ix.zip vs)).bind fun acc => gFinish m inb b acc

/-- The operand of an index (`none`: a struct field, no operand). -/
def GIdx.opnd : GIdx → Option (Opnd × Nat)
  | .first o w _ => some (o, w)
  | .field _ => none
  | .arr o w _ _ _ => some (o, w)

/-- Index values (every operand is read before the arithmetic, as
`Tr::mem_inst`); a field index reads 0. -/
def sIdxVals (R : SRegs) : List GIdx → Res (List Nat)
  | [] => .ok []
  | g :: t =>
    match g.opnd with
    | some (o, w) => (sOpnd R w o).bind fun v => (sIdxVals R t).bind fun vs => .ok (v :: vs)
    | none => (sIdxVals R t).bind fun vs => .ok (0 :: vs)

/-- The value `store` writes and whether its bytes are initialised: an
indeterminate register or `undef`/`poison` writes uninitialised bytes (the
translator writes a `havoc` value marked uninitialised), which draws. -/
def sStoreVal (ω : Nat → Nat) (R : SRegs) (W : World) (w : Nat) : FOpnd → Res (Nat × Bool × World)
  | .undef => .ok (ω W.t % 2 ^ w, false, W.adv 1)
  | .o .poison => .ok (ω W.t % 2 ^ w, false, W.adv 1)
  | .o (.const b) => .ok (b % 2 ^ w, true, W)
  | .o (.reg n) =>
    match R n with
    | some (.val v) => .ok (v, true, W)
    | some .ind => .ok (0, false, W)
    | none => .stuck

/-! ## Strict semantics -/

/-- One non-call instruction; `t` is the number of values drawn so far. -/
def sSInst (ω : Nat → Nat) (R : SRegs) (W : World) : SInst → Res (SRegs × World)
  | .i x => (sInst R x).bind fun R' => .ok (R', W.adv x.draws)
  | .freeze d w a =>
    match a with
    | .undef => .ok (R.set d (ω W.t % 2 ^ w), W.adv 1)
    | .o o => (sOpnd R w o).bind fun v => .ok (R.set d (v % 2 ^ w), W)
  | .alloca d size al =>
    let (m', p) := W.mem.alloc (size % 2 ^ 64) 1 al 0
    .ok (R.set d p, { W with mem := m' })
  | .load d w p al =>
    (sOpnd R 64 p).bind fun pv =>
      if accessBad W.mem (pv % 2 ^ 64) ((w + 7) / 8) false al then .ub
      -- PRISM's rule: reading uninitialised memory is an error at the load
      else if (loadCells W.mem pv w).any (·.isNone) then .ub
      else .ok (R.set d (bytesVal ((loadCells W.mem pv w).map (·.getD 0)) % 2 ^ w), W)
  | .store w v p al =>
    (sStoreVal ω R W w v).bind fun (vv, init, W1) =>
      (sOpnd R 64 p).bind fun pv =>
        if accessBad W1.mem (pv % 2 ^ 64) ((w + 7) / 8) true al then .ub
        else .ok (R, W1.store pv vv w init)
  | .gep d inb base ix =>
    (sOpnd R 64 base).bind fun b => (sIdxVals R ix).bind fun vs =>
      (gepVal W.mem inb b ix vs).bind fun r => .ok (R.set d r, W)

def sSInsts (ω : Nat → Nat) : SRegs → World → List SInst → Res (SRegs × World)
  | R, W, [] => .ok (R, W)
  | R, W, i :: is => (sSInst ω R W i).bind fun (R', W') => sSInsts ω R' W' is

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
  | next (st : List Frame) (W : World)
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
def retTo : List Frame → Option Nat → World → Step
  | [], v, _ => .ret v
  | c :: cs, v, t => .next ({ c with pend := v } :: cs) t

def sEnd (M : XMod) (fr : Frame) (rest : List Frame) (B : XBlock) (R : SRegs) (t : World) : Step :=
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
def sStep (M : XMod) (ω : Nat → Nat) (t : World) : List Frame → Step
  | [] => .stuck
  | fr :: rest =>
    match fr.F.blocks[fr.cur]? with
    | none => .stuck
    | some B =>
      (sEnter fr B).step fun R0 =>
        (sSInsts ω R0 t (B.insts fr.seg)).step fun (R, t') => sEnd M fr rest B R t'

def sRunX (M : XMod) (ω : Nat → Nat) : Nat → World → List Frame → Out
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
  sRunX M ω fuel World.init [initFrame F args]

/-! ## LangRef semantics (lazy poison) -/

/-- The registers holding numbers, as strict registers (poison and
indeterminate: indeterminate). -/
def lower (R : LRegs) : SRegs := fun n => (R n).map fun
  | .val v => .val v
  | _ => .ind

def lSInst (ω : Nat → Nat) (S : LSt) (W : World) : SInst → Res (LSt × World)
  | .i x => (lInst S x).bind fun S' => .ok (S', W.adv x.draws)
  | .freeze d w a =>
    match a with
    | .undef => .ok (⟨S.R.set d (.val (ω W.t % 2 ^ w)), S.c⟩, W.adv 1)
    | .o o =>
      (lOpnd S.R w o).bind fun (x, cx) =>
        match x with
        | .val v => .ok (⟨S.R.set d (.val (v % 2 ^ w)), S.c || cx⟩, W)
        | .poison => .ok (⟨S.R.set d (.val (ω W.t % 2 ^ w)), S.c || cx⟩, W.adv 1)
        | .ind => .ub
  | .alloca d size al =>
    let (m', p) := W.mem.alloc (size % 2 ^ 64) 1 al 0
    .ok (⟨S.R.set d (.val p), S.c⟩, { W with mem := m' })
  | .load d w p al =>
    match lOpnd S.R 64 p with
    | .ok (.val pv, _) =>
      if accessBad W.mem (pv % 2 ^ 64) ((w + 7) / 8) false al then .ub
      -- uninitialised bytes: an indeterminate value, flagged (PRISM reports it at the load)
      else .ok (⟨S.R.set d (.val (bytesVal ((loadCells W.mem pv w).map (·.getD 0)) % 2 ^ w)),
                S.c || (loadCells W.mem pv w).any (·.isNone)⟩, W)
    | .ok (_, _) => .ub
    | .ub => .ub
    | .stuck => .stuck
  | .store w v p al =>
    (sStoreVal ω (lower S.R) W w v).bind fun (vv, init, W1) =>
      match lOpnd S.R 64 p with
      | .ok (.val pv, _) =>
        if accessBad W1.mem (pv % 2 ^ 64) ((w + 7) / 8) true al then .ub
        else .ok (S, W1.store pv vv w init)
      | .ok (_, _) => .ub
      | .ub => .ub
      | .stuck => .stuck
  | .gep d inb base ix =>
    -- a poison or indeterminate operand, an overflow, a C bound or leaving the
    -- object: the result is poison (LLVM) or C UB; flagged
    match ((sOpnd (lower S.R) 64 base).bind fun b => (sIdxVals (lower S.R) ix).bind fun vs =>
            gepVal W.mem inb b ix vs) with
    | .ok r => .ok (⟨S.R.set d (.val r), S.c⟩, W)
    | .ub => .ok (⟨S.R.set d (.val 0), true⟩, W)
    | .stuck => .stuck

def lSInsts (ω : Nat → Nat) : LSt → World → List SInst → Res (LSt × World)
  | S, W, [] => .ok (S, W)
  | S, W, i :: is => (lSInst ω S W i).bind fun (S', W') => lSInsts ω S' W' is

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
  | next (st : List LFrame) (W : World) (c : Bool)
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

def lRetTo : List LFrame → Option Nat → World → Bool → LStep
  | [], v, _, c => .ret v c
  | f :: fs, v, t, c => .next ({ f with pend := v } :: fs) t c

def lEnd (M : XMod) (fr : LFrame) (rest : List LFrame) (B : XBlock) (S : LSt) (t : World) : LStep :=
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

def lStep (M : XMod) (ω : Nat → Nat) (t : World) (c : Bool) : List LFrame → LStep
  | [] => .stuck
  | fr :: rest =>
    match fr.F.blocks[fr.cur]? with
    | none => .stuck
    | some B =>
      (lEnter fr B c).lstep fun S0 =>
        (lSInsts ω S0 t (B.insts fr.seg)).lstep fun (S, t') => lEnd M fr rest B S t'

def lRunX (M : XMod) (ω : Nat → Nat) : Nat → World → Bool → List LFrame → LOut
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
  lRunX M ω fuel World.init false [lInitFrame F args]

end PrismRefine
