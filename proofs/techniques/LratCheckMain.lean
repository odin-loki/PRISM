/-
`prism-lrat-check`: core Lean's verified LRAT checker
(`Std.Tactic.BVDecide.LRAT.check`, soundness `LRAT.check_sound`, the checker
`certified_unsat` relies on), as a second certificate checker next to
cake_lpr (roadmap 8.2, "Certificate checking").

    prism-lrat-check CNF LRAT
        parse the DIMACS file (DIMACS variable v is CNF variable v-1, the
        convention of `Std.Sat.CNF.dimacs`) and check the proof against it.

    prism-lrat-check --dag FORMULA CNF LRAT
        rebuild the CNF from the formula with the proved bit-blaster, insist
        that the CNF file is byte for byte `CNF.dimacs (dagCNF g)`, and run
        `checkDag g cert` -- the exact premise of `checkDag_sound`, so an
        accepted proof means the DAG formula is unsatisfiable, with no DIMACS
        parser in between.

Prints `s VERIFIED UNSAT` and exits 0 when the checker accepts; prints
`s NOT VERIFIED` and exits 1 when it rejects; exits 2 on malformed input.
-/
import PrismTechniques.BitblastSexp
import Std.Tactic.BVDecide.LRAT.Parser

open PrismTechniques.Bitblast PrismTechniques.Bitblast.Sexp
open Std.Tactic.BVDecide

/-- A strict DIMACS reader: one `p cnf V C` header, `C` zero-terminated
clauses, literals within `±V`. -/
def parseDimacs (b : ByteArray) : Except String (Std.Sat.CNF Nat) := Id.run do
  let n := b.size
  let mut i := 0
  let mut header : Option (Nat × Nat) := none
  let mut clauses : Array (Std.Sat.CNF.Clause Nat) := #[]
  let mut cur : Array (Nat × Bool) := #[]
  while i < n do
    let c := b.get! i
    if c == 99 || c == 112 then -- 'c' comment, 'p' header: whole line
      let start := i
      while i < n && b.get! i != 10 do i := i + 1
      if c == 112 then
        if header.isSome then return .error "two DIMACS headers"
        let line := String.fromUTF8! (b.extract start i)
        match (line.splitOn " ").filter (· ≠ "") with
        | ["p", "cnf", v, k] =>
          match v.toNat?, k.toNat? with
          | some v, some k => header := some (v, k)
          | _, _ => return .error "bad DIMACS header"
        | _ => return .error "bad DIMACS header"
    else if c == 32 || c == 10 || c == 13 || c == 9 then
      i := i + 1
    else if c == 45 || (48 ≤ c && c ≤ 57) then
      let some (nv, _) := header | return .error "clause before the DIMACS header"
      let neg := c == 45
      if neg then i := i + 1
      let start := i
      let mut v := 0
      while i < n && 48 ≤ b.get! i && b.get! i ≤ 57 do
        v := v * 10 + (b.get! i - 48).toNat
        i := i + 1
      if i == start then return .error "bad literal"
      if v == 0 then
        if neg then return .error "bad literal -0"
        clauses := clauses.push cur.toList
        cur := #[]
      else
        if v > nv then return .error s!"literal {v} out of range"
        cur := cur.push (v - 1, !neg)
    else
      return .error s!"unexpected byte {c} in DIMACS"
  let some (_, k) := header | return .error "no DIMACS header"
  if !cur.isEmpty then return .error "unterminated clause"
  if clauses.size != k then return .error "clause count differs from the header"
  return .ok ⟨clauses⟩

def verdict (ok : Bool) : IO UInt32 := do
  if ok then
    IO.println "s VERIFIED UNSAT"
    return 0
  IO.println "s NOT VERIFIED"
  return 1

def loadProof (path : String) : IO (Option (Array LRAT.IntAction)) := do
  try
    return some (← LRAT.loadLRATProof path)
  catch e =>
    IO.eprintln s!"prism-lrat-check: cannot read the LRAT proof: {e}"
    return none

def main (args : List String) : IO UInt32 := do
  match args with
  | [cnfPath, lratPath] =>
    let cnf ← match parseDimacs (← IO.FS.readBinFile cnfPath) with
      | .ok c => pure c
      | .error e => IO.eprintln s!"prism-lrat-check: {e}"; return 2
    let some cert ← loadProof lratPath | return 2
    verdict (LRAT.check cert cnf)
  | ["--dag", dagPath, cnfPath, lratPath] =>
    let forms ← match parseAll (← IO.FS.readBinFile dagPath) with
      | .ok f => pure f
      | .error e => IO.eprintln s!"prism-lrat-check: {e}"; return 2
    if forms.isEmpty then
      IO.eprintln "prism-lrat-check: no formula"
      return 2
    let g ← match toDag forms[0]! with
      | .ok g => pure g
      | .error e => IO.eprintln s!"prism-lrat-check: {e}"; return 2
    -- The CNF the other checker and the SAT solver read must be this CNF.
    let fileBytes ← IO.FS.readBinFile cnfPath
    if fileBytes != (Std.Sat.CNF.dimacs (dagCNF g)).toUTF8 then
      IO.eprintln "prism-lrat-check: the CNF file is not the proved bit-blaster's CNF of the formula"
      return ← verdict false
    let some cert ← loadProof lratPath | return 2
    verdict (checkDag g cert)
  | _ =>
    IO.eprintln "usage: prism-lrat-check CNF LRAT | prism-lrat-check --dag FORMULA CNF LRAT"
    return 2
