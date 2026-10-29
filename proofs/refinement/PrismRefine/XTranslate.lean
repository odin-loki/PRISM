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

/-- `MemTr::store` (also `llvm.lifetime.start`, `Tr::call`). -/
def trStore (c : Ctx) (k w : Nat) (v : FOpnd) (p : Opnd) (al : Nat) : Except String (List PStmt × List Nat) := do
    need (okW w) "UNENCODED: width"
    need (alignOK al) "outside fragment: alignment"
    let (sv, tv, V, I) ← trStoreVal c w k v
    let (sp, tp, P) ← trOpndX c 64 true (k + tv.length) p
    let (sa, ta) := accessChecks (k + tv.length + tp.length) P ((w + 7) / 8) true al
    pure (sv ++ sp ++ sa ++ [.store P (V.setW w) I], tv ++ tp ++ ta)

/-! ### `llvm.memcpy` / `memmove` / `memset` (`MemTr::memcpy_`, `memset_`) -/

/-- A length widened to 64 bits (`Op::ZExt` when narrower). -/
def zext64 (k : Nat) (L : Arg) : List PStmt × List Nat × Arg :=
  if L.width < 64 then ([.assign k (.cast .zext) [L]], [64], .v k 64) else ([], [], L)

/-- `MemTr::access_checks` of `n` bytes at `P` with byte alignment under the
guard `g` (`n ≠ 0`): every check is `and g c`. -/
def accChkG (k g : Nat) (P N : Arg) (write : Bool) : List PStmt :=
  let G := Arg.v g 1
  [.assign k (.bin .lshr) [P, c64 48], .assign (k + 1) (.bin .and) [P, c64 (2 ^ 48 - 1)],
   .assign (k + 2) .objKind [P], .assign (k + 3) .objLive [P], .assign (k + 4) .objSize [P],
   .assign (k + 5) (.cmp .eq) [.v k 64, c64 0], .assign (k + 6) (.bin .and) [G, .v (k + 5) 1],
   .check (.v (k + 6) 1) "null" "PTR-NULL-DEREF",
   .assign (k + 7) (.cmp .ne) [.v k 64, c64 0], .assign (k + 8) (.cmp .eq) [.v (k + 2) 8, .c 8 0],
   .assign (k + 9) (.bin .and) [.v (k + 7) 1, .v (k + 8) 1], .assign (k + 10) (.bin .and) [G, .v (k + 9) 1],
   .check (.v (k + 10) 1) "wild" "PTR-INVALID-DEREF",
   .assign (k + 11) (.cmp .ne) [.v (k + 2) 8, .c 8 0], .assign (k + 12) (.cmp .eq) [.v (k + 3) 1, .c 1 0],
   .assign (k + 13) (.bin .and) [.v (k + 11) 1, .v (k + 12) 1], .assign (k + 14) (.bin .and) [G, .v (k + 13) 1],
   .check (.v (k + 14) 1) "uaf" "MEM-UAF",
   .assign (k + 15) (.bin .add) [.v (k + 1) 64, N], .assign (k + 16) (.cmp .ugt) [.v (k + 15) 64, .v (k + 4) 64],
   .assign (k + 17) (.ovf .uadd) [.v (k + 1) 64, N], .assign (k + 18) (.bin .or) [.v (k + 16) 1, .v (k + 17) 1],
   .assign (k + 19) (.bin .and) [.v (k + 3) 1, .v (k + 18) 1], .assign (k + 20) (.bin .and) [G, .v (k + 19) 1],
   .check (.v (k + 20) 1) (if write then "oob-write" else "oob-read")
     (if write then "MEM-OOB-WRITE" else "MEM-OOB-READ")] ++
  (if write then
    [.assign (k + 21) (.cmp .eq) [.v (k + 2) 8, .c 8 4], .assign (k + 22) (.bin .and) [.v (k + 3) 1, .v (k + 21) 1],
     .assign (k + 23) (.bin .and) [G, .v (k + 22) 1], .check (.v (k + 23) 1) "write-const" "MEM-WRITE-CONST"]
   else [])

def accTG (write : Bool) : List Nat :=
  [64, 64, 8, 1, 64, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 64, 1, 1, 1, 1, 1] ++ (if write then [1, 1, 1] else [])

/-- Overlap check for `@llvm.memcpy` (`MemTr::memcpy_` `self_ok`): ranges overlap
and the offsets differ (an exact self-copy, `d = s`, is defined; refinement
finding 6). -/
def overlapChkSelfOk (j g : Nat) (D S N : Arg) : List PStmt :=
  [.assign j (.bin .and) [D, c64 (2 ^ 48 - 1)], .assign (j + 1) (.bin .and) [S, c64 (2 ^ 48 - 1)],
   .assign (j + 2) (.bin .add) [.v (j + 1) 64, N], .assign (j + 3) (.cmp .ult) [.v j 64, .v (j + 2) 64],
   .assign (j + 4) (.bin .add) [.v j 64, N], .assign (j + 5) (.cmp .ult) [.v (j + 1) 64, .v (j + 4) 64],
   .assign (j + 6) (.bin .lshr) [D, c64 48], .assign (j + 7) (.bin .lshr) [S, c64 48],
   .assign (j + 8) (.cmp .eq) [.v (j + 6) 64, .v (j + 7) 64],
   .assign (j + 9) (.bin .and) [.v (j + 3) 1, .v (j + 5) 1], .assign (j + 10) (.bin .and) [.v (j + 8) 1, .v (j + 9) 1],
   .assign (j + 11) (.cmp .ne) [.v j 64, .v (j + 1) 64],
   .assign (j + 12) (.bin .and) [.v (j + 11) 1, .v (j + 10) 1],
   .assign (j + 13) (.bin .and) [.v g 1, .v (j + 12) 1], .check (.v (j + 13) 1) "overlap" "MEM-OVERLAP"]

/-- Overlap check for `__prism_memcpy` (no `self_ok`). -/
def overlapChkPrism (j g : Nat) (D S N : Arg) : List PStmt :=
  [.assign j (.bin .and) [D, c64 (2 ^ 48 - 1)], .assign (j + 1) (.bin .and) [S, c64 (2 ^ 48 - 1)],
   .assign (j + 2) (.bin .add) [.v (j + 1) 64, N], .assign (j + 3) (.cmp .ult) [.v j 64, .v (j + 2) 64],
   .assign (j + 4) (.bin .add) [.v j 64, N], .assign (j + 5) (.cmp .ult) [.v (j + 1) 64, .v (j + 4) 64],
   .assign (j + 6) (.bin .lshr) [D, c64 48], .assign (j + 7) (.bin .lshr) [S, c64 48],
   .assign (j + 8) (.cmp .eq) [.v (j + 6) 64, .v (j + 7) 64],
   .assign (j + 9) (.bin .and) [.v (j + 3) 1, .v (j + 5) 1], .assign (j + 10) (.bin .and) [.v (j + 8) 1, .v (j + 9) 1],
   .assign (j + 11) (.bin .and) [.v g 1, .v (j + 10) 1], .check (.v (j + 11) 1) "overlap" "MEM-OVERLAP"]

def overlapChk (selfOk : Bool) (j g : Nat) (D S N : Arg) : List PStmt :=
  if selfOk then overlapChkSelfOk j g D S N else overlapChkPrism j g D S N

def overlapT (selfOk : Bool) : List Nat :=
  if selfOk then [64, 64, 64, 1, 64, 1, 64, 64, 1, 1, 1, 1, 1, 1]
  else [64, 64, 64, 1, 64, 1, 64, 64, 1, 1, 1, 1]

/-- `MemTr::emit_entry_globals`: the initialiser's stores into the global
(variable `i`), a pointer addition first for a non-zero offset. -/
def globStores (i : Nat) : Nat → List (Nat × Nat × Nat) → List PStmt × List Nat
  | _, [] => ([], [])
  | k, (o, w, v) :: t =>
    if o = 0 then
      let r := globStores i k t
      (.store (.v i 64) (.c w (v % 2 ^ w)) (.c 1 1) :: r.1, r.2)
    else
      let r := globStores i (k + 1) t
      (.assign k (.bin .add) [.v i 64, c64 o] :: .store (.v k 64) (.c w (v % 2 ^ w)) (.c 1 1) :: r.1, 64 :: r.2)

/-- The checks `Tr::call` inserts for `llvm.abs` / `ctlz` / `cttz` with the
poison flag set. -/
def unChecks (uk : UnK) (flag : Bool) (w : Nat) (A : Arg) : List Chk :=
  match uk with
  | .abs => opt flag (.p (.cmp .eq) A (.c w (2 ^ (w - 1))) "abs" "INT-SIGNED-OVF")
  | .ctlz | .cttz => opt flag (.p (.cmp .eq) A (.c w 0) "clz0" "INT-CLZ-ZERO")
  | _ => []

/-- `MemTr::icmp`: a signed predicate on pointers is its unsigned one. -/
def uPred : Pred → Pred
  | .sgt => .ugt | .sge => .uge | .slt => .ult | .sle => .ule
  | p => p

/-- `MemTr::dealloc` of `P` as an object of kind `kd` (temporaries from `j`):
null / not allocated, not the start, another allocator, not dynamically
allocated, released already. -/
def freeChk (j : Nat) (P : Arg) (kd : Nat) : List PStmt :=
  [.assign j (.bin .lshr) [P, c64 48], .assign (j + 1) (.cmp .ne) [.v j 64, c64 0],
   .assign (j + 2) .objKind [P], .assign (j + 3) .objLive [P],
   .assign (j + 4) (.cmp .eq) [.v (j + 2) 8, .c 8 0], .assign (j + 5) (.bin .and) [.v (j + 1) 1, .v (j + 4) 1],
   .check (.v (j + 5) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 6) (.cmp .ne) [.v (j + 2) 8, .c 8 0], .assign (j + 7) (.bin .and) [.v (j + 1) 1, .v (j + 6) 1],
   .assign (j + 8) (.bin .and) [P, c64 (2 ^ 48 - 1)], .assign (j + 9) (.cmp .ne) [.v (j + 8) 64, c64 0],
   .assign (j + 10) (.bin .and) [.v (j + 7) 1, .v (j + 9) 1],
   .check (.v (j + 10) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 11) (.cmp .eq) [.v (j + 2) 8, .c 8 2], .assign (j + 12) (.cmp .eq) [.v (j + 2) 8, .c 8 5],
   .assign (j + 13) (.cmp .eq) [.v (j + 2) 8, .c 8 6], .assign (j + 14) (.bin .or) [.v (j + 12) 1, .v (j + 13) 1],
   .assign (j + 15) (.bin .or) [.v (j + 11) 1, .v (j + 14) 1],
   .assign (j + 16) (.cmp .ne) [.v (j + 2) 8, .c 8 kd], .assign (j + 17) (.bin .and) [.v (j + 7) 1, .v (j + 16) 1],
   .assign (j + 18) (.bin .and) [.v (j + 17) 1, .v (j + 15) 1],
   .check (.v (j + 18) 1) "free-mismatch" "MEM-MISMATCHED-FREE",
   .assign (j + 19) (.cmp .eq) [.v (j + 15) 1, .c 1 0], .assign (j + 20) (.bin .and) [.v (j + 17) 1, .v (j + 19) 1],
   .check (.v (j + 20) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 21) (.cmp .eq) [.v (j + 2) 8, .c 8 kd], .assign (j + 22) (.cmp .eq) [.v (j + 3) 1, .c 1 0],
   .assign (j + 23) (.bin .and) [.v (j + 21) 1, .v (j + 22) 1],
   .assign (j + 24) (.bin .and) [.v (j + 7) 1, .v (j + 23) 1],
   .check (.v (j + 24) 1) "double-free" "MEM-DOUBLE-FREE"]

def freeT : List Nat := [64, 1, 8, 1, 1, 1, 1, 1, 64, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]

/-- `MemTr::dealloc` of a `FILE` (kind 8, `fclose`): one MEM-INVALID-FREE check
for another kind instead of the two allocator checks. -/
def freeChkF (j : Nat) (P : Arg) : List PStmt :=
  [.assign j (.bin .lshr) [P, c64 48], .assign (j + 1) (.cmp .ne) [.v j 64, c64 0],
   .assign (j + 2) .objKind [P], .assign (j + 3) .objLive [P],
   .assign (j + 4) (.cmp .eq) [.v (j + 2) 8, .c 8 0], .assign (j + 5) (.bin .and) [.v (j + 1) 1, .v (j + 4) 1],
   .check (.v (j + 5) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 6) (.cmp .ne) [.v (j + 2) 8, .c 8 0], .assign (j + 7) (.bin .and) [.v (j + 1) 1, .v (j + 6) 1],
   .assign (j + 8) (.bin .and) [P, c64 (2 ^ 48 - 1)], .assign (j + 9) (.cmp .ne) [.v (j + 8) 64, c64 0],
   .assign (j + 10) (.bin .and) [.v (j + 7) 1, .v (j + 9) 1],
   .check (.v (j + 10) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 11) (.cmp .eq) [.v (j + 2) 8, .c 8 2], .assign (j + 12) (.cmp .eq) [.v (j + 2) 8, .c 8 5],
   .assign (j + 13) (.cmp .eq) [.v (j + 2) 8, .c 8 6], .assign (j + 14) (.bin .or) [.v (j + 12) 1, .v (j + 13) 1],
   .assign (j + 15) (.bin .or) [.v (j + 11) 1, .v (j + 14) 1],
   .assign (j + 16) (.cmp .ne) [.v (j + 2) 8, .c 8 8], .assign (j + 17) (.bin .and) [.v (j + 7) 1, .v (j + 16) 1],
   .check (.v (j + 17) 1) "free-invalid" "MEM-INVALID-FREE",
   .assign (j + 18) (.cmp .eq) [.v (j + 2) 8, .c 8 8], .assign (j + 19) (.cmp .eq) [.v (j + 3) 1, .c 1 0],
   .assign (j + 20) (.bin .and) [.v (j + 18) 1, .v (j + 19) 1],
   .assign (j + 21) (.bin .and) [.v (j + 7) 1, .v (j + 20) 1],
   .check (.v (j + 21) 1) "double-free" "MEM-DOUBLE-FREE"]

def freeTF : List Nat := [64, 1, 8, 1, 1, 1, 1, 1, 64, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]

def freeChkK (j : Nat) (P : Arg) (kd : Nat) : List PStmt := if kd = 8 then freeChkF j P else freeChk j P kd

def freeTK (kd : Nat) : List Nat := if kd = 8 then freeTF else freeT

/-- `MemTr::fresh_cstr` (`__prism_fresh_cstr`, D136), temporaries from `k`:
the drawn length, its bound, `n + 1`, the size check of `MemTr::alloc`, the
object, `p + n` and the NUL store. -/
def freshCore (k kd : Nat) : List PStmt :=
  [.havoc k, .assign (k + 1) (.cmp .ult) [.v k 64, c64 4096], .assume (.v (k + 1) 1),
   .assign (k + 2) (.bin .add) [.v k 64, c64 1],
   .assign (k + 3) (.cmp .ult) [.v (k + 2) 64, c64 (2 ^ 47)], .assume (.v (k + 3) 1),
   .alloc (k + 4) (.v (k + 2) 64) kd 2 16,
   .assign (k + 5) (.bin .add) [.v (k + 4) 64, .v k 64],
   .store (.v (k + 5) 64) (.c 8 0) (.c 1 1)]

def freshT : List Nat := [64, 1, 64, 1, 64, 64]

/-- The size check of `MemTr::alloc`: a variable size below `2^47` is
assumed (a constant one is checked by the translator). -/
def hallocSz (k1 : Nat) : Arg → List PStmt × List Nat
  | .c _ _ => ([], [])
  | N => ([.assign k1 (.cmp .ult) [N, c64 (2 ^ 47)], .assume (.v k1 1)], [1])

/-- `MemTr::write_havoc` (`__prism_havoc_bytes`, D136) of `N` bytes at `D`,
temporaries from `g`: the guard `N ≠ 0`, the write checks under it, the size
check of `MemTr::alloc`, a fresh `extern` object of arbitrary bytes, the copy
and the end of the object's lifetime. -/
def havocCore (g : Nat) (D N : Arg) : List PStmt :=
  let a := g + 1 + (accTG true).length
  let o := a + (hallocSz a N).2.length
  [.assign g (.cmp .ne) [N, c64 0]] ++ (accChkG (g + 1) g D N true ++ ((hallocSz a N).1 ++
    [.alloc o N 7 2 16, .memcpy D (.v o 64) N, .free (.v o 64)]))

def havocT (g : Nat) (N : Arg) : List Nat :=
  [1] ++ accTG true ++ (hallocSz (g + 1 + (accTG true).length) N).2 ++ [64]

/-- `MemTr::obj_size_remaining`: bytes from `p` to the end of its object. -/
def hobjszCore (k : Nat) (P : Arg) : List PStmt :=
  [.assign k .objSize [P],
   .assign (k + 1) (.bin .and) [P, c64 (2 ^ 48 - 1)],
   .assign (k + 2) (.bin .sub) [.v k 64, .v (k + 1) 64],
   .assign (k + 3) (.cmp .ugt) [.v (k + 1) 64, .v k 64],
   .assign (k + 4) (.select) [.v (k + 3) 1, .c 64 0, .v (k + 2) 64]]

def hobjszT : List Nat := [64, 64, 64, 1, 64]

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
  | .store w v p al => trStore c k w v p al
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
  | .mm d mk w a b => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w false k a
    let (sb, tb, B) ← trOpndX c w false (k + ta.length) b
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ [.assign i (.mm mk) [A, B]], ta ++ tb)
  | .un d uk w a flag => do
    need (okW w) "UNENCODED: width"
    need (uk != .bswap || w % 16 == 0) "UNENCODED: call @llvm.bswap"
    need (!flag || uk == .abs || uk == .ctlz || uk == .cttz) "outside fragment: flag"
    let (sa, ta, A) ← trOpndX c w false k a
    let (sc, tc) := emitAll (k + ta.length) (unChecks uk flag w A)
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sc ++ [.assign i (.un uk) [A]], ta ++ tc)
  | .expect d w a => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w false k a
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ [.assign i .copy [A]], ta)
  | .ovf d ok w a b => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w false k a
    let (sb, tb, B) ← trOpndX c w false (k + ta.length) b
    let i0 ← dstX c (pairReg d 0) w
    let i1 ← dstX c (pairReg d 1) 1
    need ((look c.sh (pairReg d 0)).isNone && (look c.sh (pairReg d 1)).isNone)
      "outside fragment: result with a shadow"
    need (A.var? != some i0 && B.var? != some i0) "outside fragment: overflow operand"
    pure (sa ++ sb ++ [.assign i0 (.bin (ovfBin ok)) [A, B], .assign i1 (.ovf ok) [A, B]], ta ++ tb)
  | .xv d w src idx => do
    need (okW w) "UNENCODED: width"
    need (decide (idx ≤ 1)) "UNENCODED: extractvalue"
    match look c.env (pairReg src idx) with
    | some a =>
      need (a.lt c.hi) "outside fragment: extractvalue reads above its instance"
      need (look c.sh (pairReg src idx)).isNone "outside fragment: pair with a shadow"
      let i ← dstX c d w
      need (look c.sh d).isNone "outside fragment: result with a shadow"
      pure ([.assign i .copy [a]], [])
    | none => throw "UNENCODED: extractvalue"
  | .lstart _ _ =>
    -- `translate.cpp` starts a new lifetime (`Stmt::Revive`, refinement
    -- finding 7), which the Lean PIR has no statement for
    throw "outside fragment: llvm.lifetime.start (a new lifetime; not in the Lean PIR)"
  | .lend p => do
    let (sp, tp, P) ← trOpndX c 64 true k p
    pure (sp ++ [.free P], tp)
  | .glob d size al kd ini st => do
    need (!c.inlined) "outside fragment: global in an inlined function"
    need (st.all fun (o, _, _) => decide (o < 2 ^ 64)) "outside fragment: initialiser offset"
    let i ← dstX c d 64
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    let (s1, t1) := globStores i k st
    pure (.alloc i (.c 64 size) kd ini al :: s1, t1)
  | .memcpy d s len lw mv selfOk => do
    need (okW lw) "UNENCODED: width"
    let (sd, td, D) ← trOpndX c 64 true k d
    let (ss, ts, S) ← trOpndX c 64 true (k + td.length) s
    let (sn, tn, L) ← trOpndX c lw true (k + td.length + ts.length) len
    need (L.width == lw) "outside fragment: length width"
    let k1 := k + td.length + ts.length + tn.length
    let (sz, tz, N) := zext64 k1 L
    let g := k1 + tz.length
    let a1 := g + 1
    let a2 := a1 + (accTG true).length
    let a3 := a2 + (accTG false).length
    pure (sd ++ ss ++ sn ++ sz ++ [.assign g (.cmp .ne) [N, c64 0]] ++ accChkG a1 g D N true ++
            accChkG a2 g S N false ++ (if mv then [] else overlapChk selfOk a3 g D S N) ++ [.memcpy D S N],
          td ++ ts ++ tn ++ tz ++ [1] ++ accTG true ++ accTG false ++ (if mv then [] else overlapT selfOk))
  | .pcmp d p a b => do
    need (p != .eq && p != .ne) "outside fragment: pointer equality is an icmp"
    let (sa, ta, A) ← trOpndX c 64 false k a
    let (sb, tb, B) ← trOpndX c 64 false (k + ta.length) b
    let i ← dstX c d 1
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    let j := k + ta.length + tb.length
    -- the object check (`obj(x) != obj(y)`, PTR-COMPARE), then the unsigned comparison
    pure (sa ++ sb ++ [.assign j (.bin .lshr) [A, c64 48], .assign (j + 1) (.bin .lshr) [B, c64 48],
            .assign (j + 2) (.cmp .ne) [.v j 64, .v (j + 1) 64], .check (.v (j + 2) 1) "ptr-cmp" "PTR-COMPARE",
            .assign i (.cmp (uPred p)) [A, B]],
          ta ++ tb ++ [64, 64, 1])
  | .psub d fl a b => do
    need (flagsOk .sub fl) "outside fragment: flags"
    let (sa, ta, A) ← trOpndX c 64 false k a
    let (sb, tb, B) ← trOpndX c 64 false (k + ta.length) b
    let j := k + ta.length + tb.length
    -- `Tr::binop`: the operands, `MemTr::ptr_sub_check` (`obj(x) != obj(y)`,
    -- PTR-COMPARE), the flag checks, then the subtraction
    let (sc, tc) := emitAll (j + 3) (checks .sub fl 64 A B)
    let i ← dstX c d 64
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure (sa ++ sb ++ [.assign j (.bin .lshr) [A, c64 48], .assign (j + 1) (.bin .lshr) [B, c64 48],
            .assign (j + 2) (.cmp .ne) [.v j 64, .v (j + 1) 64], .check (.v (j + 2) 1) "ptr-sub" "PTR-COMPARE"] ++
            sc ++ [.assign i (.bin .sub) [A, B]],
          ta ++ tb ++ [64, 64, 1] ++ tc)
  | .nondet d w => do
    need (okW w) "UNENCODED: width"
    let i ← dstX c d w
    need (look c.sh d).isNone "outside fragment: result with a shadow"
    pure ([.havoc i], [])
  | .vassume a w => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w true k a
    need (A.width == w) "outside fragment: assume operand width"
    let j := k + ta.length
    pure (sa ++ [.assign j (.cmp .ne) [A, .c w 0], .assume (.v j 1)], ta ++ [1])
  | .halloc d n kd ini => do
    need (decide (kd < 256) && decide (ini ≤ 2)) "outside fragment: allocation kind"
    let (sn, tn, N) ← trOpndX c 64 false k n
    need (N.width == 64 && (match N with | .c _ b => decide (b < 2 ^ 47) | .v _ _ => true))
      "UNENCODED: allocation larger than 2^47 bytes"
    let r := hallocSz (k + tn.length) N
    match d with
    | some d => do
      let i ← dstX c d 64
      need (look c.sh d).isNone "outside fragment: result with a shadow"
      pure (sn ++ r.1 ++ [.alloc i N kd ini 16], tn ++ r.2)
    | none => pure (sn ++ r.1 ++ [.alloc (k + tn.length + r.2.length) N kd ini 16], tn ++ r.2 ++ [64])
  | .hfree p kd kill => do
    need (decide (kd < 256)) "outside fragment: release kind"
    let (sp, tp, P) ← trOpndX c 64 true k p
    pure (sp ++ freeChkK (k + tp.length) P kd ++ (if kill then [.free P] else []), tp ++ freeTK kd)
  | .hobjsz d p => do
    let (sp, tp, P) ← trOpndX c 64 false k p
    let j := k + tp.length
    match d with
    | some d => do
      let i ← dstX c d 64
      need (look c.sh d).isNone "outside fragment: result with a shadow"
      pure (sp ++ hobjszCore j P ++ [.assign i .copy [.v (j + 4) 64]], tp ++ hobjszT)
    | none => pure (sp ++ hobjszCore j P, tp ++ hobjszT)
  | .hread p n => do
    let (sp, tp, D) ← trOpndX c 64 true k p
    let (sn, tn, N) ← trOpndX c 64 true (k + tp.length) n
    need (N.width == 64) "outside fragment: length width"
    let g := k + tp.length + tn.length
    pure (sp ++ sn ++ ([.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N false),
      tp ++ tn ++ ([1] ++ accTG false))
  | .hfresh d kd => do
    need (decide (kd < 256)) "outside fragment: allocation kind"
    match d with
    | some d => do
      let i ← dstX c d 64
      need (look c.sh d).isNone "outside fragment: result with a shadow"
      pure (freshCore k kd ++ [.assign i .copy [.v (k + 4) 64]], freshT)
    | none => pure (freshCore k kd, freshT)
  | .hmemset d b len => do
    let (sd, td, D) ← trOpndX c 64 true k d
    let (sb, tb, B) ← trOpndX c 32 true (k + td.length) b
    need (B.width == 32) "outside fragment: byte width"
    let (sn, tn, N) ← trOpndX c 64 true (k + td.length + tb.length) len
    need (N.width == 64) "outside fragment: length width"
    let g := k + td.length + tb.length + tn.length
    let o := g + 1 + (accTG true).length
    pure (sd ++ sb ++ sn ++ (([.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N true) ++
        [.assign o (.cast .trunc) [B], .memset D (.v o 8) N]),
      td ++ tb ++ tn ++ (([1] ++ accTG true) ++ [8]))
  | .vcheck a w cls => do
    need (okW w) "UNENCODED: width"
    let (sa, ta, A) ← trOpndX c w true k a
    need (A.width == w) "outside fragment: check operand width"
    let j := k + ta.length
    pure (sa ++ [.assign j (.cmp .eq) [A, .c w 0], .check (.v j 1) "model" cls], ta ++ [1])
  | .lpad .. => throw "outside fragment: landingpad (only in an exception region)"
  | .insv .. => throw "outside fragment: insertvalue (only in an exception region)"
  | .hhavoc p n => do
    let (sp, tp, D) ← trOpndX c 64 true k p
    let (sn, tn, N) ← trOpndX c 64 true (k + tp.length) n
    need (N.width == 64 && (match N with | .c _ b => decide (b < 2 ^ 47) | .v _ _ => true))
      "UNENCODED: allocation larger than 2^47 bytes"
    let g := k + tp.length + tn.length
    pure (sp ++ sn ++ havocCore g D N, tp ++ tn ++ havocT g N)
  | .memset d b len lw => do
    need (okW lw) "UNENCODED: width"
    let (sd, td, D) ← trOpndX c 64 true k d
    let (sb, tb, B) ← trOpndX c 8 true (k + td.length) b
    need (B.width == 8) "outside fragment: byte width"
    let (sn, tn, L) ← trOpndX c lw true (k + td.length + tb.length) len
    need (L.width == lw) "outside fragment: length width"
    let k1 := k + td.length + tb.length + tn.length
    let (sz, tz, N) := zext64 k1 L
    let g := k1 + tz.length
    pure (sd ++ sb ++ sn ++ sz ++ [.assign g (.cmp .ne) [N, c64 0]] ++ accChkG (g + 1) g D N true ++
            [.memset D B N],
          td ++ tb ++ tn ++ tz ++ [1] ++ accTG true)

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

/-- The variables of the function's own stack objects (`Frame::allocas`). -/
def escArgsX (c : Ctx) : List String → Except String (List Arg)
  | [] => .ok []
  | a :: t => do
    match look c.env a with
    | some x =>
      need (x.lt c.hi) "outside fragment: stack object above its instance"
      need (look c.sh a).isNone "outside fragment: stack object with a shadow"
      let r ← escArgsX c t
      pure (x :: r)
    | none => throw s!"UNENCODED: value %{a}"

/-- `MemTr::stack_escape_check`, the loop over the objects: `obj(a)`, `ro ==
obj(a)`, and the running `or` (temporaries from `j`). -/
def escLoop (ro : Arg) : Nat → Option Arg → List Arg → List PStmt × List Nat × Option Arg
  | _, hit, [] => ([], [], hit)
  | j, none, a :: t =>
    let r := escLoop ro (j + 2) (some (.v (j + 1) 1)) t
    ([.assign j (.bin .lshr) [a, c64 48], .assign (j + 1) (.cmp .eq) [ro, .v j 64]] ++ r.1, [64, 1] ++ r.2.1,
     r.2.2)
  | j, some h, a :: t =>
    let r := escLoop ro (j + 3) (some (.v (j + 2) 1)) t
    ([.assign j (.bin .lshr) [a, c64 48], .assign (j + 1) (.cmp .eq) [ro, .v j 64],
      .assign (j + 2) (.bin .or) [h, .v (j + 1) 1]] ++ r.1, [64, 1, 1] ++ r.2.1, r.2.2)

/-- `MemTr::stack_escape_check` of the returned pointer `RET` against the
objects `As` (nothing when there are none; temporaries from `j`). -/
def escChk (j : Nat) (RET : Arg) : List Arg → List PStmt × List Nat
  | [] => ([], [])
  | a :: t =>
    let r := escLoop (.v j 64) (j + 1) none (a :: t)
    let m := j + 1 + r.2.1.length
    ([.assign j (.bin .lshr) [RET, c64 48]] ++ r.1 ++
      [.assign m (.cmp .ne) [.v j 64, c64 0], .assign (m + 1) (.bin .and) [r.2.2.getD (.c 1 0), .v m 1],
       .check (.v (m + 1) 1) "stack-escape" "MEM-STACK-ESCAPE"],
     [64] ++ r.2.1 ++ [1, 1])

/-- The stack-escape check of a returned pointer: only the analysed
function's (`retTo = none`) own objects. -/
def escStmts (G : XFunc) (c : Ctx) (retTo : Option Nat) (j : Nat) (A : Arg) :
    Except String (List PStmt × List Nat) :=
  if retTo.isNone && G.retPtr then (escArgsX c G.escNames).map (fun As => escChk j A As) else .ok ([], [])

/-- `Tr::call`'s `end_lifetime` of an inlined callee's own stack objects at
its `ret` (`Frame::allocas`); a callee returning a pointer next to them would
need `stack_escape_check` (outside the fragment). -/
def freeStmts (G : XFunc) (c : Ctx) (retTo : Option Nat) : Except String (List PStmt) :=
  if retTo.isSome then
    if G.retPtr && !G.escNames.isEmpty then .error "outside fragment: inlined function returning a pointer with stack objects"
    else (escArgsX c G.escNames).map (·.map .free)
  else .ok []

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
    let sf ← freeStmts G c retTo
    pure (sf, [], (match retTo with | some cb => .jmp cb | none => .ret none), none)
  | .ret (some o) => do
    need (G.retw != 0) "outside fragment: ret value in a void function"
    let (s, tw, A) ← trOpndX c G.retw true k o
    let (se, te) ← escStmts G c retTo (k + tw.length) A
    let sf ← freeStmts G c retTo
    pure (s ++ se ++ sf, tw ++ te, (match retTo with | some cb => .jmp cb | none => .ret (some A)), some A)
  | .unreachable => .ok ([.check (.c 1 1) "unreachable" "CXX-UNREACHABLE"], [], .stop, none)

/-- The terminator of a block, or the failing check of a noreturn failure
call (`Tr::call`: `check(1, prop, cls)`, then the block ends). -/
def trTermN (G : XFunc) (c : Ctx) (heads : List Nat) (retTo : Option Nat) (k : Nat)
    (nr : Option (String × String)) (t : LTerm) : Except String (List PStmt × List Nat × PTerm × Option Arg) :=
  match nr with
  | some (p, cl) => .ok ([.check (.c 1 1) p cl], [], .stop, none)
  | none => trTermX G c heads retTo k t

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
    | .mm d _ w _ _ => [(d, w)]
    | .un d _ w _ _ => [(d, w)]
    | .expect d w _ => [(d, w)]
    | .ovf d _ w _ _ => [(pairReg d 0, w), (pairReg d 1, 1)]
    | .xv d w _ _ => [(d, w)]
    | .glob d _ _ _ _ _ => [(d, 64)]
    | .pcmp d _ _ _ => [(d, 1)]
    | .psub d _ _ _ => [(d, 64)]
    | .nondet d w => [(d, w)]
    | .halloc (some d) _ _ _ => [(d, 64)]
    | .hobjsz (some d) _ => [(d, 64)]
    | .hfresh (some d) _ => [(d, 64)]
    | .hfresh none _ => []
    | .halloc none _ _ _ | .hobjsz none _ | .vassume .. | .hfree .. | .hhavoc .. | .hread .. | .hmemset .. | .vcheck .. => []
    | .lstart .. | .lend .. | .memcpy .. | .memset .. => []
    | .lpad d _ _ => [(pairReg d 0, 64), (pairReg d 1, 32)]
    | .insv d w0 w1 _ _ _ => [(pairReg d 0, w0), (pairReg d 1, w1)]

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
  /-- `Tr::type_ids`: typeinfo symbols in the order first numbered -/
  tys : Array String := #[]
  /-- `Tr::handlers`, innermost first: (instance, landing pad, `frames` depth) -/
  hs : List (Nat × String × Nat) := []
  /-- `Tr::frames`: the instances on the inlined call path -/
  fr : List Nat := []
  /-- `Tr::eh_edges`: (instance, landing-pad block) -/
  edges : List (Nat × Nat) := []
  /-- `Tr::pphis` of the landing-pad heads: (PIR block, variable, incomings) -/
  lps : List (Nat × Nat × List (Nat × Arg)) := []
  /-- `thrown_types()` (module-wide, sorted) -/
  thrown : Option (List String) := none
  /-- `Tr::type_dtor` has a destructor (`L thrdtor`) -/
  thrDtor : Bool := false
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

/-- `Tr::type_id`. -/
def typeId (t : String) : TM Nat := do
  let s ← get
  match s.tys.toList.idxOf? t with
  | some i => pure (i + 1)
  | none => do set { s with tys := s.tys.push t }; pure (s.tys.size + 1)

/-- `Tr::at`: a new pointer variable `p + off` (`MemTr::gep` over `i8`,
inbounds), its statements. -/
def atX (B : Arg) (off : Nat) : TM (List PStmt × Arg) := do
  let i ← newVar 64
  let j := (← get).vars.size
  pushTemps (gFinT true)
  pure (gFinS i j B (c64 off) true, .v i 64)

/-- `Tr::st`: an 8-aligned store with its access checks. -/
def stX (P V : Arg) : TM (List PStmt) := do
  let k := (← get).vars.size
  let (sa, ta) := accessChecks k P ((V.width + 7) / 8) true 8
  pushTemps ta
  pure (sa ++ [.store P V (.c 1 1)])

/-- `Tr::eh_call` on `std::__throw_*` with no handler (`Tr::raise`,
`dispatch_type`): the exception object, its header stores, the `main`
uncaught check, the stop, and the fresh unreachable block. -/
def throwX (stack : List String) (t : String) : TM (List PStmt) := do
  let b ← newVar 64
  let B := Arg.v b 64
  let tid ← typeId t
  let s0 ← stX B (c64 tid)
  let (g1, P1) ← atX B 8
  let s1 ← stX P1 (c64 0)
  let (g2, P2) ← atX B 16
  let (g3, P3) ← atX B 32
  let s2 ← stX P2 P3
  let top := stack.headD ""
  let entry := top == "main" || top.startsWith "_GLOBAL__sub_I" || top.startsWith "__cxx_global_var_init"
  let _ ← newBlock
  pure ([.alloc b (c64 96) 7 2 16] ++ s0 ++ g1 ++ s1 ++ g2 ++ g3 ++ s2 ++
    (if entry then [.check (.c 1 1) "uncaught" "CXX-UNCAUGHT"] else []))

def liftE {α : Type} (e : Except String α) : TM α :=
  match e with
  | .ok a => pure a
  | .error m => throw m

/-- The successors of an LLVM block along its terminator. -/
def xSucc (G : XFunc) (B : XBlock) : List Nat :=
  match B.term with
  | .br t => (lookupBlock G.shape t).toList
  | .cbr _ t f => (lookupBlock G.shape t).toList ++ (lookupBlock G.shape f).toList
  | _ => []

/-- `normal_blocks`: the blocks reachable from the entry. -/
def xLive (G : XFunc) : List Nat :=
  let step (seen : List Nat) : List Nat :=
    seen ++ ((seen.flatMap fun j => (G.blocks[j]?.map (xSucc G)).getD []).filter (fun j => !seen.contains j)).eraseDups
  (List.range G.blocks.length).foldl (fun s _ => step s) [0]

/-- `xLive` contains the entry and is closed under `xSucc` (checked, not
proved: the proof only needs the check). -/
def xLiveClosed (G : XFunc) : Bool :=
  (xLive G).contains 0 && (List.range G.blocks.length).all fun j =>
    !(xLive G).contains j || ((G.blocks[j]?.map (xSucc G)).getD []).all (xLive G).contains

/-- A reachable block's phi names an unreachable predecessor (`Tr` drops
such incomings; not mirrored). -/
def xDeadInc (G : XFunc) (live : List Nat) : Bool :=
  G.blocks.zipIdx.any fun (B, b) => live.contains b && B.phis.any fun p => p.inc.any fun (_, pr) =>
    match lookupBlock G.shape pr with
    | some j => !live.contains j
    | none => false

/-- The number of `alloca`s of `G`. -/
def xAllocas (G : XFunc) : Nat :=
  (G.blocks.flatMap fun B => (B.segs.flatMap (·.1)) ++ B.last).countP (·.allocaDst?.isSome)

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
  -- `Frame::allocas`: an inlined callee's stack objects end at its return,
  -- those of the entry block before its first call (the others would stay)
  if retTo.isSome && xAllocas G != G.escNames.length then
    throw "outside fragment: alloca in an inlined function outside its entry block"
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


/-- `Tr::ld`: an 8-aligned load with its access checks. -/
def ldX (P : Arg) (w : Nat) : TM (List PStmt × Arg) := do
  let v ← newVar w
  let k := (← get).vars.size
  let (sa, ta) := accessChecks k P ((w + 7) / 8) false 8
  let u := k + ta.length
  pushTemps (ta ++ [1])
  pure (sa ++ [.load v u P, .check (.v u 1) "uninit" "UNINIT-READ"], .v v w)

/-- `Tr::soft_stop`. -/
def softStop (b : Nat) : TM Unit :=
  setBlock b fun pb => { pb with stmts := pb.stmts ++ [.check (.c 1 1) "unmodelled" "UNENCODED"], term := .stop }

/-- `Tr::add_phi`. -/
def addPhi (b d : Nat) (pr : Nat) (a : Arg) : TM Unit :=
  modify fun s =>
    if s.lps.any (fun (b', d', _) => b' == b && d' == d) then
      { s with lps := s.lps.map fun (b', d', inc) => if b' == b && d' == d then (b', d', inc ++ [(pr, a)]) else (b', d', inc) }
    else { s with lps := s.lps ++ [(b, d, [(pr, a)])] }

/-- The `landingpad` of a block: result, cleanup, clauses. -/
def lpadOf (B : XBlock) : Option (String × Bool × List String) :=
  ((B.segs.flatMap (·.1)) ++ B.last).findSome? fun i =>
    match i with
    | .lpad d cu cls => some (d, cu, cls)
    | _ => none

/-- `pointer_ti` / `fundamental_ti`. -/
def ptrTI (t : String) : Bool := t.startsWith "_ZTIP"
def fundTI (t : String) : Bool :=
  t.startsWith "_ZTI" &&
    let r := t.drop 4
    let r := if r.startsWith "K" then r.drop 1 else r
    ["v", "b", "c", "a", "h", "s", "t", "i", "j", "l", "m", "x", "y", "n", "o", "f", "d", "e", "g", "w",
     "Ds", "Di", "Du", "Dn", "DF16_"].contains r.toString

/-- `Tr::dispatch_type` (catch clauses are refused). -/
def dispatchX (stack : List String) (cur : Nat) (E : Arg) (_t : String) : TM Unit := do
  let s ← get
  for (ι, pad, depth) in s.hs do
    let I ← getInst ι
    let some pb := lookupBlock I.fn.shape pad | throw "outside fragment: landing pad not found"
    let some B := I.fn.blocks[pb]? | throw "internal: landing pad"
    if B.stub then throw "outside fragment: landing pad the exporter stubbed"
    let some (d, cu, cls) := lpadOf B | throw "outside fragment: landing pad not recognised"
    -- `Tr::enters` / `Tr::catches`: (enters, selector, bound pointer is the thrown pointer's value)
    let mut res : Option (Nat × Bool) := none
    let mut unknown := false
    for c in cls do
      if c == "f" then unknown := true; break
      if c == "null" then res := some (0x7FFFFFF0, false); break
      if c == _t then res := some (← typeId c, ptrTI _t); break
      if ptrTI c || ptrTI _t then
        if (ptrTI c && ptrTI _t) || (ptrTI c && _t == "_ZTIDn") then unknown := true; break
        continue
      if fundTI c || fundTI _t then continue
      if !_t.startsWith "_ZTI" then unknown := true; break
      throw "outside fragment: catch through a base class (derives)"
    if unknown then softStop cur; return
    let (sel, bind) ← match res with
      | some (sel, byPtr) => do
        -- the pointer the handler binds, stored at exn+16
        let (g1, Pl) ← atX E 32
        let (g2, Adj) ← if byPtr then ldX Pl 64 else pure ([], Pl)
        let (g3, P16) ← atX E 16
        let s4 ← stX P16 Adj
        pure (sel, g1 ++ g2 ++ g3 ++ s4)
      | none => if cu then pure (0, []) else pure (1, [])
    if res.isNone && !cu then continue
    setBlock cur fun q => { q with stmts := q.stmts ++ bind }
    -- frames between the throw and the handler: their locals end
    let mut fs : List PStmt := []
    for κ in s.fr.drop depth do
      let Iκ ← getInst κ
      let As ← liftE (escArgsX (Iκ.ctx (← curVars)) Iκ.fn.escNames)
      fs := fs ++ As.map .free
    let lb := I.heads.getD pb 0
    setBlock cur fun q => { q with stmts := q.stmts ++ fs, term := .jmp lb }
    modify fun s => { s with edges := s.edges ++ [(ι, pb)] }
    match look I.env (pairReg d 0), look I.env (pairReg d 1) with
    | some (.v e _), some (.v sl _) =>
      addPhi lb e cur E
      addPhi lb sl cur (.c 32 sel)
    | _, _ => throw "internal: landing pad pair"
    return
  let top := stack.headD ""
  let entry := top == "main" || top.startsWith "_GLOBAL__sub_I" || top.startsWith "__cxx_global_var_init"
  setBlock cur fun q => { q with stmts := q.stmts ++
    (if entry then [.check (.c 1 1) "uncaught" "CXX-UNCAUGHT"] else []), term := .stop }

/-- `Tr::eh_call` on `std::__throw_*`: the exception object and its header. -/
def throwObjX (t : String) : TM (List PStmt × Arg) := do
  let b ← newVar 64
  let B := Arg.v b 64
  let tid ← typeId t
  let s0 ← stX B (c64 tid)
  let (g1, P1) ← atX B 8
  let s1 ← stX P1 (c64 0)
  let (g2, P2) ← atX B 16
  let (g3, P3) ← atX B 32
  let s2 ← stX P2 P3
  pure ([.alloc b (c64 96) 7 2 16] ++ s0 ++ g1 ++ s1 ++ g2 ++ g3 ++ s2, B)

/-- `Tr::raise` of a type read from the header (`resume`). -/
def raiseUnknownX (stack : List String) (cur0 : Nat) (E : Arg) : TM Unit := do
  let some ts := (← get).thrown | throw "outside fragment: thrown types not exported"
  let (sl, T) ← ldX E 64
  setBlock cur0 fun q => { q with stmts := q.stmts ++ sl }
  let mut cur := cur0
  for t in ts do
    let yes ← newBlock
    let no ← newBlock
    let tid ← typeId t
    let c ← newVar 1
    setBlock cur fun q => { q with stmts := q.stmts ++ [.assign c (.cmp .eq) [T, c64 tid]], term := .br (.v c 1) yes no }
    dispatchX stack yes E t
    cur := no
  softStop cur
  let _ ← newBlock

/-- `Frame::upair` of a pair register (the fields that may be undef). -/
def upairX (G : XFunc) : Nat → String → Except String (Bool × Bool)
  | 0, _ => .error "outside fragment: insertvalue chain"
  | n + 1, r => do
    let all := G.blocks.flatMap fun B => (B.segs.flatMap (·.1)) ++ B.last
    match all.findSome? (fun i => match i with
        | .insv d _ _ ag v idx => if d == r then some (ag, v, idx) else none
        | _ => none) with
    | some (ag, _, idx) =>
      let base ← match ag with
        | none => pure (true, true)
        | some a => upairX G n a
      pure (if idx == 0 then (false, base.2) else (base.1, false))
    | none =>
      if all.any (fun i => match i with | .lpad d _ _ => d == r | _ => false) then pure (false, false)
      else .error "outside fragment: resume of a pair not from a landing pad"

/-- An `insertvalue` operand: a constant, or a field of a landing-pad pair. -/
def insvOpndOK (G : XFunc) : Opnd → Bool
  | .const _ => true
  | .reg n =>
    let all := G.blocks.flatMap fun B => (B.segs.flatMap (·.1)) ++ B.last
    all.any fun i => match i with
      | .xv d _ src _ => d == n && all.any (fun j => match j with | .lpad e _ _ => e == src | _ => false)
      | _ => false
  | _ => false

/-- `insertvalue` into a pair (`Tr::inst`). -/
def trInsvX (ι : Nat) (d : String) (w0 w1 : Nat) (ag : Option String) (v : Opnd) (idx : Nat) :
    TM (List PStmt) := do
  let I ← getInst ι
  if !insvOpndOK I.fn v then throw "outside fragment: insertvalue operand"
  let (some (.v a0 _), some (.v a1 _)) := (look I.env (pairReg d 0), look I.env (pairReg d 1))
    | throw "internal: insertvalue pair"
  let (sb, B0, B1) ← match ag with
    | some a => match look I.env (pairReg a 0), look I.env (pairReg a 1) with
      | some x, some y => pure ([], x, y)
      | _, _ => throw "outside fragment: insertvalue into a non-pair"
    | none => do
      let h0 ← newVar w0
      let h1 ← newVar w1
      pure ([PStmt.havoc h0, .havoc h1], Arg.v h0 w0, Arg.v h1 w1)
  let wsel := if idx == 0 then w0 else w1
  let V ← match v with
    | .const b => pure (Arg.c wsel b)
    | .reg n => match look I.env n with
      | some (.v i _) => pure (Arg.v i wsel)
      | _ => throw "outside fragment: insertvalue operand"
    | _ => throw "outside fragment: insertvalue operand"
  pure (sb ++ [.assign a0 .copy [if idx == 0 then V else B0], .assign a1 .copy [if idx == 1 then V else B1]])

/-! ### The caught-exception stack (`Tr::eh_call`: `__cxa_begin_catch`,
`__cxa_end_catch`, `__cxa_rethrow`, `llvm.eh.typeid.for`), region code only -/

/-- `Tr::assign`: a fresh variable of width `w`. -/
def asgX (w : Nat) (op : POp) (as : List Arg) : TM (List PStmt × Arg) := do
  let d ← newVar w
  pure ([.assign d op as], .v d w)

/-- `MemTr::gep(slot, i64, {D}, inbounds)`: `slot + 8 * D` with its checks. -/
def slotGepX (S D : Arg) : TM (List PStmt × Arg) := do
  let i ← newVar 64
  let k := (← get).vars.size
  let g ← liftE (gLoop k { s := [], ts := [], cst := 0, var := none } [(.first (.const 0) 64 8, D)])
  let (st, ts) := gEnd k i S true g
  pushTemps ts
  pure (st, .v i 64)

/-- `Tr::caught_slot()`: the object the entry block's `L glob %__prism.caught`
allocated (the eager prologue allocation of the analysed function). -/
def caughtSlotX : TM Arg := do
  let I0 ← getInst 0
  match look I0.env "__prism.caught" with
  | some (.v i 64) => pure (.v i 64)
  | _ => throw "outside fragment: caught-exception stack not exported"

/-- `soft_guard ok`: `br ok eh.ok eh.unmodelled`, a soft stop in the second;
the statements `ss` and the branch end `cur`; returns `eh.ok`. -/
def softGuardX (cur : Nat) (ss : List PStmt) (ok : Arg) : TM Nat := do
  let yes ← newBlock
  let no ← newBlock
  setBlock cur fun q => { q with stmts := q.stmts ++ ss, term := .br ok yes no }
  softStop no
  pure yes

/-- `Tr::eh_call` on a caught-exception-stack call `cl` in region code at PIR
block `cur` (its earlier statements already there); `some b`: the block the
rest of the LLVM block continues in, `none`: the call does not return. -/
def ehCallX (stack : List String) (c : Ctx) (cur : Nat) (cl : CallI) : TM (Option Nat) := do
  if cl.inv then throw s!"outside fragment: invoke of @{cl.f}"
  if cl.f.startsWith "llvm.eh.typeid.for:" then
    let t := (cl.f.drop "llvm.eh.typeid.for:".length).toString
    let some d := cl.dst | throw "outside fragment: llvm.eh.typeid.for without a result"
    let i ← liftE (dstX c d 32)
    let tid ← typeId t
    setBlock cur fun q => { q with stmts := q.stmts ++ [.assign i .copy [.c 32 (tid % 2 ^ 32)]] }
    return some cur
  let S ← caughtSlotX
  if cl.f == "__cxa_begin_catch" then
    let [(o, 64)] := cl.args | throw "outside fragment: __cxa_begin_catch arity"
    let k := (← get).vars.size
    let (s0, t0, As) ← liftE (trArgsX c k [(o, 64, 64)])
    pushTemps t0
    let [E] := As | throw "internal: begin_catch argument"
    let (s1, D) ← ldX S 64
    let (s2, ok) ← asgX 1 (.cmp .ult) [D, c64 8]
    let cur ← softGuardX cur (s0 ++ s1 ++ s2) ok
    let (g1, Cp) ← atX E 8
    let (l1, C) ← ldX Cp 64
    let (a1, Ng) ← asgX 1 (.cmp .slt) [C, c64 0]
    let (a2, Sb) ← asgX 64 (.bin .sub) [c64 0, C]
    let (a3, Ab) ← asgX 64 .select [Ng, Sb, C]
    let (a4, Ad) ← asgX 64 (.bin .add) [Ab, c64 1]
    let w1 ← stX Cp Ad
    let (g2, Te) ← slotGepX S D
    let (l2, Top) ← ldX Te 64
    let (a5, Pu) ← asgX 1 (.cmp .ne) [Top, E]
    let (a6, Z) ← asgX 64 (.cast .zext) [Pu]
    let (a7, Nd) ← asgX 64 (.bin .add) [D, Z]
    let (g3, Ep) ← slotGepX S Nd
    let w2 ← stX Ep E
    let w3 ← stX S Nd
    let (g4, P16) ← atX E 16
    let (l3, R) ← ldX P16 64
    let r ← match cl.dst with
      | some d => do
        if cl.rw != 64 then throw "outside fragment: __cxa_begin_catch result width"
        let i ← liftE (dstX c d 64)
        pure [PStmt.assign i .copy [R]]
      | none => pure []
    setBlock cur fun q => { q with stmts := q.stmts ++ g1 ++ l1 ++ a1 ++ a2 ++ a3 ++ a4 ++ w1 ++ g2 ++ l2 ++
      a5 ++ a6 ++ a7 ++ g3 ++ w2 ++ w3 ++ g4 ++ l3 ++ r }
    return some cur
  if cl.f == "__cxa_end_catch" then
    if !cl.args.isEmpty || cl.dst.isSome then throw "outside fragment: __cxa_end_catch arity"
    if (← get).thrDtor then throw "outside fragment: __cxa_end_catch with a thrown class that has a destructor"
    let (s1, D) ← ldX S 64
    let (s2, ok) ← asgX 1 (.cmp .ne) [D, c64 0]
    let cur ← softGuardX cur (s1 ++ s2) ok
    let (g1, Ep) ← slotGepX S D
    let (l1, E) ← ldX Ep 64
    let (g2, Cp) ← atX E 8
    let (l2, C) ← ldX Cp 64
    let (a1, Ng) ← asgX 1 (.cmp .slt) [C, c64 0]
    let (a2, Ad) ← asgX 64 (.bin .add) [C, c64 1]
    let (a3, Sb) ← asgX 64 (.bin .sub) [C, c64 1]
    let (a4, Nc) ← asgX 64 .select [Ng, Ad, Sb]
    let w1 ← stX Cp Nc
    let (a5, Gn) ← asgX 1 (.cmp .eq) [Nc, c64 0]
    let (a6, Z) ← asgX 64 (.cast .zext) [Gn]
    let (a7, Nd) ← asgX 64 (.bin .sub) [D, Z]
    let w2 ← stX S Nd
    let destroy ← newBlock
    let dn ← newBlock
    let (a8, Is1) ← asgX 1 (.cmp .eq) [C, c64 1]
    let ss := g1 ++ l1 ++ g2 ++ l2 ++ a1 ++ a2 ++ a3 ++ a4 ++ w1 ++ a5 ++ a6 ++ a7 ++ w2 ++ a8
    setBlock cur fun q => { q with stmts := q.stmts ++ ss, term := .br Is1 destroy dn }
    -- `thrown_types()`, the type id, the payload (no destructor: none is called)
    let (l3, _) ← ldX E 64
    let (g3, _) ← atX E 32
    setBlock destroy fun q => { q with stmts := q.stmts ++ l3 ++ g3 ++ [.free E], term := .jmp dn }
    return some dn
  if cl.f == "__cxa_rethrow" then
    if !cl.args.isEmpty || cl.dst.isSome then throw "outside fragment: __cxa_rethrow arity"
    let (s1, D) ← ldX S 64
    let (s2, Z0) ← asgX 1 (.cmp .eq) [D, c64 0]
    let (g1, Ep) ← slotGepX S D
    let (l1, E) ← ldX Ep 64
    let (g2, Cp) ← atX E 8
    let (l2, C) ← ldX Cp 64
    let (a1, Ng) ← asgX 64 (.bin .sub) [c64 0, C]
    let w1 ← stX Cp Ng
    setBlock cur fun q => { q with stmts := q.stmts ++ s1 ++ s2 ++ [.check Z0 "terminate" "CXX-TERMINATE"] ++
      g1 ++ l1 ++ g2 ++ l2 ++ a1 ++ w1 }
    raiseUnknownX stack cur E
    return none
  throw s!"UNENCODED: call @{cl.f}"

/-- The calls `ehCallX` translates. -/
def ehCallee (f : String) : Bool :=
  f == "__cxa_begin_catch" || f == "__cxa_end_catch" || f == "__cxa_rethrow" || f.startsWith "llvm.eh.typeid.for:"

/-- Instructions of a landing-pad region block (`Tr::inst`, with
`landingpad` and `insertvalue`). -/
def trRegionInsts (ι : Nat) (is : List SInst) : TM (List PStmt) := do
  let mut out : List PStmt := []
  let mut pend : List SInst := []
  for i in is ++ [.lpad "" false []] do
    match i with
    | .lpad .. | .insv .. =>
      if !pend.isEmpty then
        let I ← getInst ι
        let k := (← get).vars.size
        let (s1, t1) ← liftE (trSInstsX (I.ctx (← curVars)) k pend)
        pushTemps t1
        out := out ++ s1
        pend := []
      match i with
      | .insv d w0 w1 ag v idx => out := out ++ (← trInsvX ι d w0 w1 ag v idx)
      | _ => pure ()
    | _ => pend := pend ++ [i]
  pure out

/-- `Tr::run_frame` of instance `ι` (the unreachable blocks are skipped); the
return sites `(PIR block, returned Arg)` of the instance are returned. -/
def runFrame (M : XMod) (depth : Nat) : Nat → List String → Nat → TM (List (Nat × Arg))
  | 0, _, _ => throw "UNENCODED: call depth"
  | fuel + 1, stack, ι => do
    let I ← getInst ι
    let G := I.fn
    let live := xLive G
    if !xLiveClosed G then throw "internal: reachable blocks not closed"
    if xDeadInc G live then throw "outside fragment: phi incoming from an unreachable block"
    if G.blocks.zipIdx.any (fun (B, b) => B.stub && live.contains b) then
      throw "outside fragment: a block the exporter stubbed is reachable"
    -- `translate_block`: block `b` (`region` marks a landing-pad region
    -- block); its last PIR block and returned value
    let trB : Nat → XBlock → Bool → TM (Nat × Option Arg) := fun b B region => do
      let I ← getInst ι
      let mut cur := (I.heads.getD b 0)
      if region && B.stub then throw "outside fragment: landing-pad code the exporter stubbed"
      -- segments ending in a call
      for (is, cl) in B.segs do
        let I ← getInst ι
        let k := (← get).vars.size
        let s1 ← if region then trRegionInsts ι is else do
          let (s1, t1) ← liftE (trSInstsX (I.ctx (← curVars)) k is)
          pushTemps t1
          pure s1
        if region && ehCallee cl.f && (M.find cl.f).isNone then
          setBlock cur fun pb => { pb with stmts := pb.stmts ++ s1 }
          match ← ehCallX stack (I.ctx (← curVars)) cur cl with
          | some c' => cur := c'; continue
          | none =>
            setInst ι fun I => { I with ks := I.ks.modify b (· ++ [k]) }
            return (cur, none)
        let k1 := (← get).vars.size
        let G' ← match M.find cl.f with
          | some G' => pure G'
          | none => throw s!"UNENCODED: call @{cl.f}"
        if stack.contains cl.f then throw s!"UNENCODED: recursive call @{cl.f}"
        if stack.length > depth then throw s!"UNENCODED: call depth > {depth} (@{cl.f})"
        if !(G'.retw == 0 || okW G'.retw) then throw s!"UNENCODED: call @{cl.f} returning i{G'.retw}"
        if cl.args.length != G'.params.length then throw s!"UNENCODED: call @{cl.f} arity"
        if !(G'.params.all fun p => okW p.2) then throw s!"UNENCODED: call @{cl.f} parameter"
        let (s2, t2, As) ← liftE (trArgsX (I.ctx (← curVars)) k1
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
        setBlock cur fun pb => { pb with stmts := (if region then pb.stmts else []) ++ s1 ++ s2, term := .jmp (Iκ.heads.getD 0 0) }
        if !region then recordSeg ι b cb k (some κ)
        if cl.inv && cl.pad == "" then throw "outside fragment: invoke without its landing pad"
        modify fun s => { s with hs := if cl.inv then (ι, cl.pad, s.fr.length) :: s.hs else s.hs,
                                 fr := s.fr ++ [κ] }
        let sites ← runFrame M depth fuel (stack ++ [cl.f]) κ
        modify fun s => { s with hs := if cl.inv then s.hs.tail else s.hs, fr := s.fr.dropLast }
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
      let k := (← get).vars.size
      let s1 ← if region then trRegionInsts ι B.last else do
        let (s1, t1) ← liftE (trSInstsX (I.ctx (← curVars)) k B.last)
        pushTemps t1
        pure s1
      let k1 := (← get).vars.size
      if let some r := B.res then
        -- `resume`: re-raise the pair's exception
        let (u0, _) ← liftE (upairX G (G.blocks.length * 8 + 8) r)
        let E ← match look I.env (pairReg r 0) with
          | some (.v e _) => pure (Arg.v e 64)
          | _ => throw "outside fragment: resume of a non-pair"
        setBlock cur fun pb => { pb with stmts := (if region then pb.stmts else []) ++ s1 ++
          (if u0 then [.check (.c 1 1) "undef" "UB-POISON"] else []), term := .stop }
        setInst ι fun I => { I with ks := I.ks.modify b (· ++ [k]) }
        raiseUnknownX stack cur E
        return (cur, none)
      if B.thr then
        -- `Tr::eh_call`: the arguments are evaluated (their checks), then the raise
        let (s0, t0, _) ← liftE (trArgsX (I.ctx (← curVars)) k1 (B.thrA.map fun (o, w) => (o, w, w)))
        pushTemps t0
        let (s2, E) ← throwObjX B.thrT
        setBlock cur fun pb => { pb with stmts := (if region then pb.stmts else []) ++ s1 ++ s0 ++ s2, term := .stop }
        dispatchX stack cur E B.thrT
        let _ ← newBlock
        setInst ι fun I => { I with ks := I.ks.modify b (· ++ [k]) }
        return (cur, none)
      if region then
        match B.term with
        | .ret _ => throw "outside fragment: return from landing-pad code"
        | _ => pure ()
      let (s2, t2, T, A?) ← liftE (trTermN G (I.ctx (← curVars)) I.heads I.retTo k1 B.nr B.term)
      pushTemps t2
      setBlock cur fun pb => { pb with stmts := (if region then pb.stmts else []) ++ s1 ++ s2, term := T }
      -- the first temporary of the last segment
      setInst ι fun I => { I with ks := I.ks.modify b (· ++ [k]) }
      pure (cur, A?)
    let mut rets : List (Nat × Arg) := []
    let mut tails : List Nat := I.heads
    for (B, b) in G.blocks.zipIdx do
      -- `run_frame` translates only the reachable blocks
      if !live.contains b then continue
      let (q, A?) ← trB b B false
      match A? with
      | some A => if I.retTo.isSome then rets := rets ++ [(q, A)]
      | none => pure ()
      tails := tails.set b q
    -- `eh_pass`: the landing pads a throw reached, with the code after them
    let mut done : List Nat := []
    let mut tried : List Nat := []
    for _ in List.range (G.blocks.length + 1) do
      let next := (← get).edges.findSome? fun (ι', p) =>
        if ι' == ι && !live.contains p && !done.contains p && !tried.contains p then some p else none
      let some p := next | break
      tried := p :: tried
      let mut work : List Nat := [p]
      for _ in List.range (G.blocks.length * 4 + 4) do
        match work with
        | [] => break
        | j :: rest =>
          work := rest
          if live.contains j || done.contains j then continue
          let some B := G.blocks[j]? | throw "internal: region block"
          let (q, _) ← trB j B true
          tails := tails.set j q
          done := done ++ [j]
          for t in xSucc G B do
            if live.contains t && !(G.blocks[t]?.map (·.phis.isEmpty)).getD true then
              throw "outside fragment: landing-pad code joins a block with phis"
          work := (xSucc G B).reverse ++ work
      if !work.isEmpty then throw "internal: region worklist"
    -- phis (`resolve_phis`): predecessors are the blocks that end each LLVM block
    let I ← getInst ι
    let lps := (← get).lps
    for (B, b) in G.blocks.zipIdx do
      if !live.contains b && !done.contains b then continue
      if done.contains b && B.phis.any (fun p => p.inc.any fun (_, pr) =>
          match lookupBlock G.shape pr with
          | some j => !(live.contains j || done.contains j)
          | none => true) then
        throw "outside fragment: landing-pad code phi from a block not translated"
      let qs ← liftE (trPhisX G (I.ctx (← curVars)) tails B.phis)
      let h := I.heads.getD b 0
      let lq := (lps.filter fun (b', _, _) => b' == h).map fun (_, d, inc) => ({ dst := d, inc := inc } : PPhi)
      setBlock h fun pb => { pb with phis := lq ++ qs }
    pure rets

/-- The translator (`prism::pir::translate` on the extended fragment):
the PIR function and the certificate. -/
def translateX (M : XMod) (depth : Nat) (F : XFunc) : Except String (PFunc × List IInfo) := do
  need (F.params.all (fun p => okW p.2)) "UNENCODED: parameter type"
  need (F.retw == 0 || okW F.retw) "UNENCODED: return type"
  need (!F.blocks.isEmpty) "UNENCODED: empty function body"
  need F.ptrParams.isEmpty
    "pointer parameter: unguarded model checking reports missing preconditions, not defects (Law 6)"
  let np := F.params.length
  let go : TM Unit := do
    let mut penv : List (String × Arg) := []
    for (p, w) in F.params do
      let v ← newVar w
      penv := penv ++ [(p, .v v w)]
    let ι ← enterFrame F penv 0 none
    modify fun s => { s with fr := [ι], thrown := F.thrown, thrDtor := F.thrDtor }
    let _ ← runFrame M depth (depth + 2) [F.name] ι
  let ((), s) ← go.run { vars := #[], blocks := #[], insts := #[] }
  pure ({ vars := s.vars.toList, params := List.range' 0 np, retw := F.retw, blocks := s.blocks.toList },
        s.insts.toList)

end PrismRefine
