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
* the intrinsics `translate.cpp` translates specially (`llvm.smax` …,
  `abs`, `ctlz`/`cttz`, `ctpop`, `bswap`, `expect`, `*.with.overflow` with
  `extractvalue`, `lifetime.start`/`end`, `memcpy`/`memmove`/`memset`) and the
  globals the analysed function names (`SInst.glob`).

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
  /-- `%dst = call iw @llvm.smax.iw(a, b)` (`smin`, `umax`, `umin`) -/
  | mm (dst : String) (k : MMK) (w : Nat) (a b : Opnd)
  /-- `%dst = call iw @llvm.abs.iw(a, i1 flag)` (`ctlz`, `cttz` with their
  flag; `ctpop`, `bswap` without) -/
  | un (dst : String) (k : UnK) (w : Nat) (a : Opnd) (flag : Bool)
  /-- `%dst = call iw @llvm.expect.iw(a, c)` -/
  | expect (dst : String) (w : Nat) (a : Opnd)
  /-- `%dst = call {iw, i1} @llvm.<k>.with.overflow.iw(a, b)`: the two fields
  are the registers `pairReg dst 0` and `pairReg dst 1` -/
  | ovf (dst : String) (k : OvfOp) (w : Nat) (a b : Opnd)
  /-- `%dst = extractvalue {iw, i1} %src, idx` of an overflow pair -/
  | xv (dst : String) (w : Nat) (src : String) (idx : Nat)
  /-- `call void @llvm.lifetime.start(i64 n, ptr p)` -/
  | lstart (n : Nat) (p : Opnd)
  /-- `call void @llvm.lifetime.end(i64 n, ptr p)` -/
  | lend (p : Opnd)
  /-- `call void @llvm.memcpy` / `memmove` (`move`) `(ptr d, ptr s, i<lw> len, i1 volatile)` -/
  | memcpy (d s len : Opnd) (lw : Nat) (move : Bool)
  /-- `call void @llvm.memset(ptr d, i8 b, i<lw> len, i1 volatile)` -/
  | memset (d b len : Opnd) (lw : Nat)
  /-- A read-only global the analysed function names (`%@g`): at the start of
  the entry block, a fresh object of `size` bytes (`kind` 4 read-only, 3 a
  static the function may write), zero-filled (`init = 1`: read-only data, and
  every global of `main`, which starts from the initialisers —
  `TranslateOptions::globals_initial`) then
  its initialiser's non-zero stores `(offset, width, bits)`; or (`init = 2`:
  a mutable global of any other function, which may run in any program state;
  an external object; a large table) of arbitrary initialised bytes.  The entry block
  has no predecessors (an LLVM verifier rule), so this runs once, before
  anything reads the global: a run of the function from a program state where
  the global holds its initial value. -/
  | glob (dst : String) (size align kind init : Nat) (st : List (Nat × Nat × Nat))
  /-- `%dst = icmp <p> ptr a, b` with a relational predicate (`ult` … `sge`):
  PRISM's rule is C's (C17 6.5.8p5), the two pointers must point into the same
  object; the value is the comparison of the 64-bit pointers (as LLVM compares
  addresses).  Pointer `eq`/`ne` is the integer `icmp` at width 64. -/
  | pcmp (dst : String) (p : Pred) (a b : Opnd)
  deriving DecidableEq, Repr, Inhabited

/-- The register of field `i` of an overflow pair (a name no LLVM register
the exporter writes can have: it contains a blank). -/
def pairReg (d : String) (i : Nat) : String := d ++ " ." ++ toString i

/-- The arithmetic of an overflow intrinsic. -/
def ovfBin : OvfOp → BinOp
  | .sadd | .uadd => .add
  | .ssub | .usub => .sub
  | .smul | .umul => .mul

/-- LangRef: `llvm.abs(x, true)` is poison for `INT_MIN`,
`llvm.ctlz/cttz(x, true)` for `0`. -/
def unPoison (k : UnK) (flag : Bool) (w x : Nat) : Bool :=
  flag && match k with
    | .abs => bv w x == BitVec.intMin w
    | .ctlz | .cttz => bv w x == 0#w
    | _ => false

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
  /-- pointer parameters of the analysed function that no harness object
  binds (no `byval`/`sret`, no contract): the translator refuses the
  function (Law 6, NEEDS-HARNESS); they have no semantics here.  A bound one
  is not a parameter but an object allocated on entry (`SInst.glob`). -/
  ptrParams : List String := []
  /-- the analysed function returns a pointer (checked for pointing into its
  own stack objects, `MemTr::stack_escape_check`) -/
  retPtr : Bool := false
  deriving DecidableEq, Repr, Inhabited

/-- A module: the functions calls can reach (first match by name). -/
abbrev XMod := List XFunc

def XMod.find (M : XMod) (n : String) : Option XFunc := M.find? (·.name == n)

/-- The instructions of segment `s`. -/
def XBlock.insts (B : XBlock) (s : Nat) : List SInst :=
  match B.segs[s]? with
  | some (is, _) => is
  | none => B.last

def SInst.allocaDst? : SInst → Option String
  | .alloca d _ _ => some d
  | _ => none

/-- The function's own stack objects: the registers of the `alloca`s of the
entry block before its first call (`Tr::mem_inst` records exactly those in
`Frame::allocas`). -/
def XFunc.escNames (G : XFunc) : List String :=
  match G.blocks with
  | B :: _ => (B.insts 0).filterMap SInst.allocaDst?
  | [] => []

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

/-- `llvm.memcpy` of `n` bytes from `s` to `d` is undefined (`n ≠ 0`): either
access is bad (`accessBad`, byte alignment), or — `memcpy`, not `memmove` —
the two ranges overlap and are not the same range: the LangRef allows an
exact self-copy (`d = s`: same object, same offset), which clang emits for a
struct assignment `*p = *q` that may be a self-assignment.  (C's `memcpy`
function forbids `d = s` too, C17 7.24.2.1; the pir stage compiles with
`-fno-builtin-memcpy`, so a C `memcpy` call reaches its library model, not
this intrinsic.) -/
def cpyBad (m : Mem) (d s n : Nat) (move : Bool) : Bool :=
  n != 0 && (accessBad m d n true 1 || accessBad m s n false 1 ||
    (!move && ptrOff d != ptrOff s && ptrObj d == ptrObj s && decide (ptrOff d < ptrOff s + n) &&
      decide (ptrOff s < ptrOff d + n)))

/-- The initialiser's stores into the global at `p`. -/
def globW (p : Nat) : World → List (Nat × Nat × Nat) → World
  | W, [] => W
  | W, (o, w, v) :: t => globW p (W.store ((p + o) % 2 ^ 64) (v % 2 ^ w) w true) t

/-- A global: allocated (zero-filled, `init = 1`, or arbitrary bytes,
`init = 2`), then initialised. -/
def globAlloc (ω : Nat → Nat) (W : World) (size align kind init : Nat) (st : List (Nat × Nat × Nat)) :
    Nat × World :=
  ((W.allocW ω (size % 2 ^ 64) kind align init).2,
   globW (W.allocW ω (size % 2 ^ 64) kind align init).2 (W.allocW ω (size % 2 ^ 64) kind align init).1 st)

/-- A relational pointer comparison: UB (C17 6.5.8p5) unless both pointers
point into the same object, else the predicate on the 64-bit values. -/
def sPcmpV (R : SRegs) (p : Pred) (a b : Opnd) : Res Nat :=
  (sOpnd R 64 a).bind fun x => (sOpnd R 64 b).bind fun y =>
    if ptrObj (x % 2 ^ 64) = ptrObj (y % 2 ^ 64) then .ok (icmpVal p 64 x y) else .ub

/-- Whether pointer `v` points into the object of one of the registers `ns`
(each a use; a missing register: stuck). -/
def escHit (R : SRegs) (v : Nat) : List String → Res Bool
  | [] => .ok false
  | a :: t => (sOpnd R 64 (.reg a)).bind fun pa => (escHit R v t).bind fun h =>
      .ok ((ptrObj (v % 2 ^ 64) == ptrObj (pa % 2 ^ 64)) || h)

/-! ## Strict semantics -/

/-- The values of the integer intrinsics. -/
def sUnV (R : SRegs) (k : UnK) (w : Nat) (a : Opnd) (flag : Bool) : Res Nat :=
  (sOpnd R w a).bind fun x => if unPoison k flag w x then .ub else .ok (unVal k w x % 2 ^ w)

def sMMV (R : SRegs) (k : MMK) (w : Nat) (a b : Opnd) : Res Nat :=
  (sOpnd R w a).bind fun x => (sOpnd R w b).bind fun y => .ok (mmVal k w x y % 2 ^ w)

def sExpV (R : SRegs) (w : Nat) (a : Opnd) : Res Nat :=
  (sOpnd R w a).bind fun x => .ok (x % 2 ^ w)

def sXvV (R : SRegs) (w : Nat) (src : String) (idx : Nat) : Res Nat :=
  match R (pairReg src idx) with
  | some (.val v) => .ok (v % 2 ^ w)
  | some .ind => .ub
  | none => .stuck

def sOvfV (R : SRegs) (k : OvfOp) (w : Nat) (a b : Opnd) : Res (Nat × Nat) :=
  (sOpnd R w a).bind fun x => (sOpnd R w b).bind fun y =>
    .ok (binVal (ovfBin k) w x y, (ovfTest k w x y).toNat)

/-- `store` (and `llvm.lifetime.start`, as PRISM's model once was: `n`
uninitialised bytes stored through the pointer, with a write's checks.  The
LangRef starts a new lifetime there, which `translate.cpp` now does
(`Stmt::Revive`); the Lean translator refuses `lifetime.start`, so no theorem
uses this clause). -/
def sStoreR (ω : Nat → Nat) (R : SRegs) (W : World) (w : Nat) (v : FOpnd) (p : Opnd) (al : Nat) :
    Res (SRegs × World) :=
  (sStoreVal ω R W w v).bind fun (vv, init, W1) =>
    (sOpnd R 64 p).bind fun pv =>
      if accessBad W1.mem (pv % 2 ^ 64) ((w + 7) / 8) true al then .ub
      else .ok (R, W1.store pv vv w init)

/-- `llvm.lifetime.end`: the object's lifetime ends (`Stmt::Free`; no check). -/
def sLend (R : SRegs) (W : World) (p : Opnd) : Res World :=
  (sOpnd R 64 p).bind fun pv => .ok { W with mem := W.mem.free (pv % 2 ^ 64) }

def sMemcpy (R : SRegs) (W : World) (d s len : Opnd) (lw : Nat) (move : Bool) : Res World :=
  (sOpnd R 64 d).bind fun dv => (sOpnd R 64 s).bind fun sv => (sOpnd R lw len).bind fun n0 =>
    let n := n0 % 2 ^ lw
    if cpyBad W.mem (dv % 2 ^ 64) (sv % 2 ^ 64) n move then .ub
    else .ok { W with mem := W.mem.copy (dv % 2 ^ 64) (sv % 2 ^ 64) n }

def sMemset (R : SRegs) (W : World) (d b len : Opnd) (lw : Nat) : Res World :=
  (sOpnd R 64 d).bind fun dv => (sOpnd R 8 b).bind fun bv => (sOpnd R lw len).bind fun n0 =>
    let n := n0 % 2 ^ lw
    if n != 0 && accessBad W.mem (dv % 2 ^ 64) n true 1 then .ub
    else .ok { W with mem := W.mem.fill (dv % 2 ^ 64) (bv % 256) n }

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
  | .store w v p al => sStoreR ω R W w v p al
  | .gep d inb base ix =>
    (sOpnd R 64 base).bind fun b => (sIdxVals R ix).bind fun vs =>
      (gepVal W.mem inb b ix vs).bind fun r => .ok (R.set d r, W)
  | .mm d k w a b => (sMMV R k w a b).bind fun v => .ok (R.set d v, W)
  | .un d k w a flag => (sUnV R k w a flag).bind fun v => .ok (R.set d v, W)
  | .expect d w a => (sExpV R w a).bind fun v => .ok (R.set d v, W)
  | .xv d w src idx => (sXvV R w src idx).bind fun v => .ok (R.set d v, W)
  | .ovf d k w a b => (sOvfV R k w a b).bind fun (v, f) =>
    .ok ((R.set (pairReg d 0) v).set (pairReg d 1) f, W)
  | .lstart n p => sStoreR ω R W (8 * n) .undef p 1
  | .lend p => (sLend R W p).bind fun W' => .ok (R, W')
  | .memcpy d s len lw mv => (sMemcpy R W d s len lw mv).bind fun W' => .ok (R, W')
  | .memset d b len lw => (sMemset R W d b len lw).bind fun W' => .ok (R, W')
  | .glob d size al kd ini st =>
    .ok (R.set d (globAlloc ω W size al kd ini st).1, (globAlloc ω W size al kd ini st).2)
  | .pcmp d p a b => (sPcmpV R p a b).bind fun v => .ok (R.set d v, W)

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
    | .ret (some o) => (sOpnd R fr.F.retw o).step fun v =>
      -- PRISM's rule (C17 6.2.4p2): the analysed function returning a pointer
      -- into one of its own stack objects returns a dangling pointer
      if rest.isEmpty && fr.F.retPtr then
        (escHit R v fr.F.escNames).step fun hit =>
          if hit && ptrObj (v % 2 ^ 64) != 0 then .ub else retTo rest (some v) t
      else retTo rest (some v) t
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

/-- A single-result intrinsic on the LangRef side: an undefined strict value
(a poison flag condition, a poison or indeterminate operand) is a poison
result, flagged (poison was created, or was already there). -/
def lOne (S : LSt) (W : World) (d : String) : Res Nat → Res (LSt × World)
  | .ok v => .ok (⟨S.R.set d (.val v), S.c⟩, W)
  | .ub => .ok (⟨S.R.set d .poison, true⟩, W)
  | .stuck => .stuck

/-- A memory intrinsic on the LangRef side: as the strict side on the
registers holding numbers (a poison pointer or length is a use of poison). -/
def lMem (S : LSt) : Res World → Res (LSt × World)
  | .ok W' => .ok (S, W')
  | .ub => .ub
  | .stuck => .stuck

def lStoreR (ω : Nat → Nat) (S : LSt) (W : World) (w : Nat) (v : FOpnd) (p : Opnd) (al : Nat) :
    Res (LSt × World) :=
    (sStoreVal ω (lower S.R) W w v).bind fun (vv, init, W1) =>
      match lOpnd S.R 64 p with
      | .ok (.val pv, _) =>
        if accessBad W1.mem (pv % 2 ^ 64) ((w + 7) / 8) true al then .ub
        else .ok (S, W1.store pv vv w init)
      | .ok (_, _) => .ub
      | .ub => .ub
      | .stuck => .stuck

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
  | .store w v p al => lStoreR ω S W w v p al
  | .gep d inb base ix =>
    -- a poison or indeterminate operand, an overflow, a C bound or leaving the
    -- object: the result is poison (LLVM) or C UB; flagged
    match ((sOpnd (lower S.R) 64 base).bind fun b => (sIdxVals (lower S.R) ix).bind fun vs =>
            gepVal W.mem inb b ix vs) with
    | .ok r => .ok (⟨S.R.set d (.val r), S.c⟩, W)
    | .ub => .ok (⟨S.R.set d (.val 0), true⟩, W)
    | .stuck => .stuck
  | .mm d k w a b => lOne S W d (sMMV (lower S.R) k w a b)
  | .un d k w a flag => lOne S W d (sUnV (lower S.R) k w a flag)
  | .expect d w a => lOne S W d (sExpV (lower S.R) w a)
  | .xv d w src idx => lOne S W d (sXvV (lower S.R) w src idx)
  | .ovf d k w a b =>
    match sOvfV (lower S.R) k w a b with
    | .ok (v, f) => .ok (⟨(S.R.set (pairReg d 0) (.val v)).set (pairReg d 1) (.val f), S.c⟩, W)
    | .ub => .ok (⟨(S.R.set (pairReg d 0) .poison).set (pairReg d 1) .poison, true⟩, W)
    | .stuck => .stuck
  | .lstart n p => lStoreR ω S W (8 * n) .undef p 1
  | .lend p => lMem S (sLend (lower S.R) W p)
  | .memcpy d s len lw mv => lMem S (sMemcpy (lower S.R) W d s len lw mv)
  | .memset d b len lw => lMem S (sMemset (lower S.R) W d b len lw)
  | .glob d size al kd ini st =>
    .ok (⟨S.R.set d (.val (globAlloc ω W size al kd ini st).1), S.c⟩, (globAlloc ω W size al kd ini st).2)
  -- pointers into different objects: C UB (not LLVM poison), flagged like a C array bound
  | .pcmp d p a b => lOne S W d (sPcmpV (lower S.R) p a b)

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
      | .ok (.val v, cv) =>
        if rest.isEmpty && fr.F.retPtr then
          (escHit (lower S.R) v fr.F.escNames).lstep fun hit =>
            if hit && ptrObj (v % 2 ^ 64) != 0 then .ub else lRetTo rest (some v) t (S.c || cv)
        else lRetTo rest (some v) t (S.c || cv)
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
