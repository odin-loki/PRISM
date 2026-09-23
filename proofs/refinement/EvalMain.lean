/-
`llvm_eval FILE.pirl [FUEL] < queries` — run the Lean semantics on concrete
inputs (roadmap 8.3: the formal LLVM semantics "is tested against `lli`").

Each query line is `FUNCTION a1 a2 ...` (arguments as unsigned two's
complement values).  For every query the answer line is

    FUNCTION a1 a2 ... | lazy=<outcome> strict=<outcome> pir=<outcome>

where `lazy` is the LangRef semantics `lRunF`, `strict` the strict one and
`pir` the PIR semantics of the Lean translation (`translate`).  Outcomes:
`ret V`, `ret-void`, `ret-poison-created V`, `ub`, `fail`, `stuck`, `fuel`,
`stop`, `blocked`, or `outside: <why>` for a function not in the fragment.
`tools/llvm_sem_vs_lli.py` compares the `lazy` values with `lli`.
-/
import PrismRefine.Check
import PrismRefine.Refine

open PrismRefine PrismRefine.Check

def showL : LOut → String
  | .ret (some v) false => s!"ret {v}"
  | .ret none false => "ret-void"
  | .ret (some v) true => s!"ret-poison-created {v}"
  | .ret none true => "ret-void-poison-created"
  | .ub => "ub"
  | .stuck => "stuck"
  | .fuel c => if c then "fuel-poison-created" else "fuel"

def showS : Out → String
  | .ret (some v) => s!"ret {v}"
  | .ret none => "ret-void"
  | .ub => "ub"
  | .stuck => "stuck"
  | .fuel => "fuel"

def showP : POut → String
  | .ret (some v) => s!"ret {v}"
  | .ret none => "ret-void"
  | .fail => "fail"
  | .blocked => "blocked"
  | .stop => "stop"
  | .fuel => "fuel"

partial def loop (stdin : IO.FS.Stream) (recs : List Record) (fuel : Nat) : IO Unit := do
  let line ← stdin.getLine
  if line.isEmpty then return
  let ws := words line.trimAsciiEnd.toString
  match ws with
  | [] => loop stdin recs fuel
  | fn :: rest =>
    let args := rest.filterMap String.toNat?
    let ans := match recs.find? (·.name == fn) with
      | none => "outside: no such function"
      | some r =>
        match r.llvm with
        | .error e => s!"outside: {e}"
        | .ok F =>
          let pir := match translate F with
            | .ok P => showP (pRunF P args fuel)
            | .error e => s!"outside: {e}"
          s!"lazy={showL (lRunF F args fuel)} strict={showS (sRunF F args fuel)} pir={pir}"
    IO.println s!"{line.trimAsciiEnd.toString} | {ans}"
    (← IO.getStdout).flush
    loop stdin recs fuel

def main (args : List String) : IO UInt32 := do
  match args with
  | file :: rest =>
    let fuel := (rest.head? >>= String.toNat?).getD 5000
    let recs := parseFile (← IO.FS.readFile file)
    loop (← IO.getStdin) recs fuel
    return 0
  | [] =>
    IO.eprintln "usage: llvm_eval FILE.pirl [FUEL] < queries"
    return 2
