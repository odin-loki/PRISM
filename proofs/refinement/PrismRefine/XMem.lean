/-
PRISM refinement, extended fragment — the byte-addressed memory both the
LLVM semantics (`XLlvm.lean`) and the PIR semantics (`XPir.lean`) run on,
and the *world* they thread: the oracle counter and the memory.

The model is the (object, offset) model of `proofs/semantics`
(`PrismSem/Memory.lean`: objects with a size, a liveness bit and byte
contents, object `0` is null and never allocated, allocation takes the next
id) with what the C++ engine adds (`src/prism/pir/memory.hpp`, `ConcMem`):
every byte cell is *initialised* or not (`none`), every object has a kind
(`MemKind`: 1 stack, 3 static, 4 read-only, …) and a base alignment, and a
pointer is one 64-bit value, the object id in bits 63..48 and the offset in
bits 47..0 (`ptrObj`, `ptrOff`).  Reads outside a live object's cells read
an uninitialised zero byte and writes there are dropped, as `ConcMem`
does; every access the translator emits is preceded by the checks that rule
this out (null, wild, freed, out of bounds, misaligned, read-only).
-/
import PrismRefine.Pir

namespace PrismRefine

/-- One object.  `cells[i]` is byte `i`: `some b` initialised with `b`
(`b < 256`), `none` uninitialised. -/
structure MObj where
  size : Nat
  live : Bool
  kind : Nat
  align : Nat
  cells : List (Option Nat)
  deriving DecidableEq, Repr, Inhabited

/-- Object `i + 1` is `objs[i]`. -/
structure Mem where
  objs : List MObj
  deriving DecidableEq, Repr, Inhabited

def ptrObj (p : Nat) : Nat := p / 2 ^ 48
def ptrOff (p : Nat) : Nat := p % 2 ^ 48
def mkPtr (o f : Nat) : Nat := (o * 2 ^ 48 + f) % 2 ^ 64

def Mem.obj? (m : Mem) (p : Nat) : Option MObj :=
  if ptrObj p = 0 then none else m.objs[ptrObj p - 1]?

/-- `Op::ObjSize` / `ObjLive` / `ObjKind` / `ObjAlign` (`ConcMem::size` …):
0 / false / 0 / 1 for a pointer to no object; as the C++ model's machine
integers (64-bit size and alignment, 8-bit kind). -/
def Mem.size (m : Mem) (p : Nat) : Nat := ((m.obj? p).map (·.size)).getD 0 % 2 ^ 64
def Mem.live (m : Mem) (p : Nat) : Bool := ((m.obj? p).map (·.live)).getD false
def Mem.kind (m : Mem) (p : Nat) : Nat := ((m.obj? p).map (·.kind)).getD 0 % 256
def Mem.align (m : Mem) (p : Nat) : Nat := ((m.obj? p).map (·.align)).getD 1 % 2 ^ 64

/-- An access of `n` bytes at `p` (a 64-bit pointer) is undefined:
through null (`PTR-NULL-DEREF`), through a pointer to no object
(`PTR-INVALID-DEREF`), to an object whose lifetime ended (`MEM-UAF`),
beyond the object's end (`MEM-OOB-READ/WRITE`), at an address that is not a
multiple of the access's `align` or in an object less aligned
(`MEM-MISALIGNED`), or a write to a read-only object (`MEM-WRITE-CONST`,
kind 4).  These are the conditions `MemTr::access_checks` tests. -/
def accessBad (m : Mem) (p n : Nat) (write : Bool) (al : Nat) : Bool :=
  ptrObj p == 0 ||
  (ptrObj p != 0 && m.kind p == 0) ||
  (m.kind p != 0 && !m.live p) ||
  (m.live p && decide (m.size p < ptrOff p + n)) ||
  (decide (1 < al) && m.live p && (ptrOff p % al != 0 || decide (m.align p < al))) ||
  (write && m.live p && m.kind p == 4)

/-- The cell at address `a` (`ConcMem::read`). -/
def Mem.read (m : Mem) (a : Nat) : Option Nat :=
  match m.obj? a with
  | some o => (o.cells[ptrOff a]?).getD none
  | none => none

/-- Write the cell at address `a` (`ConcMem::write`; dropped outside an
object's cells). -/
def Mem.write (m : Mem) (a : Nat) (c : Option Nat) : Mem :=
  if ptrObj a = 0 then m
  else { objs := m.objs.modify (ptrObj a - 1) fun o => { o with cells := o.cells.set (ptrOff a) c } }

/-- `ConcMem::alloc`: a live object of `size` bytes, uninitialised
(`init = 0`) or zero (`init = 1`); the pointer to its first byte. -/
def Mem.alloc (m : Mem) (size kind align init : Nat) : Mem × Nat :=
  let o : MObj := { size := size, live := true, kind := kind, align := max 1 align,
                    cells := List.replicate size (if init = 1 then some 0 else none) }
  ({ objs := m.objs ++ [o] }, mkPtr (m.objs.length + 1) 0)

/-- `ConcMem::free`: the object ends its lifetime. -/
def Mem.free (m : Mem) (p : Nat) : Mem :=
  if ptrObj p = 0 then m
  else { objs := m.objs.modify (ptrObj p - 1) fun o => { o with live := false } }

/-- The `n` cells from address `p` (little endian). -/
def Mem.readN (m : Mem) (p : Nat) (n : Nat) : List (Option Nat) :=
  (List.range n).map fun k => m.read ((p + k) % 2 ^ 64)

/-- Store the `n` low bytes of `v`, initialised or not. -/
def Mem.writeN (m : Mem) (p v n : Nat) (init : Bool) : Mem :=
  (List.range n).foldl (fun m k => m.write ((p + k) % 2 ^ 64)
    (if init then some ((v / 2 ^ (8 * k)) % 256) else none)) m

/-- `Stmt::MemCpy` (the interpreter's loop): the `n` cells from `s` are read
first, then written from `d` (memmove semantics), initialised or not. -/
def Mem.copy (m : Mem) (d s n : Nat) : Mem :=
  (List.range n).foldl (fun m' k => m'.write ((d + k) % 2 ^ 64) ((m.readN s n).getD k none)) m

/-- `Stmt::MemSet`: `n` initialised bytes `b` from `d`. -/
def Mem.fill (m : Mem) (d b n : Nat) : Mem :=
  (List.range n).foldl (fun m' k => m'.write ((d + k) % 2 ^ 64) (some b)) m

/-- Little-endian value of initialised bytes. -/
def bytesVal : List Nat → Nat
  | [] => 0
  | b :: bs => b + 256 * bytesVal bs

/-- The world a run threads: how many arbitrary values it has drawn from the
oracle, and the memory. -/
structure World where
  t : Nat
  mem : Mem
  deriving Inhabited

def World.init : World := { t := 0, mem := { objs := [] } }

/-- `n` more values drawn. -/
def World.adv (W : World) (n : Nat) : World := { W with t := W.t + n }

/-- The cells a `load` into a variable of width `w` reads. -/
def loadCells (m : Mem) (p w : Nat) : List (Option Nat) := m.readN (p % 2 ^ 64) ((w + 7) / 8)

/-- `Stmt::Store` of a value of width `w` (little endian, each byte
initialised or not). -/
def World.store (t : World) (p v w : Nat) (init : Bool) : World :=
  { t with mem := t.mem.writeN (p % 2 ^ 64) v ((w + 7) / 8) init }

@[simp] theorem World.adv_zero (W : World) : W.adv 0 = W := by
  cases W; simp [World.adv]

@[simp] theorem World.adv_t (W : World) (n : Nat) : (W.adv n).t = W.t + n := rfl

end PrismRefine
