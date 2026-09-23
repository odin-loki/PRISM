/-
`prism-bitblast`: run the proved bit-blaster on a formula.

    prism-bitblast < formula.sexp          DIMACS CNF + variable map on stdout
    prism-bitblast --eval < formula.sexp   evaluate the formula (Dag.eval) under
                                           each `(rho ...)` form after it; one
                                           0/1 line per assignment

The CNF is `Std.Sat.CNF.dimacs (dagCNF g)`: exactly the function
`PrismTechniques.Bitblast.toCNF` of the flattened DAG, whose correctness is
`toCNF_equisat` / `certified_dag_unsat` (so the executed code is the proved
code, modulo the Lean compiler; docs/TRUSTED_BASE.md).  `CNF.dimacs` numbers
CNF variable `v` as DIMACS variable `v+1`, so input bit `j` is DIMACS variable
`2*j+1`.

Output (before the DIMACS text):

    c prism-bitblast 1
    c var BASE WIDTH V0 V1 ... V(WIDTH-1)    one line per free input variable

Exit status: 0 ok, 2 malformed input (nothing is written to stdout).
-/
import PrismTechniques.BitblastSexp

open PrismTechniques.Bitblast PrismTechniques.Bitblast.Sexp

partial def readAll (s : IO.FS.Stream) (acc : ByteArray := .empty) : IO ByteArray := do
  let chunk ← s.read (USize.ofNat 1048576)
  if chunk.isEmpty then return acc else readAll s (acc ++ chunk)

def main (args : List String) : IO UInt32 := do
  let eval := args == ["--eval"]
  if !(args.isEmpty || eval) then
    IO.eprintln "usage: prism-bitblast [--eval] < formula.sexp"
    return 2
  let input ← readAll (← IO.getStdin)
  let forms ← match parseAll input with
    | .ok f => pure f
    | .error e => IO.eprintln s!"prism-bitblast: {e}"; return 2
  if forms.isEmpty then
    IO.eprintln "prism-bitblast: no formula"
    return 2
  let g ← match toDag forms[0]! with
    | .ok g => pure g
    | .error e => IO.eprintln s!"prism-bitblast: {e}"; return 2
  if !defsOK 0 g.defs then
    IO.eprintln "prism-bitblast: a definition reads a bit at or above its own base, or the definitions overlap"
    return 2
  let out ← IO.getStdout
  if eval then
    for f in forms[1:] do
      match toRho f with
      | .ok ρ => out.putStrLn (if (g.eval ρ).getLsbD 0 then "1" else "0")
      | .error e => IO.eprintln s!"prism-bitblast: {e}"; return 2
    return 0
  let mut header := "c prism-bitblast 1\n"
  for (b, w) in inputVars g do
    let vs := (List.range w).map (fun i => toString (2 * (b + i) + 1))
    header := header ++ s!"c var {b} {w} " ++ " ".intercalate vs ++ "\n"
  out.putStr header
  out.putStr (Std.Sat.CNF.dimacs (dagCNF g))
  out.flush
  return 0
