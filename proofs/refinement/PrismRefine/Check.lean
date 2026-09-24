/-
PRISM refinement — correspondence checking (roadmap 8.2 "Property
instrumentation", 2.4 translation validation).

Input: the `.pirl` records `src/prism/pir/export_lean.cpp` writes — for each
function, the LLVM side in the fragment syntax and what the C++ translator
produced.  For each record the checker runs the Lean translator
(`PrismRefine.translate`, the one the refinement theorems are proved about)
on the LLVM side and demands the C++ PIR be *exactly* its output: the same
variables and widths, blocks, phis, statements, operators, arguments, check
property names and taxonomy classes, and terminators.  When it is, every
theorem in `Refine.lean` holds of the PIR the C++ engine actually verified.

Verdicts per function:
* `agree`       — C++ PIR = Lean translation (theorems apply);
* `agree-reject`— both translators refuse the function;
* `outside`     — the function is not in the modelled fragment (the reason
                  is printed; nothing is claimed);
* `MISMATCH`    — the C++ translator and the proved translator differ (the
                  checker exits non-zero).
-/
import PrismRefine.Translate
import PrismRefine.XValid

namespace PrismRefine.Check

open PrismSem

def words (s : String) : List String :=
  (s.splitOn " ").filter (· ≠ "")

def nat? (s : String) : Except String Nat :=
  match s.toNat? with
  | some n => .ok n
  | none => .error s!"bad number '{s}'"

def binOp? : String → Option BinOp
  | "add" => some .add | "sub" => some .sub | "mul" => some .mul
  | "udiv" => some .udiv | "sdiv" => some .sdiv | "urem" => some .urem | "srem" => some .srem
  | "shl" => some .shl | "lshr" => some .lshr | "ashr" => some .ashr
  | "and" => some .and | "or" => some .or | "xor" => some .xor
  | _ => none

def pred? : String → Option Pred
  | "eq" => some .eq | "ne" => some .ne | "ult" => some .ult | "ule" => some .ule
  | "ugt" => some .ugt | "uge" => some .uge | "slt" => some .slt | "sle" => some .sle
  | "sgt" => some .sgt | "sge" => some .sge
  | _ => none

def castK? : String → Option CastK
  | "zext" => some .zext | "sext" => some .sext | "trunc" => some .trunc
  | _ => none

/-- `prism::pir::op_name` → `POp` (fragment operators only). -/
def pop? (s : String) : Option POp :=
  match binOp? s, pred? s, castK? s with
  | some o, _, _ => some (.bin o)
  | _, some p, _ => some (.cmp p)
  | _, _, some k => some (.cast k)
  | _, _, _ =>
    match s with
    | "select" => some .select
    | "copy" => some .copy
    | "obj.size" => some .objSize | "obj.live" => some .objLive
    | "obj.kind" => some .objKind | "obj.align" => some .objAlign
    | "sadd.ovf" => some (.ovf .sadd) | "ssub.ovf" => some (.ovf .ssub) | "smul.ovf" => some (.ovf .smul)
    | "uadd.ovf" => some (.ovf .uadd) | "usub.ovf" => some (.ovf .usub) | "umul.ovf" => some (.ovf .umul)
    | "sdiv.ovf" => some .sdivOvf
    | "shift.oob" => some .shiftOob
    | "shl.signed.ovf" => some .shlSOvf
    | "shl.nsw.ovf" => some .shlNswOvf
    | "shl.nuw.ovf" => some .shlNuwOvf
    | "lshr.inexact" => some .lostBitsL
    | "ashr.inexact" => some .lostBitsA
    | "udiv.inexact" => some .inexactU
    | "sdiv.inexact" => some .inexactS
    | "smax" => some (.mm .smax) | "smin" => some (.mm .smin)
    | "umax" => some (.mm .umax) | "umin" => some (.mm .umin)
    | "abs" => some (.un .abs) | "ctlz" => some (.un .ctlz) | "cttz" => some (.un .cttz)
    | "ctpop" => some (.un .ctpop) | "bswap" => some (.un .bswap)
    | _ => none

def opnd? (s : String) : Except String Opnd :=
  if s == "poison" then .ok .poison
  else if s == "undef" then
    .error "undef operand outside freeze (a set of values each use picks from; not modelled)"
  else if s.startsWith "%" then .ok (.reg (s.drop 1).toString)
  else if s.startsWith "#" then do
    let n ← nat? (s.drop 1).toString
    pure (.const n)
  else .error s!"bad operand '{s}'"

/-- `c<w>:<bits>` or `v<index>:<w>`. -/
def arg? (s : String) : Except String Arg := do
  match (s.drop 1).toString.splitOn ":" with
  | [x, y] =>
    let a ← nat? x
    let b ← nat? y
    if s.startsWith "c" then pure (.c a b)
    else if s.startsWith "v" then pure (.v a b)
    else throw s!"bad argument '{s}'"
  | _ => throw s!"bad argument '{s}'"

def flags? (s : String) : Except String LFlags :=
  if s == "-" then .ok {} else
  (s.splitOn ",").foldlM (fun (fl : LFlags) f =>
    match f with
    | "nsw" => .ok { fl with nsw := true }
    | "nuw" => .ok { fl with nuw := true }
    | "exact" => .ok { fl with exact := true }
    | "disjoint" => .ok { fl with disjoint := true }
    | "csigned" => .ok { fl with csigned := true }
    | _ => .error s!"unknown flag {f}") {}

def reg? (s : String) : Except String String :=
  if s.startsWith "%" then .ok (s.drop 1).toString else .error s!"bad register '{s}'"

def pairs {α : Type} : List α → List (α × α)
  | a :: b :: t => (a, b) :: pairs t
  | _ => []

/-- One exported record. -/
structure Record where
  name : String
  llvm : Except String LFunc      -- error: `L unsupported` or a parse error
  ll : List (List String)         -- the raw `L` lines (extended fragment)
  cxx : Except String PFunc       -- error: C++ `status unencoded <reason>` (prefixed) or parse error
  deriving Inhabited

/-- Parse the `L ...` lines. -/
def parseLlvm (name : String) (ls : List (List String)) : Except String LFunc := do
  let mut params : List (String × Nat) := []
  let mut retw := 0
  -- blocks in reverse, each with reversed phis / insts
  let mut blocks : List LBlock := []
  let mut cur : Option (String × List PhiI × List Inst) := none
  for l in ls do
    match l with
    | "unsupported" :: why => throw ("unsupported: " ++ " ".intercalate why)
    | ["depth", _] => pure ()
    | "params" :: _ :: rest =>
      params ← (pairs rest).mapM (fun (n, w) => do pure (n, ← nat? w))
    | ["ret", w] =>
      if cur.isNone then retw ← nat? w
      else
        let (bn, ps, is) := cur.get!
        let t ← if w == "-" then pure (LTerm.ret none) else do pure (LTerm.ret (some (← opnd? w)))
        blocks := ⟨bn, ps.reverse, is.reverse, t⟩ :: blocks
        cur := none
    | ["block", n] =>
      if cur.isSome then throw s!"block {n}: previous block has no terminator"
      cur := some (n, [], [])
    | "phi" :: d :: w :: _ :: inc =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "phi outside a block"
      let incs ← (pairs inc).mapM (fun (o, p) => do pure ((← opnd? o), p))
      cur := some (bn, ⟨← reg? d, ← nat? w, incs⟩ :: ps, is)
    | ["bin", d, op, fl, w, a, b] =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "instruction outside a block"
      let o ← match binOp? op with | some o => pure o | none => throw s!"binop {op}"
      cur := some (bn, ps, .bin (← reg? d) o (← flags? fl) (← nat? w) (← opnd? a) (← opnd? b) :: is)
    | ["icmp", d, p, w, a, b] =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "instruction outside a block"
      let q ← match pred? p with | some q => pure q | none => throw s!"icmp {p}"
      cur := some (bn, ps, .icmp (← reg? d) q (← nat? w) (← opnd? a) (← opnd? b) :: is)
    | ["select", d, w, c, a, b] =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "instruction outside a block"
      cur := some (bn, ps, .select (← reg? d) (← nat? w) (← opnd? c) (← opnd? a) (← opnd? b) :: is)
    | ["uninit", d, w] =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "instruction outside a block"
      cur := some (bn, ps, .uninit (← reg? d) (← nat? w) :: is)
    | ["cast", d, k, nn, fw, tw, a] =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw "instruction outside a block"
      let ck ← match castK? k with | some c => pure c | none => throw s!"cast {k}"
      cur := some (bn, ps, .cast (← reg? d) ck (nn == "1") (← nat? fw) (← nat? tw) (← opnd? a) :: is)
    | _ =>
      let (bn, ps, is) ← match cur with | some c => pure c | none => throw s!"bad line {l}"
      let t ← match l with
        | ["br", t] => pure (LTerm.br t)
        | ["cbr", c, t, f] => do pure (LTerm.cbr (← opnd? c) t f)
        | ["unreachable"] => pure LTerm.unreachable
        | _ => throw s!"bad line {l}"
      blocks := ⟨bn, ps.reverse, is.reverse, t⟩ :: blocks
      cur := none
  if cur.isSome then throw "last block has no terminator"
  pure { name := name, params := params, retw := retw, blocks := blocks.reverse }

/-- Parse the `P ...` lines. -/
def parsePir (ls : List (List String)) : Except String PFunc := do
  let mut vars : List Nat := []
  let mut params : List Nat := []
  let mut retw := 0
  let mut blocks : List PBlock := []
  let mut cur : Option (List PPhi × List PStmt) := none
  for l in ls do
    match l with
    | "vars" :: _ :: ws => vars ← ws.mapM nat?
    | "params" :: _ :: ps => params ← ps.mapM nat?
    | ["retw", w] => retw ← nat? w
    | ["block"] =>
      if cur.isSome then throw "PIR block without terminator"
      cur := some ([], [])
    | "phi" :: d :: _ :: inc =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "phi outside a block"
      let incs ← (pairs inc).mapM (fun (p, a) => do pure ((← nat? p), (← arg? a)))
      cur := some (⟨← nat? d, incs⟩ :: ps, ss)
    | "assign" :: d :: op :: _ :: args =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      let o ← match pop? op with | some o => pure o | none => throw s!"PIR operator {op} is outside the fragment"
      cur := some (ps, .assign (← nat? d) o (← args.mapM arg?) :: ss)
    | ["havoc", d] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .havoc (← nat? d) :: ss)
    | ["check", a, prop, cls] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .check (← arg? a) prop cls :: ss)
    | ["assume", a] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .assume (← arg? a) :: ss)
    | ["alloc", d, sz, kind, init, al] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .alloc (← nat? d) (← arg? sz) (← nat? kind) (← nat? init) (← nat? al) :: ss)
    | ["load", d, u, d3, p, tag] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      if d3 != "-1" || tag != "0" then throw "PIR load with an effective-type tag"
      cur := some (ps, .load (← nat? d) (← nat? u) (← arg? p) :: ss)
    | ["free", p] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .free (← arg? p) :: ss)
    | ["memcpy", d, sp, n] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .memcpy (← arg? d) (← arg? sp) (← arg? n) :: ss)
    | ["memset", d, b, n] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      cur := some (ps, .memset (← arg? d) (← arg? b) (← arg? n) :: ss)
    | ["store", p, v, init, tag] =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw "statement outside a block"
      if tag != "0" then throw "PIR store with an effective-type tag"
      cur := some (ps, .store (← arg? p) (← arg? v) (← arg? init) :: ss)
    | _ =>
      let (ps, ss) ← match cur with | some c => pure c | none => throw s!"bad line {l}"
      let t ← match l with
        | ["jmp", t] => do pure (PTerm.jmp (← nat? t))
        | ["br", c, t, f] => do pure (PTerm.br (← arg? c) (← nat? t) (← nat? f))
        | ["ret", "-"] => pure (PTerm.ret none)
        | ["ret", a] => do pure (PTerm.ret (some (← arg? a)))
        | ["stop"] => pure PTerm.stop
        | _ => throw s!"bad line {l}"
      blocks := ⟨ps.reverse, ss.reverse, t⟩ :: blocks
      cur := none
  if cur.isSome then throw "last PIR block has no terminator"
  pure { vars := vars, params := params, retw := retw, blocks := blocks.reverse }

/-- Split a `.pirl` file into records. -/
def parseFile (text : String) : List Record := Id.run do
  let mut out : List Record := []
  let mut name := ""
  let mut ll : List (List String) := []
  let mut pl : List (List String) := []
  let mut status : Option String := none
  for raw in text.splitOn "\n" do
    let w := words raw
    match w with
    | "func" :: n :: _ => name := n; ll := []; pl := []; status := none
    | "L" :: rest => ll := rest :: ll
    | "P" :: rest => pl := rest :: pl
    | "status" :: "ok" :: _ => status := some ""
    | "status" :: _ :: why => status := some (" ".intercalate why)
    | ["end"] =>
      let llvm := parseLlvm name ll.reverse
      let cxx : Except String PFunc :=
        match status with
        | some "" => parsePir pl.reverse
        | some why => .error ("C++ refused: " ++ why)
        | none => .error "no status line"
      out := { name := name, llvm := llvm, ll := ll.reverse, cxx := cxx } :: out
    | _ => pure ()
  return out.reverse

/-- First difference between two PIR functions (for the report). -/
def diff (a b : PFunc) : String := Id.run do
  if a.vars != b.vars then return s!"vars: lean {a.vars} vs c++ {b.vars}"
  if a.params != b.params then return s!"params: lean {a.params} vs c++ {b.params}"
  if a.retw != b.retw then return s!"ret width: lean {a.retw} vs c++ {b.retw}"
  if a.blocks.length != b.blocks.length then
    return s!"block count: lean {a.blocks.length} vs c++ {b.blocks.length}"
  for (x, y, i) in (a.blocks.zip b.blocks).zipIdx.map (fun ((x, y), i) => (x, y, i)) do
    if x.phis != y.phis then return s!"bb{i} phis: lean {repr x.phis} vs c++ {repr y.phis}"
    if x.stmts.length != y.stmts.length then
      return s!"bb{i} statement count: lean {x.stmts.length} vs c++ {y.stmts.length}"
    for (s, t, j) in (x.stmts.zip y.stmts).zipIdx.map (fun ((s, t), j) => (s, t, j)) do
      if s != t then return s!"bb{i} statement {j}: lean {repr s} vs c++ {repr t}"
    if x.term != y.term then return s!"bb{i} terminator: lean {repr x.term} vs c++ {repr y.term}"
  return "equal"

/-! ### The extended fragment (`XLlvm.lean`): `freeze`, `undef`, calls -/

def mmK? : String → Except String MMK
  | "smax" => .ok .smax | "smin" => .ok .smin | "umax" => .ok .umax | "umin" => .ok .umin
  | k => .error s!"intrinsic {k}"

def unK? : String → Except String UnK
  | "abs" => .ok .abs | "ctlz" => .ok .ctlz | "cttz" => .ok .cttz
  | "ctpop" => .ok .ctpop | "bswap" => .ok .bswap
  | k => .error s!"intrinsic {k}"

def ovfK? : String → Except String OvfOp
  | "sadd" => .ok .sadd | "uadd" => .ok .uadd | "ssub" => .ok .ssub
  | "usub" => .ok .usub | "smul" => .ok .smul | "umul" => .ok .umul
  | k => .error s!"overflow intrinsic {k}"

def fopnd? (s : String) : Except String FOpnd :=
  if s == "undef" then .ok .undef else do pure (.o (← opnd? s))

/-- `getelementptr` indices: `f OPND W SCALE`, `s OFF`, `a OPND W SCALE N USE`. -/
def gidx? : List String → Except String (List GIdx)
  | [] => .ok []
  | "f" :: o :: w :: sc :: t => do pure (.first (← opnd? o) (← nat? w) (← nat? sc) :: (← gidx? t))
  | "s" :: off :: t => do pure (.field (← nat? off) :: (← gidx? t))
  | "a" :: o :: w :: sc :: n :: u :: t => do
    pure (.arr (← opnd? o) (← nat? w) (← nat? sc) (← nat? n) (← nat? u) :: (← gidx? t))
  | t => .error s!"bad getelementptr index {t}"

/-- A function being parsed: name, params, ret width, finished blocks
(reversed), the open block (name, phis, finished segments, instructions; all
reversed). -/
structure XPF where
  name : String
  params : List (String × Nat) := []
  retw : Nat := 0
  blocks : List XBlock := []
  cur : Option (String × List PhiI × List (List SInst × CallI) × List SInst) := none

def XPF.close (f : XPF) : Except String XFunc := do
  if f.cur.isSome then throw s!"{f.name}: last block has no terminator"
  pure { name := f.name, params := f.params, retw := f.retw, blocks := f.blocks.reverse }

/-- Parse the `L ...` lines of a record in the extended syntax: the analysed
function, then an `L fn NAME` section per function its calls reach. -/
def parseXLlvm (name : String) (ls : List (List String)) :
    Except String (XMod × XFunc × Nat) := do
  let mut depth := 4
  let mut done : List XFunc := []
  let mut f : XPF := { name := name }
  for l in ls do
    let inBlock : Except String (String × List PhiI × List (List SInst × CallI) × List SInst) :=
      match f.cur with
      | some c => pure c
      | none => throw s!"bad line {l}"
    match l with
    | "unsupported" :: why => throw ("unsupported: " ++ " ".intercalate why)
    | ["depth", d] => depth ← nat? d
    | ["fn", n] =>
      done := (← f.close) :: done
      f := { name := n }
    | "params" :: _ :: rest =>
      let ps ← (pairs rest).mapM (fun (n, w) => do pure (n, ← nat? w))
      f := { f with params := ps }
    | ["block", n] =>
      if f.cur.isSome then throw s!"block {n}: previous block has no terminator"
      f := { f with cur := some (n, [], [], []) }
    | "phi" :: d :: w :: _ :: inc =>
      let (bn, ps, sg, is) ← inBlock
      let incs ← (pairs inc).mapM (fun (o, p) => do pure ((← opnd? o), p))
      f := { f with cur := some (bn, ⟨← reg? d, ← nat? w, incs⟩ :: ps, sg, is) }
    | ["bin", d, op, fl, w, a, b] =>
      let (bn, ps, sg, is) ← inBlock
      let o ← match binOp? op with | some o => pure o | none => throw s!"binop {op}"
      f := { f with cur := some (bn, ps, sg,
        .i (.bin (← reg? d) o (← flags? fl) (← nat? w) (← opnd? a) (← opnd? b)) :: is) }
    | ["icmp", d, p, w, a, b] =>
      let (bn, ps, sg, is) ← inBlock
      let q ← match pred? p with | some q => pure q | none => throw s!"icmp {p}"
      f := { f with cur := some (bn, ps, sg, .i (.icmp (← reg? d) q (← nat? w) (← opnd? a) (← opnd? b)) :: is) }
    | ["select", d, w, c, a, b] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg,
        .i (.select (← reg? d) (← nat? w) (← opnd? c) (← opnd? a) (← opnd? b)) :: is) }
    | ["uninit", d, w] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .i (.uninit (← reg? d) (← nat? w)) :: is) }
    | ["cast", d, k, nn, fw, tw, a] =>
      let (bn, ps, sg, is) ← inBlock
      let ck ← match castK? k with | some c => pure c | none => throw s!"cast {k}"
      f := { f with cur := some (bn, ps, sg,
        .i (.cast (← reg? d) ck (nn == "1") (← nat? fw) (← nat? tw) (← opnd? a)) :: is) }
    | ["freeze", d, w, a] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .freeze (← reg? d) (← nat? w) (← fopnd? a) :: is) }
    | ["alloca", d, sz, al] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .alloca (← reg? d) (← nat? sz) (← nat? al) :: is) }
    | ["load", d, w, p, al] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .load (← reg? d) (← nat? w) (← opnd? p) (← nat? al) :: is) }
    | ["store", w, v, p, al] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .store (← nat? w) (← fopnd? v) (← opnd? p) (← nat? al) :: is) }
    | "gep" :: d :: inb :: base :: _ :: rest =>
      let (bn, ps, sg, is) ← inBlock
      let ix ← gidx? rest
      f := { f with cur := some (bn, ps, sg, .gep (← reg? d) (inb == "1") (← opnd? base) ix :: is) }
    | ["mm", d, k, w, a, b] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .mm (← reg? d) (← mmK? k) (← nat? w) (← opnd? a) (← opnd? b) :: is) }
    | ["un", d, k, w, a, fl] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .un (← reg? d) (← unK? k) (← nat? w) (← opnd? a) (fl == "1") :: is) }
    | ["expect", d, w, a] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .expect (← reg? d) (← nat? w) (← opnd? a) :: is) }
    | ["ovf", d, k, w, a, b] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .ovf (← reg? d) (← ovfK? k) (← nat? w) (← opnd? a) (← opnd? b) :: is) }
    | ["xv", d, w, src, idx] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .xv (← reg? d) (← nat? w) (← reg? src) (← nat? idx) :: is) }
    | ["lstart", n, p] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .lstart (← nat? n) (← opnd? p) :: is) }
    | ["lend", p] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .lend (← opnd? p) :: is) }
    | ["memcpy", d, sp, n, lw, mv] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg,
        .memcpy (← opnd? d) (← opnd? sp) (← opnd? n) (← nat? lw) (mv == "1") :: is) }
    | ["memset", d, b, n, lw] =>
      let (bn, ps, sg, is) ← inBlock
      f := { f with cur := some (bn, ps, sg, .memset (← opnd? d) (← opnd? b) (← opnd? n) (← nat? lw) :: is) }
    | "call" :: d :: rw :: callee :: _ :: args =>
      let (bn, ps, sg, is) ← inBlock
      let dst ← if d == "-" then pure none else do pure (some (← reg? d))
      let as ← (pairs args).mapM (fun (o, w) => do pure ((← opnd? o), ← nat? w))
      let cl : CallI := { dst := dst, rw := ← nat? rw, f := callee, args := as }
      f := { f with cur := some (bn, ps, (is.reverse, cl) :: sg, []) }
    | ["ret", w] =>
      match f.cur with
      | none => f := { f with retw := ← nat? w }
      | some (bn, ps, sg, is) =>
        let t ← if w == "-" then pure (LTerm.ret none) else do pure (LTerm.ret (some (← opnd? w)))
        f := { f with blocks := ⟨bn, ps.reverse, sg.reverse, is.reverse, t⟩ :: f.blocks, cur := none }
    | _ =>
      let (bn, ps, sg, is) ← inBlock
      let t ← match l with
        | ["br", t] => pure (LTerm.br t)
        | ["cbr", c, t, e] => do pure (LTerm.cbr (← opnd? c) t e)
        | ["unreachable"] => pure LTerm.unreachable
        | _ => throw s!"bad line {l}"
      f := { f with blocks := ⟨bn, ps.reverse, sg.reverse, is.reverse, t⟩ :: f.blocks, cur := none }
  let last ← f.close
  let fs := (last :: done).reverse
  match fs with
  | top :: _ => pure (fs, top, depth)
  | [] => throw "no function"

inductive Verdict where
  | agree | agreeExt | agreeReject | outside | mismatch
  deriving DecidableEq

def Verdict.tag : Verdict → String
  | .agree => "agree"
  | .agreeExt => "agree-ext"
  | .agreeReject => "agree-reject"
  | .outside => "outside"
  | .mismatch => "MISMATCH"

def check (r : Record) : Verdict × String :=
  match r.llvm with
  | .error e => (.outside, e)
  | .ok F =>
    match translate F, r.cxx with
    | .ok P, .ok Q => if P == Q then (.agree, "") else (.mismatch, diff P Q)
    | .ok _, .error e => (.mismatch, s!"Lean translates it, C++ does not ({e})")
    | .error e, .error _ => (.agreeReject, e)
    | .error e, .ok _ =>
      if e.startsWith "outside fragment" then (.outside, e)
      else (.mismatch, s!"C++ translates it, Lean refuses ({e})")

/-- The extended fragment: the C++ PIR must be the extended translator's
output, and the certificate must pass `validB` (the hypothesis of the
extended theorems, `XSound.lean`). -/
def checkX (r : Record) : Verdict × String :=
  match parseXLlvm r.name r.ll with
  | .error e => (.outside, e)
  | .ok (M, F, depth) =>
    match translateX M depth F, r.cxx with
    | .ok (P, C), .ok Q =>
      if P == Q then
        if validB M F P C then (.agreeExt, "")
        else (.mismatch, "the extended translation's certificate does not check (validB)")
      else (.mismatch, diff P Q)
    | .ok _, .error e => (.mismatch, s!"Lean translates it, C++ does not ({e})")
    | .error e, .error _ => (.agreeReject, e)
    | .error e, .ok _ =>
      if e.startsWith "outside fragment" then (.outside, e)
      else (.mismatch, s!"C++ translates it, Lean refuses ({e})")

/-- The base fragment first (unconditional theorems, `Sound.lean`); what it
leaves outside, the extended fragment. -/
def checkAll (r : Record) : Verdict × String :=
  match check r with
  | (.outside, e) =>
    match checkX r with
    | (.outside, e') => (.outside, if e' == e then e else s!"{e'} (base fragment: {e})")
    | v => v
  | v => v

end PrismRefine.Check
