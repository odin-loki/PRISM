/-
PRISM refinement — a formal semantics for the fragment of LLVM IR that the
C++ translator `src/prism/pir/translate.cpp` turns into PIR (roadmap 8.2,
"LLVM IR to PIR translation").

The fragment (everything `translate.cpp` handles in a single, call-free
function whose values are all integers):

* SSA registers of type `iN` (`1 ≤ N ≤ 64`), integer constants, `poison`;
* `add/sub/mul` with `nsw`/`nuw`, `udiv/sdiv` with `exact`, `urem/srem`,
  `shl` with `nsw`/`nuw`, `lshr/ashr` with `exact`, `and`, `or` with
  `disjoint`, `xor`;
* `icmp` (all ten predicates), `select`, `zext` (with `nneg`), `sext`,
  `trunc`, `phi`;
* terminators `br label`, `br i1`, `ret`, `ret void`, `unreachable`.

Two semantics are given, both deterministic interpreters over a control
state *(previous block, current block, register file)* whose fuel counts
executed blocks:

* `lRun` — the **LangRef semantics** ("lazy poison"): a flag violation
  (`nsw` overflow, over-wide shift, inexact `exact` operation, `disjoint` with
  common bits, negative `zext nneg` argument) produces the value `poison`;
  poison propagates through arithmetic, `icmp`, casts and `phi`, `select`
  on a poison condition is poison; *immediate* undefined behaviour is
  division by zero, `INT_MIN / -1` (for `sdiv` and `srem`), a poison
  divisor, branching on poison, `unreachable`, and — PRISM's rule, matching
  clang's `noundef` on C return values — returning poison.
  The run also reports whether poison was ever *created*.
* `sRun` — the **strict semantics** PRISM's instrumentation implements:
  every poison-producing operation is itself an error (`ub`).

`Refine.lean` proves `sRun` and `lRun` agree up to the first creation of
poison, and that the PIR produced by the translator (`Translate.lean`) fails
an assertion exactly when `sRun` reaches `ub`.

Value representation: a register holds a natural number; an instruction of
width `w` reads it as `BitVec.ofNat w`.  Values the semantics writes are
always `< 2^w`, so this is the usual two's-complement bitvector semantics.
The value of each operation is `PrismSem.evalBin` / `PrismSem.evalPred`
from the PIR semantics (proofs/semantics), imported, not copied.
-/
import PrismSem.Eval

namespace PrismRefine

open PrismSem

/-- `BitVec.ofNat` shorthand. -/
abbrev bv (w x : Nat) : BitVec w := BitVec.ofNat w x

/-- Truth of an `i1` stored as a natural number (bit 0), as the C++
interpreter's `x & 1`. -/
def truthN (x : Nat) : Bool := x % 2 == 1

/-! ## Syntax -/

/-- Instruction flags.  `csigned` is not an LLVM flag: the C++ translator
receives, from clang's AST, the positions of C signed `<<` expressions
(`TranslateOptions::signed_shl`) and adds the C 6.5.7p4 check there; the
exporter passes that fact per instruction. -/
structure LFlags where
  nsw : Bool := false
  nuw : Bool := false
  exact : Bool := false
  disjoint : Bool := false
  csigned : Bool := false
  deriving DecidableEq, Repr, Inhabited

/-- An operand: a register, an integer constant (two's-complement bits,
masked to the width of the use), or the constant `poison`. -/
inductive Opnd where
  | reg (n : String)
  | const (bits : Nat)
  | poison
  deriving DecidableEq, Repr, Inhabited

inductive CastK where
  | zext | sext | trunc
  deriving DecidableEq, Repr, Inhabited

/-- Non-terminator, non-phi instructions.  `w` is the operand width (for
`icmp`, the width of the compared operands; the result is `i1`). -/
inductive Inst where
  | bin (dst : String) (op : BinOp) (fl : LFlags) (w : Nat) (a b : Opnd)
  | icmp (dst : String) (p : Pred) (w : Nat) (a b : Opnd)
  | select (dst : String) (w : Nat) (c a b : Opnd)
  | cast (dst : String) (k : CastK) (nneg : Bool) (fw tw : Nat) (a : Opnd)
  deriving Repr, Inhabited

/-- `%dst = phi iw [v, %pred], ...` -/
structure PhiI where
  dst : String
  w : Nat
  inc : List (Opnd × String)
  deriving Repr, Inhabited

inductive LTerm where
  | br (t : String)
  | cbr (c : Opnd) (t f : String)
  | ret (v : Option Opnd)
  | unreachable
  deriving Repr, Inhabited

structure LBlock where
  name : String
  phis : List PhiI
  insts : List Inst
  term : LTerm
  deriving Repr, Inhabited

/-- A function: parameters `(name, width)`, return width (`0` = void), blocks
(the first is the entry). -/
structure LFunc where
  name : String
  params : List (String × Nat)
  retw : Nat
  blocks : List LBlock
  deriving Repr, Inhabited

/-! ## Values of the operations (shared by every semantics) -/

def binVal (op : BinOp) (w x y : Nat) : Nat := (evalBin op (bv w x) (bv w y)).toNat

def icmpVal (p : Pred) (w x y : Nat) : Nat :=
  (BitVec.ofBool (evalPred p (bv w x) (bv w y))).toNat

def selVal (w c x y : Nat) : Nat := if truthN c then x % 2 ^ w else y % 2 ^ w

def castVal (k : CastK) (fw tw x : Nat) : Nat :=
  match k with
  | .zext => ((bv fw x).setWidth tw).toNat
  | .sext => ((bv fw x).signExtend tw).toNat
  | .trunc => ((bv fw x).setWidth tw).toNat

/-! ## Undefined behaviour and poison, per the LangRef

These are stated in arithmetic terms, independently of how the C++
translator tests them; `Ops.lean` proves the translator's tests equal them. -/

/-- The signed range of `iw`. -/
def sInRange (w : Nat) (z : Int) : Bool := decide (-2 ^ (w - 1) ≤ z ∧ z < 2 ^ (w - 1))

/-- Immediate UB of a division / remainder with non-poison operands:
division by zero; `INT_MIN / -1` and `INT_MIN % -1` for the signed forms. -/
def binUB (op : BinOp) (w x y : Nat) : Bool :=
  match op with
  | .udiv | .urem => bv w y == 0#w
  | .sdiv | .srem => bv w y == 0#w || (bv w x == BitVec.intMin w && bv w y == BitVec.allOnes w)
  | _ => false

/-- C UB carried by `csigned` (C11 6.5.7p4): `E1 << E2` with `E1` signed is
undefined if `E1` is negative or `E1 * 2^E2` is not representable. -/
def cUB (op : BinOp) (fl : LFlags) (w x y : Nat) : Bool :=
  match op with
  | .shl => fl.csigned && decide ((bv w y).toNat < w) &&
      ((bv w x).toInt < 0 || !decide ((bv w x).toInt * 2 ^ (bv w y).toNat < 2 ^ (w - 1)))
  | _ => false

/-- The poison-producing conditions (LangRef, "poison value" of each
instruction):
* `nsw`: the mathematical signed result is out of range;
* `nuw`: the mathematical unsigned result is out of range;
* `shl`/`lshr`/`ashr`: shift amount `≥ w`; `shl nuw`: a non-zero bit is
  shifted out (`x * 2^y ≥ 2^w`); `shl nsw`: `x * 2^y` (signed) is out of range;
* `lshr/ashr exact`: a non-zero bit is shifted out (`x mod 2^y ≠ 0`);
* `udiv/sdiv exact`: `x` is not a multiple of `y`;
* `or disjoint`: the operands have a common set bit. -/
def binPoison (op : BinOp) (fl : LFlags) (w x y : Nat) : Bool :=
  let a := bv w x
  let b := bv w y
  match op with
  | .add => (fl.nsw && !sInRange w (a.toInt + b.toInt)) || (fl.nuw && decide (2 ^ w ≤ a.toNat + b.toNat))
  | .sub => (fl.nsw && !sInRange w (a.toInt - b.toInt)) || (fl.nuw && decide (a.toNat < b.toNat))
  | .mul => (fl.nsw && !sInRange w (a.toInt * b.toInt)) || (fl.nuw && decide (2 ^ w ≤ a.toNat * b.toNat))
  | .udiv => fl.exact && a.toNat % b.toNat != 0
  | .sdiv => fl.exact && !decide (b.toInt ∣ a.toInt)
  | .shl => decide (w ≤ b.toNat) ||
      (fl.nsw && !sInRange w (a.toInt * 2 ^ b.toNat)) ||
      (fl.nuw && decide (2 ^ w ≤ a.toNat * 2 ^ b.toNat))
  | .lshr | .ashr => decide (w ≤ b.toNat) || (fl.exact && a.toNat % 2 ^ b.toNat != 0)
  | .or => fl.disjoint && (a &&& b) != 0#w
  | _ => false

/-- `zext nneg` of a negative value is poison. -/
def castPoison (k : CastK) (nneg : Bool) (fw x : Nat) : Bool :=
  k == .zext && nneg && (bv fw x).msb

/-- The divisor of `udiv/sdiv/urem/srem` may not be poison (LangRef: UB). -/
def isDivOp : BinOp → Bool
  | .udiv | .sdiv | .urem | .srem => true
  | _ => false

/-! ## Control helpers -/

/-- Index of the block called `n` (first match). -/
def lookupBlock (F : LFunc) (n : String) : Option Nat := F.blocks.findIdx? (·.name == n)

/-- The first incoming entry of a phi whose predecessor is block `prev`. -/
def phiPick (F : LFunc) (prev : Nat) : List (Opnd × String) → Option Opnd
  | [] => none
  | (o, p) :: t => if lookupBlock F p = some prev then some o else phiPick F prev t

/-- Result of a step: a value, immediate undefined behaviour, or stuck (the
program is not well formed on this path: an undefined register, a missing
phi entry, a branch to a missing block). -/
inductive Res (α : Type) where
  | ok (a : α)
  | ub
  | stuck
  deriving Repr

def Res.bind {α β : Type} : Res α → (α → Res β) → Res β
  | .ok a, f => f a
  | .ub, _ => .ub
  | .stuck, _ => .stuck

/-- Observable outcome of the strict run. -/
inductive Out where
  | ret (v : Option Nat)
  | ub
  | stuck
  | fuel
  deriving DecidableEq, Repr

/-! ## Strict semantics (`sRun`): poison creation is an error -/

abbrev SRegs := String → Option Nat

def SRegs.set (R : SRegs) (n : String) (v : Nat) : SRegs := fun m => if m = n then some v else R m

def sOpnd (R : SRegs) (w : Nat) : Opnd → Res Nat
  | .reg n => match R n with
    | some v => .ok v
    | none => .stuck
  | .const b => .ok (b % 2 ^ w)
  | .poison => .ub

def sInst (R : SRegs) : Inst → Res SRegs
  | .bin d op fl w a b =>
    (sOpnd R w a).bind fun x => (sOpnd R w b).bind fun y =>
      if binUB op w x y || cUB op fl w x y || binPoison op fl w x y then .ub
      else .ok (R.set d (binVal op w x y))
  | .icmp d p w a b =>
    (sOpnd R w a).bind fun x => (sOpnd R w b).bind fun y => .ok (R.set d (icmpVal p w x y))
  | .select d w c a b =>
    (sOpnd R 1 c).bind fun z => (sOpnd R w a).bind fun x => (sOpnd R w b).bind fun y =>
      .ok (R.set d (selVal w z x y))
  | .cast d k nneg fw tw a =>
    (sOpnd R fw a).bind fun x =>
      if castPoison k nneg fw x then .ub else .ok (R.set d (castVal k fw tw x))

def sInsts (R : SRegs) : List Inst → Res SRegs
  | [] => .ok R
  | i :: is => (sInst R i).bind fun R' => sInsts R' is

/-- Values of the phis of a block entered from `prev` (read in parallel). -/
def sPhis (F : LFunc) (R : SRegs) (prev : Option Nat) : List PhiI → Res (List (String × Nat))
  | [] => .ok []
  | p :: ps =>
    match prev with
    | none => .stuck
    | some pv =>
      match phiPick F pv p.inc with
      | none => .stuck
      | some o => (sOpnd R p.w o).bind fun v => (sPhis F R prev ps).bind fun t => .ok ((p.dst, v) :: t)

def SRegs.setAll (R : SRegs) : List (String × Nat) → SRegs
  | [] => R
  | (n, v) :: t => (R.set n v).setAll t

/-- Continue a step result into an outcome. -/
def Res.out {α : Type} : Res α → (α → Out) → Out
  | .ok a, f => f a
  | .ub, _ => .ub
  | .stuck, _ => .stuck

/-- The terminator of a block; `run j R` continues at block `j`. -/
def sTerm (F : LFunc) (R : SRegs) (run : Nat → SRegs → Out) : LTerm → Out
  | .br t =>
    match lookupBlock F t with
    | some j => run j R
    | none => .stuck
  | .cbr c t f =>
    (sOpnd R 1 c).out fun v =>
      match lookupBlock F (if truthN v then t else f) with
      | some j => run j R
      | none => .stuck
  | .ret none => .ret none
  | .ret (some o) => (sOpnd R F.retw o).out fun v => .ret (some v)
  | .unreachable => .ub

def sRun (F : LFunc) : Nat → Option Nat → Nat → SRegs → Out
  | 0, _, _, _ => .fuel
  | n + 1, prev, cur, R =>
    match F.blocks[cur]? with
    | none => .stuck
    | some B =>
      (sPhis F R prev B.phis).out fun upd =>
        (sInsts (R.setAll upd) B.insts).out fun R' =>
          sTerm F R' (fun j R'' => sRun F n (some cur) j R'') B.term

/-- Parameter `i` gets `args[i] mod 2^w` (a missing argument is `0`, as in
the C++ interpreter; with a repeated name the first parameter wins). -/
def initRegs : List (String × Nat) → List Nat → SRegs
  | [], _ => fun _ => none
  | (p, w) :: ps, args => fun n => if n = p then some (args.headD 0 % 2 ^ w) else initRegs ps args.tail n

def sRunF (F : LFunc) (args : List Nat) (fuel : Nat) : Out :=
  sRun F fuel none 0 (initRegs F.params args)

/-! ## LangRef semantics (`lRun`): lazy poison -/

inductive LV where
  | poison
  | val (v : Nat)
  deriving DecidableEq, Repr

abbrev LRegs := String → Option LV

def LRegs.set (R : LRegs) (n : String) (v : LV) : LRegs := fun m => if m = n then some v else R m

/-- Operand read: the value, and whether the read *created* poison (the
literal constant `poison`). -/
def lOpnd (R : LRegs) (w : Nat) : Opnd → Res (LV × Bool)
  | .reg n => match R n with
    | some v => .ok (v, false)
    | none => .stuck
  | .const b => .ok (.val (b % 2 ^ w), false)
  | .poison => .ok (.poison, true)

/-- Lazy state: registers and "poison has been created". -/
structure LSt where
  R : LRegs
  c : Bool

def lBin (S : LSt) (d : String) (op : BinOp) (fl : LFlags) (w : Nat) :
    LV × Bool → LV × Bool → Res LSt
  | (a, ca), (b, cb) =>
    let c := S.c || ca || cb
    match a, b with
    | _, .poison => if isDivOp op then .ub else .ok ⟨S.R.set d .poison, c⟩
    | .poison, .val y =>
      -- a poison dividend may be refined to any value, so a zero divisor, or
      -- `-1` for a signed division (the dividend could be `INT_MIN`), is UB
      if binUB op w (BitVec.intMin w).toNat y then .ub else .ok ⟨S.R.set d .poison, c⟩
    | .val x, .val y =>
      if binUB op w x y || cUB op fl w x y then .ub
      else if binPoison op fl w x y then .ok ⟨S.R.set d .poison, true⟩
      else .ok ⟨S.R.set d (.val (binVal op w x y)), c⟩

def lInst (S : LSt) : Inst → Res LSt
  | .bin d op fl w a b =>
    (lOpnd S.R w a).bind fun A => (lOpnd S.R w b).bind fun B => lBin S d op fl w A B
  | .icmp d p w a b =>
    (lOpnd S.R w a).bind fun (x, ca) => (lOpnd S.R w b).bind fun (y, cb) =>
      let c := S.c || ca || cb
      match x, y with
      | .val x, .val y => .ok ⟨S.R.set d (.val (icmpVal p w x y)), c⟩
      | _, _ => .ok ⟨S.R.set d .poison, c⟩
  | .select d w c a b =>
    (lOpnd S.R 1 c).bind fun (z, cz) => (lOpnd S.R w a).bind fun (x, ca) =>
      (lOpnd S.R w b).bind fun (y, cb) =>
      let cc := S.c || cz || ca || cb
      match z with
      | .poison => .ok ⟨S.R.set d .poison, cc⟩
      | .val z =>
        let pick := if truthN z then x else y
        match pick with
        | .poison => .ok ⟨S.R.set d .poison, cc⟩
        | .val v => .ok ⟨S.R.set d (.val (v % 2 ^ w)), cc⟩
  | .cast d k nneg fw tw a =>
    (lOpnd S.R fw a).bind fun (x, ca) =>
      let c := S.c || ca
      match x with
      | .poison => .ok ⟨S.R.set d .poison, c⟩
      | .val x =>
        if castPoison k nneg fw x then .ok ⟨S.R.set d .poison, true⟩
        else .ok ⟨S.R.set d (.val (castVal k fw tw x)), c⟩

def lInsts (S : LSt) : List Inst → Res LSt
  | [] => .ok S
  | i :: is => (lInst S i).bind fun S' => lInsts S' is

def lPhis (F : LFunc) (R : LRegs) (prev : Option Nat) : List PhiI → Res (List (String × LV) × Bool)
  | [] => .ok ([], false)
  | p :: ps =>
    match prev with
    | none => .stuck
    | some pv =>
      match phiPick F pv p.inc with
      | none => .stuck
      | some o => (lOpnd R p.w o).bind fun (v, c1) => (lPhis F R prev ps).bind fun (t, c2) =>
          .ok ((p.dst, v) :: t, c1 || c2)

def LRegs.setAll (R : LRegs) : List (String × LV) → LRegs
  | [] => R
  | (n, v) :: t => (R.set n v).setAll t

/-- Outcome of the LangRef run; a return and running out of fuel report
whether poison was created on the way. -/
inductive LOut where
  | ret (v : Option Nat) (c : Bool)
  | ub
  | stuck
  | fuel (c : Bool)
  deriving DecidableEq, Repr

/-- "The run reached undefined behaviour or created poison." -/
def LOut.bad : LOut → Bool
  | .ub => true
  | .ret _ c | .fuel c => c
  | .stuck => false

def Res.lout {α : Type} : Res α → (α → LOut) → LOut
  | .ok a, f => f a
  | .ub, _ => .ub
  | .stuck, _ => .stuck

def lTerm (F : LFunc) (S : LSt) (run : Nat → LSt → LOut) : LTerm → LOut
  | .br t =>
    match lookupBlock F t with
    | some j => run j S
    | none => .stuck
  | .cbr c t f =>
    match lOpnd S.R 1 c with
    | .ok (.val v, _) =>
      match lookupBlock F (if truthN v then t else f) with
      | some j => run j S
      | none => .stuck
    | .ok (.poison, _) => .ub
    | .ub => .ub
    | .stuck => .stuck
  | .ret none => .ret none S.c
  | .ret (some o) =>
    match lOpnd S.R F.retw o with
    | .ok (.val v, cv) => .ret (some v) (S.c || cv)
    | .ok (.poison, _) => .ub
    | .ub => .ub
    | .stuck => .stuck
  | .unreachable => .ub

def lRun (F : LFunc) : Nat → Option Nat → Nat → LSt → LOut
  | 0, _, _, S => .fuel S.c
  | n + 1, prev, cur, S =>
    match F.blocks[cur]? with
    | none => .stuck
    | some B =>
      (lPhis F S.R prev B.phis).lout fun (upd, c1) =>
        (lInsts ⟨S.R.setAll upd, S.c || c1⟩ B.insts).lout fun S' =>
          lTerm F S' (fun j S'' => lRun F n (some cur) j S'') B.term

def lInit (ps : List (String × Nat)) (args : List Nat) : LRegs :=
  fun n => (initRegs ps args n).map LV.val

def lRunF (F : LFunc) (args : List Nat) (fuel : Nat) : LOut :=
  lRun F fuel none 0 ⟨lInit F.params args, false⟩

end PrismRefine
