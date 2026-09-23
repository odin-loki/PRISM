/-
PRISM refinement, extended fragment — the translator, mirroring
`src/prism/pir/translate.cpp` on `freeze`, `undef` and direct calls.

`translate.cpp` inlines every direct call to a function of the module
(`Tr::inline_call`): the callee gets a fresh *instance* (its own blocks and
variables, allocated when the call is reached), its parameters are bound to
the caller's argument `Arg`s (no copy), the caller's block is split at the
call (a new *continuation* block receives the rest), and every `ret` of the
callee jumps to the continuation, whose phis collect the returned value and
its uninitialised-bytes mask (`_retu`, never set in this fragment).

This file has two layers:

* **Per-segment translation** (`trOpndX`, `trSInstX`, `trPhisX`, `trArgsX`,
  `trTermX`): exactly the statements `translate.cpp` emits for the
  instructions of one block segment, in a *context* — the instance's map
  from LLVM names to `Arg`s (`env`, parameters bound to the caller's
  arguments), its uninitialised-read shadows (`sh`), the range `[lo, hi)` of
  the variables it owns and the variable widths.  These are the functions
  the proofs (`XRefine.lean`) are about.  They re-check locally what the
  proofs need (a result variable is owned by the instance and referenced by
  no other name, every reference is below `hi`); on the translator's own
  output those checks always hold.
* **The mirror** (`translateX`): the stateful translator, allocating
  variables and blocks in `translate.cpp`'s order (parameters; per instance
  its blocks, its SSA results, its shadow variables; temporaries as
  instructions are translated; at a call the continuation block, the result
  and `_retu` variables, then the callee instance).  It returns the PIR
  function and a *certificate*: for each instance its context and, per LLVM
  block and segment, the PIR block and the first temporary.

The proofs do not reason about the mirror's bookkeeping.  `XValid.lean`
checks, from the certificate alone, that every PIR block is the per-segment
translation of its segment and that the instances' variables are disjoint
as the proof needs; the refinement theorem is proved for every
(function, PIR, certificate) that passes the check, and `pir_lean_check`
runs the check on every function it reports as `agree-ext`.
-/
import PrismRefine.Translate
import PrismRefine.XLlvm

namespace PrismRefine

open PrismSem

/-! ## Contexts -/

/-- First binding of a name. -/
def look {α : Type} : List (String × α) → String → Option α
  | [], _ => none
  | (m, a) :: t, n => if m = n then some a else look t n

def Arg.setW : Arg → Nat → Arg
  | .c _ b, w => .c w b
  | .v i _, w => .v i w

def Arg.var? : Arg → Option Nat
  | .c _ _ => none
  | .v i _ => some i

/-- Every variable the argument reads is below `m`. -/
def Arg.lt (m : Nat) : Arg → Bool
  | .c _ _ => true
  | .v i _ => decide (i < m)

/-- An instance's translation context. -/
structure Ctx where
  env : List (String × Arg)
  sh : List (String × Arg)
  lo : Nat
  hi : Nat
  vars : List Nat
  /-- an inlined instance (its `alloca`s would end at its `ret`: outside the fragment) -/
  inlined : Bool := false
  deriving Repr, Inhabited

/-- The uninitialised-read shadow checked at a use of `n` (`Tr::operand`:
only a variable has one). -/
def Ctx.shOf (c : Ctx) (n : String) : Option Arg :=
  match look c.env n with
  | some (.v _ _) => look c.sh n
  | _ => none

/-- No binding other than `d`'s, and no shadow, reads variable `i`. -/
def Ctx.uniq (c : Ctx) (d : String) (i : Nat) : Bool :=
  c.env.all (fun (m, a) => m == d || a.var? != some i) && c.sh.all (fun (_, s) => s.var? != some i)

/-- No binding, and no shadow other than `d`'s, reads variable `s`. -/
def Ctx.uniqS (c : Ctx) (d : String) (s : Nat) : Bool :=
  c.env.all (fun (_, a) => a.var? != some s) && c.sh.all (fun (m, a) => m == d || a.var? != some s)

/-- The variable of a result: owned by the instance, width `w`, read by no
other name. -/
def dstX (c : Ctx) (d : String) (w : Nat) : Except String Nat :=
  match look c.env d with
  | some (.v i w') =>
    if w' = w ∧ c.lo ≤ i ∧ i < c.hi ∧ c.vars.getD i 0 = w ∧ c.uniq d i = true then .ok i
    else .error s!"outside fragment: result %{d} is not an owned variable"
  | _ => .error s!"UNENCODED: result %{d}"

/-! ## Per-segment translation -/

/-- `Tr::operand` in a context. -/
def trOpndX (c : Ctx) (w : Nat) (keep : Bool) (k : Nat) : Opnd → Except String (List PStmt × List Nat × Arg)
  | .const b => .ok ([], [], .c w (b % 2 ^ w))
  | .reg n =>
    match look c.env n with
    | some a =>
      if a.lt c.hi then .ok (shChecks (c.shOf n), [], if keep then a else a.setW w)
      else .error s!"outside fragment: %{n} reads above its instance"
    | none => .error s!"UNENCODED: value %{n}"
  | .poison => .ok ([.check (.c 1 1) "poison" "UB-POISON", .havoc k], [w], .v k w)

/-- The operand of `freeze`: `undef` is a `havoc` temporary (`Tr::operand`,
`ir::Value::Undef`). -/
def trFOpnd (c : Ctx) (w : Nat) (k : Nat) : FOpnd → Except String (List PStmt × List Nat × Arg)
  | .undef => .ok ([.havoc k], [w], .v k w)
  | .o o => trOpndX c w false k o

/-- `Tr::binop` / `Tr::inst` for the base instructions, in a context. -/
def trInstX (c : Ctx) (k : Nat) : Inst → Except String (List PStmt × List Nat)
  | .bin d op fl w a b => do
    need (okW w) "UNENCODED: width"
    need (flagsOk op fl) "outside fragment: flags"
    let (sa, ta, A) ← trOpndX c w false k a
    let (sb, tb, B) ← trOpndX c w false (k + ta.length) b
    let (sc, tc) := emitAll (k + ta.length + tb.length) (checks op fl w A B)
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ sc ++ [.assign i (.bin op) [A, B]], ta ++ tb ++ tc)
  | .icmp d p w a b => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w false k a
    let (sb, tb, B) ← trOpndX c w false (k + ta.length) b
    let i ← dstX c d 1
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ [.assign i (.cmp p) [A, B]], ta ++ tb)
  | .select d w cnd a b => do
    need (okW w) "UNENCODED: width"
    let (sc, tc, C) ← trOpndX c 1 true k cnd
    let (sa, ta, A) ← trOpndX c w false (k + tc.length) a
    let (sb, tb, B) ← trOpndX c w false (k + tc.length + ta.length) b
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sc ++ sa ++ sb ++ [.assign i .select [C, A, B]], tc ++ ta ++ tb)
  | .cast d ck nneg fw tw a => do
    need (okW fw && okW tw) "UNENCODED: width"
    need (!nneg || ck == .zext) "outside fragment: flags"
    let (sa, ta, A) ← trOpndX c fw false k a
    let (sc, tc) := emitAll (k + ta.length)
      (opt nneg (.p (.cmp .slt) A (.c fw 0) "nneg" "UB-POISON"))
    let i ← dstX c d tw
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sc ++ [.assign i (.cast ck) [A]], ta ++ tc)
  | .uninit d w => do
    need (okW w) "UNENCODED: width"
    let i ← dstX c d w
    need (look c.sh d == some (.c 1 1)) "outside fragment: uninit marker without its shadow"
    pure ([.havoc i], [])

/-! ### Memory (`translate_mem.cpp`) -/

/-- `c64(v)`. -/
def c64 (v : Nat) : Arg := .c 64 (v % 2 ^ 64)

/-- `MemTr::access_checks` (no guard), first part: the object, offset,
kind, liveness and size of the pointer (temporaries `k … k+4`), then the
null, wild, freed and bounds checks. -/
def accChk0 (k : Nat) (P : Arg) (n : Nat) (write : Bool) : List PStmt :=
  let o := k
  let f := k + 1
  let kd := k + 2
  let lv := k + 3
  let sz := k + 4
  [.assign o (.bin .lshr) [P, c64 48], .assign f (.bin .and) [P, c64 (2 ^ 48 - 1)],
   .assign kd .objKind [P], .assign lv .objLive [P], .assign sz .objSize [P],
   .assign (k + 5) (.cmp .eq) [.v o 64, c64 0], .check (.v (k + 5) 1) "null" "PTR-NULL-DEREF",
   .assign (k + 6) (.cmp .ne) [.v o 64, c64 0], .assign (k + 7) (.cmp .eq) [.v kd 8, .c 8 0],
   .assign (k + 8) (.bin .and) [.v (k + 6) 1, .v (k + 7) 1],
   .check (.v (k + 8) 1) "wild" "PTR-INVALID-DEREF",
   .assign (k + 9) (.cmp .ne) [.v kd 8, .c 8 0], .assign (k + 10) (.cmp .eq) [.v lv 1, .c 1 0],
   .assign (k + 11) (.bin .and) [.v (k + 9) 1, .v (k + 10) 1], .check (.v (k + 11) 1) "uaf" "MEM-UAF",
   .assign (k + 12) (.bin .add) [.v f 64, c64 n], .assign (k + 13) (.cmp .ugt) [.v (k + 12) 64, .v sz 64],
   .assign (k + 14) (.ovf .uadd) [.v f 64, c64 n], .assign (k + 15) (.bin .or) [.v (k + 13) 1, .v (k + 14) 1],
   .assign (k + 16) (.bin .and) [.v (k + 3) 1, .v (k + 15) 1],
   .check (.v (k + 16) 1) (if write then "oob-write" else "oob-read")
     (if write then "MEM-OOB-WRITE" else "MEM-OOB-READ")]

/-- The alignment check (temporaries `a … a+5`; the offset is `k+1`, the
liveness `k+3`). -/
def accAlign (k a : Nat) (P : Arg) (al : Nat) : List PStmt :=
  [.assign a (.bin .and) [.v (k + 1) 64, c64 (al - 1)], .assign (a + 1) .objAlign [P],
   .assign (a + 2) (.cmp .ne) [.v a 64, c64 0], .assign (a + 3) (.cmp .ult) [.v (a + 1) 64, c64 al],
   .assign (a + 4) (.bin .or) [.v (a + 2) 1, .v (a + 3) 1],
   .assign (a + 5) (.bin .and) [.v (k + 3) 1, .v (a + 4) 1], .check (.v (a + 5) 1) "align" "MEM-MISALIGNED"]

/-- The read-only check of a write (temporaries `j`, `j+1`; the kind is
`k+2`, the liveness `k+3`). -/
def accConst (k j : Nat) : List PStmt :=
  [.assign j (.cmp .eq) [.v (k + 2) 8, .c 8 4], .assign (j + 1) (.bin .and) [.v (k + 3) 1, .v j 1],
   .check (.v (j + 1) 1) "write-const" "MEM-WRITE-CONST"]

def accT0 : List Nat := [64, 64, 8, 1, 64, 1, 1, 1, 1, 1, 1, 1, 64, 1, 1, 1, 1]
def accT1 (al : Nat) : List Nat := if 1 < al then [64, 64, 1, 1, 1, 1] else []
def accT2 (write : Bool) : List Nat := if write then [1, 1] else []

/-- `MemTr::access_checks` (no guard): the checks above, the alignment check
when `al > 1`, the read-only check for a write. -/
def accessChecks (k : Nat) (P : Arg) (n : Nat) (write : Bool) (al : Nat) : List PStmt × List Nat :=
  (accChk0 k P n write ++ (if 1 < al then accAlign k (k + 17) P al else []) ++
    (if write then accConst k (k + 17 + (accT1 al).length) else []),
   accT0 ++ accT1 al ++ accT2 write)

/-- Alignments the fragment accepts: none, or a power of two below `2^32`. -/
def alignOK (al : Nat) : Bool := al == 0 || (List.range 32).any (fun e => al == 2 ^ e)

/-- The value a `store` writes, and its "initialised" argument. -/
def trStoreVal (c : Ctx) (w k : Nat) : FOpnd → Except String (List PStmt × List Nat × Arg × Arg)
  | .undef => .ok ([.havoc k], [w], .v k w, .c 1 0)
  | .o .poison => .ok ([.havoc k], [w], .v k w, .c 1 0)
  | .o (.const b) => .ok ([], [], .c w (b % 2 ^ w), .c 1 1)
  | .o (.reg n) =>
    match look c.env n with
    | some a =>
      if a.lt c.hi then
        match c.shOf n with
        | some s =>
          if s.width == 1 then .ok ([.assign k (.cmp .eq) [s, .c 1 0]], [1], a, .v k 1)
          else .error "outside fragment: shadow width"
        | none => .ok ([], [], a, .c 1 1)
      else .error s!"outside fragment: %{n} reads above its instance"
    | none => .error s!"UNENCODED: value %{n}"

/-- Index operands (`Tr::mem_inst`: each at its own width). -/
def trIdxOps (c : Ctx) : Nat → List GIdx → Except String (List PStmt × List Nat × List Arg)
  | _, [] => .ok ([], [], [])
  | k, g :: t =>
    match g.opnd with
    | some (o, w) => do
      need (okW w) "UNENCODED: width"
      let (s1, t1, A) ← trOpndX c w false k o
      -- a register bound to a constant (an inlined argument) is folded by the
      -- translator: outside the fragment, whose semantics reads it as a register
      match o, A with
      | .reg _, .c _ _ => throw "outside fragment: constant-bound getelementptr index"
      | _, _ => pure ()
      let (s2, t2, As) ← trIdxOps c (k + t1.length) t
      pure (s1 ++ s2, t1 ++ t2, A :: As)
    | none => do
      let (s2, t2, As) ← trIdxOps c k t
      pure (s2, t2, .c 64 0 :: As)

/-- `MemTr::gep`'s running state. -/
structure GSt where
  s : List PStmt
  ts : List Nat
  cst : Int
  var : Option Arg

def inI64 (x : Int) : Bool := decide (-2 ^ 63 ≤ x ∧ x < 2 ^ 63)

/-- `add_var`. -/
def gAddVarT (k : Nat) (g : GSt) (x : Arg) : GSt :=
  match g.var with
  | none => { g with var := some x }
  | some y =>
    let j := k + g.ts.length
    { g with s := g.s ++ [.assign j (.ovf .sadd) [y, x], .check (.v j 1) "ptr-arith" "MEM-PTR-ARITH",
                          .assign (j + 1) (.bin .add) [y, x]],
             ts := g.ts ++ [1, 64], var := some (.v (j + 1) 64) }

/-- The sign extension of a variable index narrower than 64 bits. -/
def gSext (k : Nat) (g : GSt) (A : Arg) : GSt × Arg :=
  match A with
  | .v _ w =>
    if w < 64 then
      let j := k + g.ts.length
      ({ g with s := g.s ++ [.assign j (.cast .sext) [A]], ts := g.ts ++ [64] }, .v j 64)
    else (g, A)
  | .c _ _ => (g, A)

/-- One scaled index. -/
def gTermT (k : Nat) (g : GSt) (A : Arg) (scale : Nat) : Except String GSt :=
  match A with
  | .c w b =>
    let cst := g.cst + (bv 64 (sext64 w b)).toInt * scale
    if inI64 cst then .ok { g with cst := cst } else .error "UNENCODED: constant pointer offset overflows"
  | .v _ _ =>
    let (g, s) := gSext k g A
    if scale = 0 then .ok g
    else if scale = 1 then .ok (gAddVarT k g s)
    else
      let j := k + g.ts.length
      .ok (gAddVarT k { g with s := g.s ++ [.assign j (.ovf .smul) [s, c64 scale],
                                            .check (.v j 1) "ptr-arith" "MEM-PTR-ARITH",
                                            .assign (j + 1) (.bin .mul) [s, c64 scale]],
                               ts := g.ts ++ [1, 64] } (.v (j + 1) 64))

/-- The C array-bound check of an index. -/
def gBoundT (k : Nat) (g : GSt) (A : Arg) (n use : Nat) : GSt :=
  if n = 0 then g
  else
    let (g, s64) : GSt × Arg :=
      match A with
      | .c w b => (g, if w < 64 then c64 (sext64 w b) else A)
      | .v _ _ => gSext k g A
    let j := k + g.ts.length
    let (prop, cls) :=
      if use = 1 then ("oob-read", "MEM-OOB-READ")
      else if use = 2 then ("oob-write", "MEM-OOB-WRITE") else ("ptr-arith", "MEM-PTR-ARITH")
    { g with s := g.s ++ [.assign j (.cmp (if use = 0 then .ugt else .uge)) [s64, c64 n], .check (.v j 1) prop cls],
             ts := g.ts ++ [1] }

def gLoop (k : Nat) : GSt → List (GIdx × Arg) → Except String GSt
  | g, [] => .ok g
  | g, (.first _ _ scale, A) :: t => do gLoop k (← gTermT k g A scale) t
  | g, (.field off, _) :: t => gLoop k { g with cst := g.cst + off } t
  | g, (.arr _ _ scale n use, A) :: t => do
    if !decide (n < 2 ^ 63) then throw "outside fragment: array length"
    gLoop k (← gTermT k (gBoundT k g A n use) A scale) t

/-- The pointer addition and the object checks of `MemTr::gep` (result `i`,
temporaries from `j`). -/
def gFinS (i j : Nat) (B delta : Arg) (inb : Bool) : List PStmt :=
  [.assign i (.bin .add) [B, delta], .assign j (.bin .lshr) [B, c64 48],
   .assign (j + 1) (.bin .lshr) [.v i 64, c64 48]] ++
  (if inb then
    [.assign (j + 2) (.cmp .eq) [.v j 64, c64 0], .assign (j + 3) (.cmp .ne) [delta, c64 0],
     .assign (j + 4) (.bin .and) [.v (j + 2) 1, .v (j + 3) 1],
     .check (.v (j + 4) 1) "ptr-arith" "MEM-PTR-ARITH",
     .assign (j + 5) .objKind [B], .assign (j + 6) .objSize [B],
     .assign (j + 7) (.cmp .ne) [.v (j + 1) 64, .v j 64],
     .assign (j + 8) (.bin .and) [.v i 64, c64 (2 ^ 48 - 1)],
     .assign (j + 9) (.cmp .ugt) [.v (j + 8) 64, .v (j + 6) 64],
     .assign (j + 10) (.bin .or) [.v (j + 7) 1, .v (j + 9) 1],
     .assign (j + 11) (.cmp .ne) [.v (j + 5) 8, .c 8 0],
     .assign (j + 12) (.bin .and) [.v (j + 11) 1, .v (j + 10) 1],
     .check (.v (j + 12) 1) "ptr-arith" "MEM-PTR-ARITH"]
  else
    [.assign (j + 2) (.cmp .ne) [.v j 64, c64 0], .assign (j + 3) (.cmp .ne) [.v (j + 1) 64, .v j 64],
     .assign (j + 4) (.bin .and) [.v (j + 2) 1, .v (j + 3) 1],
     .check (.v (j + 4) 1) "ptr-arith" "MEM-PTR-ARITH"])

def gFinT (inb : Bool) : List Nat :=
  if inb then [64, 64, 1, 1, 1, 8, 64, 1, 64, 1, 1, 1, 1] else [64, 64, 1, 1, 1]

/-- `MemTr::gep` after the loop: the offset, the pointer addition and the
object checks. -/
def gEnd (k i : Nat) (B : Arg) (inb : Bool) (g : GSt) : List PStmt × List Nat :=
  let dc := c64 (g.cst % 2 ^ 64).toNat
  let (g, delta) : GSt × Arg :=
    match g.var with
    | some v => if g.cst ≠ 0 then (let g' := gAddVarT k g dc; (g', g'.var.getD dc)) else (g, v)
    | none => (g, dc)
  if g.var.isNone && g.cst == 0 then (g.s ++ [.assign i .copy [B]], g.ts)
  else (g.s ++ gFinS i (k + g.ts.length) B delta inb, g.ts ++ gFinT inb)

/-- The operands a `getelementptr` reads. -/
def gepUses (base : Opnd) (ix : List GIdx) : List Opnd := base :: ix.filterMap (fun g => g.opnd.map (·.1))

/-- A non-call instruction.  `freeze`: `Op::Copy` of the operand at the
result width (`Tr::inst`). -/
def trSInstX (c : Ctx) (k : Nat) : SInst → Except String (List PStmt × List Nat)
  | .i x => trInstX c k x
  | .freeze d w a => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trFOpnd c w k a
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ [.assign i .copy [A.setW w]], ta)
  | .alloca d size al => do
    need (!c.inlined) "outside fragment: alloca in an inlined function"
    need (decide (size < 2 ^ 47)) "UNENCODED: stack object larger than 2^47 bytes"
    let i ← dstX c d 64
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure ([.alloc i (.c 64 size) 1 0 al], [])
  | .load d w p al => do
    need (okW w) "UNENCODED: width"
    need (alignOK al) "outside fragment: alignment"
    let (sp, tp, P) ← trOpndX c 64 true k p
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    let (sa, ta) := accessChecks (k + tp.length) P ((w + 7) / 8) false al
    let u := k + tp.length + ta.length
    pure (sp ++ sa ++ [.load i u P, .check (.v u 1) "uninit" "UNINIT-READ"], tp ++ ta ++ [1])
  | .store w v p al => do
    need (okW w) "UNENCODED: width"
    need (alignOK al) "outside fragment: alignment"
    let (sv, tv, V, I) ← trStoreVal c w k v
    let (sp, tp, P) ← trOpndX c 64 true (k + tv.length) p
    let (sa, ta) := accessChecks (k + tv.length + tp.length) P ((w + 7) / 8) true al
    pure (sv ++ sp ++ sa ++ [.store P (V.setW w) I], tv ++ tp ++ ta)
  | .gep d inb base ix => do
    need ((gepUses base ix).all (· != .reg d)) "outside fragment: getelementptr reads its own result"
    let (sb, tb, B) ← trOpndX c 64 true k base
    let (si, ti, As) ← trIdxOps c (k + tb.length) ix
    let i ← dstX c d 64
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    let k' := k + tb.length + ti.length
    let g ← gLoop k' { s := [], ts := [], cst := 0, var := none } (ix.zip As)
    let (sg, tg) := gEnd k' i B inb g
    pure (sb ++ si ++ sg, tb ++ ti ++ tg)

def trSInstsX (c : Ctx) : Nat → List SInst → Except String (List PStmt × List Nat)
  | _, [] => .ok ([], [])
  | k, i :: is => do
    let (s1, t1) ← trSInstX c k i
    let (s2, t2) ← trSInstsX c (k + t1.length) is
    pure (s1 ++ s2, t1 ++ t2)

/-! ### Phis of a block's first segment -/

def trIncArgX (c : Ctx) (w : Nat) : Opnd → Except String Arg
  | .const b => .ok (.c w (b % 2 ^ w))
  | .reg n =>
    match look c.env n with
    | some a => if a.lt c.hi then .ok a else .error s!"outside fragment: %{n} reads above its instance"
    | none => .error s!"UNENCODED: value %{n}"
  | .poison => .error "outside fragment: poison incoming value of a phi"

/-- Incoming entries, predecessor = the PIR block that ends the predecessor
LLVM block (`tail`); an unknown predecessor is dropped (`resolve_phis`). -/
def trIncX (G : XFunc) (c : Ctx) (tails : List Nat) (w : Nat) :
    List (Opnd × String) → Except String (List (Nat × Arg))
  | [] => .ok []
  | (o, pr) :: t => do
    let a ← trIncArgX c w o
    let rest ← trIncX G c tails w t
    pure (match lookupBlock G.shape pr with
      | some j =>
        match tails[j]? with
        | some q => (q, a) :: rest
        | none => rest
      | none => rest)

def incShadowX (c : Ctx) : Opnd → Arg
  | .reg m => (c.shOf m).getD (.c 1 0)
  | _ => .c 1 0

def trIncShX (G : XFunc) (c : Ctx) (tails : List Nat) : List (Opnd × String) → List (Nat × Arg)
  | [] => []
  | (o, pr) :: t =>
    match lookupBlock G.shape pr with
    | some j =>
      match tails[j]? with
      | some q => (q, incShadowX c o) :: trIncShX G c tails t
      | none => trIncShX G c tails t
    | none => trIncShX G c tails t

def hasShadowedInputX (c : Ctx) (inc : List (Opnd × String)) : Bool :=
  inc.any fun (o, _) => match o with
    | .reg m => (c.shOf m).isSome
    | _ => false

def trPhiX (G : XFunc) (c : Ctx) (tails : List Nat) (p : PhiI) : Except String (List PPhi) := do
  need (okW p.w) "UNENCODED: width"
  let i ← dstX c p.dst p.w
  let inc ← trIncX G c tails p.w p.inc
  match look c.sh p.dst with
  | some (.v sv _) =>
    need (decide (c.lo ≤ sv ∧ sv < c.hi) && c.uniqS p.dst sv) "outside fragment: shadow variable"
    pure [{ dst := i, inc := inc }, { dst := sv, inc := trIncShX G c tails p.inc }]
  | some (.c _ _) => throw "outside fragment: phi with a constant shadow"
  | none =>
    need (!hasShadowedInputX c p.inc) "outside fragment: uninitialised input without a shadow phi"
    pure [{ dst := i, inc := inc }]

def trPhisX (G : XFunc) (c : Ctx) (tails : List Nat) : List PhiI → Except String (List PPhi)
  | [] => .ok []
  | p :: ps => do
    let q ← trPhiX G c tails p
    let qs ← trPhisX G c tails ps
    pure (q ++ qs)

/-! ### Segment ends -/

/-- Call arguments (`Tr::inline_call`): each a use at its own width, then
given the parameter's width. -/
def trArgsX (c : Ctx) : Nat → List (Opnd × Nat × Nat) → Except String (List PStmt × List Nat × List Arg)
  | _, [] => .ok ([], [], [])
  | k, (o, wo, wp) :: t => do
    let (s1, t1, A) ← trOpndX c wo true k o
    let (s2, t2, As) ← trArgsX c (k + t1.length) t
    pure (s1 ++ s2, t1 ++ t2, A.setW wp :: As)

def targetX (G : XFunc) (heads : List Nat) (t : String) : Except String Nat :=
  match lookupBlock G.shape t with
  | some j =>
    match heads[j]? with
    | some h => .ok h
    | none => .error s!"UNENCODED: branch to unknown block {t}"
  | none => .error s!"UNENCODED: branch to unknown block {t}"

/-- The terminator; `retTo` is the continuation block of an inlined
instance (its `ret` jumps there), `none` for the analysed function.  Also
returns the `Arg` of a returned value (the continuation phi's entry). -/
def trTermX (G : XFunc) (c : Ctx) (heads : List Nat) (retTo : Option Nat) (k : Nat) :
    LTerm → Except String (List PStmt × List Nat × PTerm × Option Arg)
  | .br t => do
    let j ← targetX G heads t
    pure ([], [], .jmp j, none)
  | .cbr cnd t f => do
    let (s, tw, C) ← trOpndX c 1 true k cnd
    let tj ← targetX G heads t
    let fj ← targetX G heads f
    pure (s, tw, .br C tj fj, none)
  | .ret none => do
    need (G.retw == 0) "outside fragment: ret void in a function returning a value"
    pure ([], [], (match retTo with | some cb => .jmp cb | none => .ret none), none)
  | .ret (some o) => do
    need (G.retw != 0) "outside fragment: ret value in a void function"
    let (s, tw, A) ← trOpndX c G.retw true k o
    pure (s, tw, (match retTo with | some cb => .jmp cb | none => .ret (some A)), some A)
  | .unreachable => .ok ([.check (.c 1 1) "unreachable" "CXX-UNREACHABLE"], [], .stop, none)

/-! ## Instances and certificates -/

/-- SSA results of a function, in textual order (`Tr::enter_frame`). -/
def xResultNames (G : XFunc) : List (String × Nat) :=
  G.blocks.flatMap fun B =>
    B.phis.map (fun p => (p.dst, p.w)) ++
    (B.segs.flatMap fun (is, cl) =>
      is.flatMap SInst.result ++ (match cl.dst with | some d => [(d, cl.rw)] | none => [])) ++
    B.last.flatMap SInst.result
where
  SInst.result : SInst → List (String × Nat)
    | .i x => [x.result]
    | .freeze d w _ => [(d, w)]
    | .alloca d _ _ => [(d, 64)]
    | .load d w _ _ => [(d, w)]
    | .store .. => []
    | .gep d _ _ _ => [(d, 64)]

def xPhisOf (G : XFunc) : List PhiI := G.blocks.flatMap (·.phis)

def SInst.uninitDst? : SInst → Option String
  | .i (.uninit d _) => some d
  | _ => none

def xUninitDsts (G : XFunc) : List String :=
  G.blocks.flatMap fun B =>
    (B.segs.flatMap fun (is, _) => is.filterMap SInst.uninitDst?) ++ B.last.filterMap SInst.uninitDst?

/-- Phis that may carry an uninitialised value (the fixpoint of
`Tr::enter_frame`), in textual order. -/
def xShadowPhis (G : XFunc) : List String :=
  let mu := muFix (xPhisOf G) ((xPhisOf G).length + 1) (xUninitDsts G)
  ((xPhisOf G).filter (fun p => mu.contains p.dst)).map (·.dst)

/-- What the certificate records of one instance. -/
structure IInfo where
  fn : XFunc
  env : List (String × Arg)
  sh : List (String × Arg)
  lo : Nat
  hi : Nat
  /-- the continuation block its `ret`s jump to (`none`: the analysed function) -/
  retTo : Option Nat
  /-- per LLVM block, the PIR block of each segment -/
  blks : List (List Nat)
  /-- per LLVM block, the first temporary of each segment -/
  ks : List (List Nat)
  /-- per LLVM block, the child instance of each call -/
  kids : List (List Nat)
  deriving Repr, Inhabited

def IInfo.ctx (I : IInfo) (vars : List Nat) : Ctx :=
  { env := I.env, sh := I.sh, lo := I.lo, hi := I.hi, vars := vars, inlined := I.retTo.isSome }

def IInfo.heads (I : IInfo) : List Nat := I.blks.map (·.headD 0)
def IInfo.tails (I : IInfo) : List Nat := I.blks.map (·.getLastD 0)

/-! ## The mirror -/

structure TS where
  vars : Array Nat
  blocks : Array PBlock
  insts : Array IInfo
  deriving Inhabited

abbrev TM := StateT TS (Except String)

def newVar (w : Nat) : TM Nat := modifyGet fun s => (s.vars.size, { s with vars := s.vars.push w })

def newBlock : TM Nat :=
  modifyGet fun s => (s.blocks.size, { s with blocks := s.blocks.push { phis := [], stmts := [], term := .stop } })

def setBlock (b : Nat) (f : PBlock → PBlock) : TM Unit :=
  modify fun s => { s with blocks := s.blocks.modify b f }

def pushTemps (ts : List Nat) : TM Unit :=
  modify fun s => { s with vars := ts.foldl (fun a w => a.push w) s.vars }

def curVars : TM (List Nat) := do return (← get).vars.toList

def liftE {α : Type} (e : Except String α) : TM α :=
  match e with
  | .ok a => pure a
  | .error m => throw m

/-- Blocks reachable from the entry (`Tr::normal_blocks`). -/
def xLive (G : XFunc) : List Nat :=
  let succ (B : XBlock) : List Nat :=
    match B.term with
    | .br t => (lookupBlock G.shape t).toList
    | .cbr _ t f => (lookupBlock G.shape t).toList ++ (lookupBlock G.shape f).toList
    | _ => []
  let step (seen : List Nat) : List Nat :=
    seen ++ ((seen.flatMap fun j => (G.blocks[j]?.map succ).getD []).filter (fun j => !seen.contains j)).eraseDups
  (List.range G.blocks.length).foldl (fun s _ => step s) [0]

/-- `Tr::enter_frame`: blocks, result variables, shadow variables of a new
instance whose parameters are bound to `penv`.  Returns the instance index. -/
def enterFrame (G : XFunc) (penv : List (String × Arg)) (lo : Nat) (retTo : Option Nat) : TM Nat := do
  if G.blocks.isEmpty then throw "UNENCODED: empty function body"
  let heads ← G.blocks.mapM fun _ => newBlock
  let mut env := penv
  for (d, w) in xResultNames G do
    if !okW w then throw s!"UNENCODED: result type i{w}"
    let v ← newVar w
    env := env ++ [(d, .v v w)]
  let mut sh : List (String × Arg) := (xUninitDsts G).map fun d => (d, .c 1 1)
  for d in xShadowPhis G do
    let v ← newVar 1
    sh := sh ++ [(d, .v v 1)]
  if !nodupB (env.map Prod.fst) then throw "outside fragment: duplicate SSA name"
  if !nodupB (G.blocks.map XBlock.name) then throw "outside fragment: duplicate block name"
  let hi := (← get).vars.size
  let I : IInfo := { fn := G, env := env, sh := sh, lo := lo, hi := hi, retTo := retTo,
                     blks := heads.map fun h => [h], ks := G.blocks.map fun _ => [],
                     kids := G.blocks.map fun _ => [] }
  modifyGet fun s => (s.insts.size, { s with insts := s.insts.push I })

def getInst (ι : Nat) : TM IInfo := do
  match (← get).insts[ι]? with
  | some I => pure I
  | none => throw "internal: instance"

def setInst (ι : Nat) (f : IInfo → IInfo) : TM Unit :=
  modify fun s => { s with insts := s.insts.modify ι f }

/-- Append to the per-block lists of block `b` of instance `ι`. -/
def recordSeg (ι b : Nat) (pb k : Nat) (kid : Option Nat) : TM Unit :=
  setInst ι fun I =>
    { I with blks := I.blks.modify b (· ++ [pb]),
             ks := I.ks.modify b (· ++ [k]),
             kids := match kid with
               | some x => I.kids.modify b (· ++ [x])
               | none => I.kids }

/-- `Tr::run_frame` of instance `ι` (every block must be reachable); the
return sites `(PIR block, returned Arg)` of the instance are returned. -/
def runFrame (M : XMod) (depth : Nat) : Nat → List String → Nat → TM (List (Nat × Arg))
  | 0, _, _ => throw "UNENCODED: call depth"
  | fuel + 1, stack, ι => do
    let I ← getInst ι
    let G := I.fn
    let live := xLive G
    if (List.range G.blocks.length).any (fun j => !live.contains j) then
      throw "outside fragment: unreachable block"
    let mut rets : List (Nat × Arg) := []
    let mut tails : List Nat := []
    for (B, b) in G.blocks.zipIdx do
      let mut cur := (I.heads.getD b 0)
      -- segments ending in a call
      for (is, cl) in B.segs do
        let I ← getInst ι
        let c := I.ctx (← curVars)
        let k := (← get).vars.size
        let (s1, t1) ← liftE (trSInstsX c k is)
        pushTemps t1
        let G' ← match M.find cl.f with
          | some G' => pure G'
          | none => throw s!"UNENCODED: call @{cl.f}"
        if stack.contains cl.f then throw s!"UNENCODED: recursive call @{cl.f}"
        if stack.length > depth then throw s!"UNENCODED: call depth > {depth} (@{cl.f})"
        if !(G'.retw == 0 || okW G'.retw) then throw s!"UNENCODED: call @{cl.f} returning i{G'.retw}"
        if cl.args.length != G'.params.length then throw s!"UNENCODED: call @{cl.f} arity"
        if !(G'.params.all fun p => okW p.2) then throw s!"UNENCODED: call @{cl.f} parameter"
        let (s2, t2, As) ← liftE (trArgsX (I.ctx (← curVars)) (k + t1.length)
          ((cl.args.zip G'.params).map fun ((o, w), (_, wp)) => (o, w, wp)))
        pushTemps t2
        let cb ← newBlock
        let rv? ← if G'.retw == 0 then pure none else do
          if cl.rw != G'.retw then throw s!"outside fragment: call @{cl.f} result width"
          let rv ← match cl.dst with
            | some d => liftE (dstX (I.ctx (← curVars)) d cl.rw)
            | none => newVar G'.retw
          let uv ← newVar ((G'.retw + 7) / 8)
          pure (some (rv, uv))
        if G'.retw == 0 && cl.dst.isSome then throw s!"outside fragment: void call @{cl.f} with a result"
        let lo' := (← get).vars.size
        let κ ← enterFrame G' (G'.params.map Prod.fst |>.zip As) lo' (some cb)
        let Iκ ← getInst κ
        setBlock cur fun pb => { pb with stmts := s1 ++ s2, term := .jmp (Iκ.heads.getD 0 0) }
        recordSeg ι b cb k (some κ)
        let sites ← runFrame M depth fuel (stack ++ [cl.f]) κ
        match rv? with
        | some (rv, uv) =>
          if !sites.isEmpty then
            setBlock cb fun pb => { pb with phis :=
              [{ dst := uv, inc := sites.map fun (q, _) => (q, .c ((G'.retw + 7) / 8) 0) },
               { dst := rv, inc := sites }] }
        | none => pure ()
        cur := cb
      -- the last segment
      let I ← getInst ι
      let c := I.ctx (← curVars)
      let k := (← get).vars.size
      let (s1, t1) ← liftE (trSInstsX c k B.last)
      pushTemps t1
      let (s2, t2, T, A?) ← liftE (trTermX G (I.ctx (← curVars)) I.heads I.retTo (k + t1.length) B.term)
      pushTemps t2
      setBlock cur fun pb => { pb with stmts := s1 ++ s2, term := T }
      -- the first temporary of the last segment
      setInst ι fun I => { I with ks := I.ks.modify b (· ++ [k]) }
      match A? with
      | some A => if I.retTo.isSome then rets := rets ++ [(cur, A)]
      | none => pure ()
      tails := tails ++ [cur]
    -- phis (`resolve_phis`): predecessors are the blocks that end each LLVM block
    let I ← getInst ι
    for (B, b) in G.blocks.zipIdx do
      let qs ← liftE (trPhisX G (I.ctx (← curVars)) tails B.phis)
      setBlock (I.heads.getD b 0) fun pb => { pb with phis := qs }
    pure rets

/-- The translator (`prism::pir::translate` on the extended fragment):
the PIR function and the certificate. -/
def translateX (M : XMod) (depth : Nat) (F : XFunc) : Except String (PFunc × List IInfo) := do
  need (F.params.all (fun p => okW p.2)) "UNENCODED: parameter type"
  need (F.retw == 0 || okW F.retw) "UNENCODED: return type"
  need (!F.blocks.isEmpty) "UNENCODED: empty function body"
  let np := F.params.length
  let go : TM Unit := do
    let mut penv : List (String × Arg) := []
    for (p, w) in F.params do
      let v ← newVar w
      penv := penv ++ [(p, .v v w)]
    let ι ← enterFrame F penv 0 none
    let _ ← runFrame M depth (depth + 2) [F.name] ι
  let ((), s) ← go.run { vars := #[], blocks := #[], insts := #[] }
  pure ({ vars := s.vars.toList, params := List.range' 0 np, retw := F.retw, blocks := s.blocks.toList },
        s.insts.toList)

end PrismRefine
