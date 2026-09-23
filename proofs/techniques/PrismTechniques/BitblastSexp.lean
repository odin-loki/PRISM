/-
PRISM techniques: the text format of the proved bit-blaster's input
(`prism-bitblast`, `prism-lrat-check --dag`).  Not part of any proof: the
theorems are about the parsed `Dag`, and `prism-bitblast --eval` evaluates
that parsed `Dag` with the proved semantics (`Dag.eval`) so PRISM's C++
serializer is tested against it (docs/TRUSTED_BASE.md).

Grammar (S-expressions; numbers are decimal):

    dag   ::= (dag def* expr)            ; expr must have width 1
    def   ::= (def W BASE expr)          ; bits BASE..BASE+W-1 := expr (width W)
    expr  ::= (var W BASE) | (const W VALUE)
            | (not e) | (neg e)
            | (and e e) | (or e e) | (xor e e) | (add e e) | (sub e e) | (mul e e)
            | (udiv e e) | (urem e e) | (sdiv e e) | (srem e e)
            | (shl e e) | (lshr e e) | (ashr e e)
            | (shlc K e) | (lshrc K e) | (ashrc K e)
            | (ite c e e)                ; c has width 1
            | (eq e e) | (ult e e) | (ule e e) | (slt e e) | (sle e e)
            | (uaddo e e) | (saddo e e) | (usubo e e) | (ssubo e e) | (umulo e e)
            | (smulhi e e) | (smullo e e)
            | (zext N e) | (sext N e) | (extract LO LEN e) | (concat e e)
    rho   ::= (rho (BASE W VALUE)*)      ; an input assignment, for --eval

Each constructor is the `BVExpr` constructor of the same name; binary
operators need equal widths, and a width mismatch is a parse error.
-/
import PrismTechniques.BitblastEncode

namespace PrismTechniques.Bitblast.Sexp

open PrismTechniques.Bitblast

inductive Sexp where
  | atom (s : String)
  | list (xs : Array Sexp)
  deriving Inhabited

partial def Sexp.toString : Sexp → String
  | .atom s => s
  | .list xs => "(" ++ " ".intercalate (xs.toList.map Sexp.toString) ++ ")"

/-- Parse every top-level S-expression in the input (iteratively, so deep
nesting does not exhaust the stack). -/
def parseAll (input : ByteArray) : Except String (Array Sexp) := Id.run do
  let mut stack : Array (Array Sexp) := #[#[]]
  let mut i := 0
  let n := input.size
  while i < n do
    let c := input.get! i
    if c == 40 then -- '('
      stack := stack.push #[]
      i := i + 1
    else if c == 41 then -- ')'
      if stack.size ≤ 1 then return .error s!"unbalanced ')' at byte {i}"
      let top := stack.back!
      stack := stack.pop
      stack := stack.modify (stack.size - 1) (·.push (.list top))
      i := i + 1
    else if c == 59 then -- ';' comment to end of line
      while i < n && input.get! i != 10 do i := i + 1
    else if c == 32 || c == 10 || c == 13 || c == 9 then
      i := i + 1
    else
      let start := i
      while i < n && (let d := input.get! i; d != 40 && d != 41 && d != 32 && d != 10 && d != 13 &&
          d != 9 && d != 59) do
        i := i + 1
      let tok := String.fromUTF8! (input.extract start i)
      stack := stack.modify (stack.size - 1) (·.push (.atom tok))
  if stack.size != 1 then return .error "unbalanced '(' at end of input"
  return .ok stack[0]!

abbrev TExpr := Σ w, BVExpr w

def num (s : Sexp) : Except String Nat :=
  match s with
  | .atom a => match a.toNat? with
    | some v => .ok v
    | none => .error s!"expected a number, got {a}"
  | _ => .error s!"expected a number, got {s.toString}"

def castTo (w : Nat) (e : TExpr) : Except String (BVExpr w) :=
  if h : e.1 = w then .ok (h ▸ e.2) else .error s!"width mismatch: {e.1} vs {w}"

/-- Type-check an S-expression into a `BVExpr` of some width. -/
partial def toExpr (s : Sexp) : Except String TExpr := do
  match s with
  | .list xs =>
    if h : xs.size = 0 then throw "empty expression" else
    let op := match xs[0] with | .atom a => a | _ => ""
    let arg (i : Nat) : Except String Sexp :=
      if h : i < xs.size then .ok xs[i] else .error s!"{op}: missing argument {i}"
    let need (k : Nat) : Except String Unit :=
      if xs.size = k + 1 then .ok () else .error s!"{op}: expected {k} arguments, got {xs.size - 1}"
    let bin (mk : {w : Nat} → BVExpr w → BVExpr w → TExpr) : Except String TExpr := do
      need 2
      let a ← toExpr (← arg 1)
      let b ← castTo a.1 (← toExpr (← arg 2))
      return mk a.2 b
    match op with
    | "var" => need 2; let w ← num (← arg 1); let b ← num (← arg 2); return ⟨w, .var b⟩
    | "const" =>
      need 2; let w ← num (← arg 1); let v ← num (← arg 2); return ⟨w, .const (BitVec.ofNat w v)⟩
    | "not" => need 1; let a ← toExpr (← arg 1); return ⟨a.1, .not a.2⟩
    | "neg" => need 1; let a ← toExpr (← arg 1); return ⟨a.1, .neg a.2⟩
    | "and" => bin fun a b => ⟨_, .and a b⟩
    | "or" => bin fun a b => ⟨_, .or a b⟩
    | "xor" => bin fun a b => ⟨_, .xor a b⟩
    | "add" => bin fun a b => ⟨_, .add a b⟩
    | "sub" => bin fun a b => ⟨_, .sub a b⟩
    | "mul" => bin fun a b => ⟨_, .mul a b⟩
    | "udiv" => bin fun a b => ⟨_, .udiv a b⟩
    | "urem" => bin fun a b => ⟨_, .urem a b⟩
    | "sdiv" => bin fun a b => ⟨_, .sdiv a b⟩
    | "srem" => bin fun a b => ⟨_, .srem a b⟩
    | "shl" => bin fun a b => ⟨_, .shl a b⟩
    | "lshr" => bin fun a b => ⟨_, .lshr a b⟩
    | "ashr" => bin fun a b => ⟨_, .ashr a b⟩
    | "eq" => bin fun a b => ⟨1, .eq a b⟩
    | "ult" => bin fun a b => ⟨1, .ult a b⟩
    | "ule" => bin fun a b => ⟨1, .ule a b⟩
    | "slt" => bin fun a b => ⟨1, .slt a b⟩
    | "sle" => bin fun a b => ⟨1, .sle a b⟩
    | "uaddo" => bin fun a b => ⟨1, .uaddo a b⟩
    | "saddo" => bin fun a b => ⟨1, .saddo a b⟩
    | "usubo" => bin fun a b => ⟨1, .usubo a b⟩
    | "ssubo" => bin fun a b => ⟨1, .ssubo a b⟩
    | "umulo" => bin fun a b => ⟨1, .umulo a b⟩
    | "smulhi" => bin fun a b => ⟨1, .smulHi a b⟩
    | "smullo" => bin fun a b => ⟨1, .smulLo a b⟩
    | "shlc" => need 2; let k ← num (← arg 1); let a ← toExpr (← arg 2); return ⟨a.1, .shlC k a.2⟩
    | "lshrc" => need 2; let k ← num (← arg 1); let a ← toExpr (← arg 2); return ⟨a.1, .lshrC k a.2⟩
    | "ashrc" => need 2; let k ← num (← arg 1); let a ← toExpr (← arg 2); return ⟨a.1, .ashrC k a.2⟩
    | "zext" => need 2; let n ← num (← arg 1); let a ← toExpr (← arg 2); return ⟨n, .zext n a.2⟩
    | "sext" => need 2; let n ← num (← arg 1); let a ← toExpr (← arg 2); return ⟨n, .sext n a.2⟩
    | "extract" =>
      need 3; let lo ← num (← arg 1); let len ← num (← arg 2); let a ← toExpr (← arg 3)
      return ⟨len, .extract lo len a.2⟩
    | "concat" =>
      need 2; let a ← toExpr (← arg 1); let b ← toExpr (← arg 2)
      return ⟨a.1 + b.1, .concat a.2 b.2⟩
    | "ite" =>
      need 3
      let c ← castTo 1 (← toExpr (← arg 1))
      let a ← toExpr (← arg 2)
      let b ← castTo a.1 (← toExpr (← arg 3))
      return ⟨a.1, .ite c a.2 b⟩
    | _ => throw s!"unknown operator {op}"
  | .atom a => throw s!"unexpected atom {a}"

/-- Parse a `(dag def* expr)` form. -/
def toDag (s : Sexp) : Except String Dag := do
  match s with
  | .list xs =>
    if xs.size < 2 then throw "dag: expected (dag def* expr)"
    match xs[0]! with
    | .atom "dag" => pure ()
    | _ => throw "dag: expected (dag def* expr)"
    let mut defs : Array Def := #[]
    for i in [1:xs.size - 1] do
      match xs[i]! with
      | .list d =>
        if d.size != 4 then throw "def: expected (def W BASE expr)"
        match d[0]! with
        | .atom "def" => pure ()
        | _ => throw "def: expected (def W BASE expr)"
        let w ← num d[1]!
        let b ← num d[2]!
        let e ← castTo w (← toExpr d[3]!)
        defs := defs.push ⟨w, b, e⟩
      | _ => throw "def: expected (def W BASE expr)"
    let top ← castTo 1 (← toExpr xs[xs.size - 1]!)
    return ⟨defs.toList, top⟩
  | _ => throw "dag: expected (dag def* expr)"

/-- Parse a `(rho (BASE W VALUE)*)` assignment. -/
def toRho (s : Sexp) : Except String (Nat → Bool) := do
  match s with
  | .list xs =>
    match xs[0]? with
    | some (.atom "rho") => pure ()
    | _ => throw "rho: expected (rho (BASE W VALUE)*)"
    let mut ents : Array (Nat × Nat × Nat) := #[]
    for i in [1:xs.size] do
      match xs[i]! with
      | .list t =>
        if t.size != 3 then throw "rho: expected (BASE W VALUE)"
        ents := ents.push (← num t[0]!, ← num t[1]!, ← num t[2]!)
      | _ => throw "rho: expected (BASE W VALUE)"
    let es := ents
    return fun j => match es.find? (fun (b, w, _) => b ≤ j && j < b + w) with
      | some (b, _, v) => v.testBit (j - b)
      | none => false
  | _ => throw "rho: expected (rho (BASE W VALUE)*)"

/-! ## Free input variables (for the variable map) -/

partial def collectVars : {w : Nat} → BVExpr w → Array (Nat × Nat) → Array (Nat × Nat)
  | w, .var base, acc => if acc.contains (base, w) then acc else acc.push (base, w)
  | _, .const _, acc => acc
  | _, .not e, acc | _, .neg e, acc | _, .shlC _ e, acc | _, .lshrC _ e, acc
  | _, .ashrC _ e, acc | _, .zext _ e, acc | _, .sext _ e, acc | _, .extract _ _ e, acc =>
    collectVars e acc
  | _, .ite s a b, acc => collectVars b (collectVars a (collectVars s acc))
  | _, .and a b, acc | _, .or a b, acc | _, .xor a b, acc | _, .add a b, acc | _, .eq a b, acc
  | _, .ult a b, acc | _, .slt a b, acc | _, .mul a b, acc | _, .sub a b, acc | _, .ule a b, acc
  | _, .sle a b, acc | _, .shl a b, acc | _, .lshr a b, acc | _, .ashr a b, acc
  | _, .concat a b, acc | _, .uaddo a b, acc | _, .saddo a b, acc | _, .usubo a b, acc
  | _, .ssubo a b, acc | _, .umulo a b, acc | _, .smulHi a b, acc | _, .smulLo a b, acc
  | _, .udiv a b, acc | _, .urem a b, acc | _, .sdiv a b, acc | _, .srem a b, acc =>
    collectVars b (collectVars a acc)

/-- Input variables: `(base, width)` of every `var` that is not a definition. -/
def inputVars (g : Dag) : Array (Nat × Nat) :=
  let all := g.defs.foldl (fun acc d => collectVars d.e acc) (collectVars g.top #[])
  let defined := g.defs.map (·.base)
  all.filter (fun (b, _) => !defined.contains b)

end PrismTechniques.Bitblast.Sexp
