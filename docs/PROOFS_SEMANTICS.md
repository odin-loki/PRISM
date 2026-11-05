# PIR semantics and encoder soundness in Lean

`proofs/semantics/` defines PIR, gives it a reference semantics, and proves
that a bounded model-checking encoder with the same design as PRISM's (SSA
by substitution, `ite` at joins, `k`-unrolling with an unwinding assertion,
a read-over-write heap) is exact: a model of the formula is always a real
counterexample, and if the formula is unsatisfiable the program is safe.
It also proves that PRISM's UB instrumentation is exact.

These are proofs about a model of the design. They are not proofs about the
C++ encoder in `src/prism/pir/`. What separates the two is in
[The gap](#the-gap-between-this-model-and-the-c-encoder).

## Project

| | |
|---|---|
| Location | `proofs/semantics/`, a Lake project (`PrismSem`) |
| Toolchain | `leanprover/lean4:v4.34.0` (`proofs/semantics/lean-toolchain`), core Lean only: no Mathlib, no other dependencies |
| Build | `cd proofs/semantics && lake build` |
| Audit | `./check.sh` builds the project and fails on `sorry`, on any escape hatch in the sources (`admit`, `native_decide`, `bv_decide`, `implemented_by`, `extern`, a top-level `axiom` or `unsafe`), or on any of the 47 theorems in `Audit.lean` using an axiom other than `propext`, `Classical.choice`, `Quot.sound` |
| CI | `.github/workflows/proofs-semantics.yml` runs `check.sh` on every change under `proofs/semantics/` and uploads `axioms.txt`; `.github/workflows/proofs-recheck.yml` rechecks the project independently ([PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md#independent-proof-rechecking)) |

`bv_decide` and `native_decide` are banned because they rely on
`Lean.ofReduceBool`, which trusts the compiler. The concrete examples in
`Examples.lean` are checked with `decide`, which the kernel evaluates.

| File | Contents |
|---|---|
| `PrismSem/Syntax.lean` | PIR syntax: types, expressions, statements, fragments (`StraightLine`, `LoopFree`) |
| `PrismSem/Eval.lean` | Environments, expression evaluation, the UB predicate `ubE`, the syntactic UB condition `ubExpr` |
| `PrismSem/Semantics.lean` | Outcomes, the bounded semantics `run k`, the reference big-step semantics `BigStep`, and how the two relate |
| `PrismSem/Instrument.lean` | UB instrumentation `instr` and its exactness theorem |
| `PrismSem/Encode.lean` | The bounded encoder `enc`/`encode` and its soundness and completeness |
| `PrismSem/Memory.lean` | The memory model, `mrun`, `MBigStep`, memory-safety instrumentation `minstr` |
| `PrismSem/MemEncode.lean` | The symbolic heap and the memory encoder `menc`/`mencode` with its soundness and completeness |
| `PrismSem/Examples.lean` | Checks that the theorems are not vacuous: concrete programs whose outcomes and VCs are evaluated by the kernel |
| `Audit.lean` | `#print axioms` for every headline theorem |

`proofs/refinement` imports this project for the PIR expression semantics
(`PrismSem.evalBin`, `evalPred`, `ubBin`).

## What is modelled

### Syntax (bitvector fragment)

* **Types.** Only bitvectors `BitVec w`, of any width. Booleans are
  `BitVec 1`, as LLVM's `i1`. Expressions are indexed by width
  (`Expr : Nat → Type`), so every expression is well typed by
  construction. A variable is a name together with a width.
* **Expressions.** `const` and `var`. Binary operations `add sub mul udiv
  sdiv urem srem shl lshr ashr and or xor`, with flags
  `{nsw, nuw, total}`. `icmp` with the predicates `eq ne ult ule ugt uge slt
  sle sgt sge`. `ovf` (the `*.with.overflow` predicates). `select`,
  `zext`, `sext` and `trunc`.
* **Statements.** `skip`, `assign`, `assert` (tagged `user` or `ub`),
  `assume`, `seq`, `ite` (if/else), `loop` (while) and `ret` (return).
* **With memory** (`Mem.MStmt`). The statements above without `ret`, plus
  `alloc p n`, `free p`, `gep q p e` (q := p + e), `load x p` (x : i8),
  `store p v` (v : i8), and `assert` on the memory predicates
  `live p`, `inBounds p` and `atBase p`.

### Semantics

* The **state** is an environment `Env := (w : Nat) → String → BitVec w`.
  The initial environment is the program input. With memory, the state is
  `MState = {ρ, π, heap, next}`:
  * `π` maps pointer variables to pointers `⟨obj, off⟩`, both `BitVec 64`;
  * `heap : BitVec 64 → Obj`, where each object has a size, a liveness
    bit and its bytes;
  * `next` is the next object id. It starts at 1; object 0 is null and is
    never allocated.
* **Outcomes** are `normal ρ`, `ret ρ`, `fail t` (assertion `t` was
  false), `blocked` (an `assume` was false, so the path does not exist),
  `ub`, and `unwind` (bounded semantics only).
* **UB is part of the semantics.** `ubE ρ e` is true when evaluating `e`
  runs any operation whose C behaviour is undefined:
  * signed overflow of `add/sub/mul nsw`, and unsigned overflow of `nuw`
    operations;
  * division or remainder by zero;
  * `INT_MIN / -1` and `INT_MIN % -1`;
  * a shift amount `>= w`, read as unsigned, so negative amounts count too.

  Operands are evaluated eagerly, including both arms of `select`, which
  matches LLVM.

  With memory, `ub` also covers: `load` or `store` through a dead object
  (null dereference, use after free, wild pointer); `load` or `store` at an
  offset that is not `< size` (out of bounds, including negative offsets);
  `free` of a dead object (double free, freeing null); `free` at an offset
  other than 0 (invalid free).
* **Two semantics.** `run k` (and `Mem.mrun k`) is the bounded semantics:
  each entry into a loop may run the body at most `k` times, and needing one
  more iteration gives `unwind`. `BigStep` (and `Mem.MBigStep`) is the
  unbounded reference semantics, the meaning of "the program is safe". The
  two are proved to agree (`bigStep_iff_run`).

## Theorems

All proved with no `sorry`. `ρ₀` ranges over all inputs.

### Properties of the semantics

| Lean name | Statement |
|---|---|
| `ubExpr_correct` | `truth (evalE ρ (ubExpr e)) = ubE ρ e`: the syntactic UB condition is exact |
| `ubE_ubExpr` | `ubE ρ (ubExpr e) = false`: the UB check itself cannot have UB |
| `BigStep.det` | `BigStep s ρ o₁ → BigStep s ρ o₂ → o₁ = o₂` |
| `BigStep.not_unwind` | `BigStep s ρ o → o ≠ .unwind` |
| `run_sound` | `run k s ρ = o → o ≠ .unwind → BigStep s ρ o`: bounded runs are real runs |
| `run_mono` | `k ≤ k' → run k s ρ ≠ .unwind → run k' s ρ = run k s ρ` |
| `run_adequate` | `BigStep s ρ o → ∃ k, run k s ρ = o` |
| `bigStep_iff_run` | `BigStep s ρ o ↔ o ≠ .unwind ∧ ∃ k, run k s ρ = o` |
| `Mem.MBigStep.det`, `Mem.mrun_sound`, `Mem.mrun_adequate`, `Mem.mbigStep_iff_mrun` | The same results for the semantics with memory |
| `Mem.uaf_is_ub`, `Mem.double_free_is_ub`, `Mem.oob_is_ub`, `Mem.null_deref_is_ub`, `Mem.store_load_ok` | Sanity checks: the classic memory errors are `ub`, and an in-bounds store followed by a load reads the byte back |

### Property instrumentation

`instr` inserts `assert[ub] ¬(ubExpr e)` before every evaluation of an
expression `e` (for a loop condition: on entry and after every iteration).
`Mem.minstr` does the same for memory operations: it inserts
`assert[ub] live p`, then `inBounds p` or `atBase p`, before `load`, `store`
and `free`.

| Lean name | Statement |
|---|---|
| `run_instr` | `run k (instr s) ρ = mapUb (run k s ρ)`: the instrumented program behaves exactly like the original, except that `ub` becomes `fail .ub` |
| `instr_fail_ub_iff` | `s.NoUbTags → (run k (instr s) ρ = .fail .ub ↔ run k s ρ = .ub)`: **an inserted assertion fails iff the original program executes an operation with UB** |
| `instr_no_ub` | `run k (instr s) ρ ≠ .ub` |
| `instr_fail_user_iff` | `run k (instr s) ρ = .fail .user ↔ run k s ρ = .fail .user` |
| `bigStep_instr` | `BigStep (instr s) ρ o' ↔ ∃ o, BigStep s ρ o ∧ o' = mapUb o` |
| `bigStep_instr_fail_ub_iff` | `s.NoUbTags → (BigStep (instr s) ρ (.fail .ub) ↔ BigStep s ρ .ub)` |
| `Mem.mrun_minstr` | `mrun k (minstr s) st = mapUbM (mrun k s st)`, covering memory errors |
| `Mem.minstr_fail_ub_iff` | `s.NoUbTags → (mrun k (minstr s) st = .fail .ub ↔ mrun k s st = .ub)` |
| `Mem.mbigStep_minstr_fail_ub_iff` | `s.NoUbTags → (MBigStep (minstr s) st (.fail .ub) ↔ MBigStep s st .ub)` |

`NoUbTags` says that the user did not write assertions carrying PRISM's
own `ub` tag.

### The bounded encoder

The encoder is `enc k : Stmt → SymSt → SymSt`, and
`encode k p = enc k p SymSt.init`:

* **Symbolic state.** `SymSt` holds a substitution `σ` (the current SSA
  value of each variable as an expression over the inputs), a path guard
  `g` and three accumulated violation formulas: `fl` (an assertion failed),
  `ub` and `uw` (unwinding).
* **Straight-line code** is encoded by substitution. Every evaluation first
  records its UB condition under the guard.
* **Branches.** Each arm is encoded under `g ∧ c` or `g ∧ ¬c`, and the two
  results are merged with `select c` (φ as `ite`).
* **Loops** are unrolled `k` times; the final unrolling adds `g ∧ c` to
  `uw` as the unwinding assertion.
* **Verification conditions.** `vcBounded k p = fl ∨ ub`, and
  `vcFull k p = fl ∨ ub ∨ uw`. `Sat φ := ∃ ρ₀, truth (evalE ρ₀ φ) = true`.

The central lemma `enc_spec` is an exact simulation: for every symbolic
state `S` and input `ρ₀`, the guard of `enc k s S` holds iff the concrete
run is normal, each violation formula gains exactly the violation the run
has, and on a normal outcome `σ` denotes the final environment.

| Lean name | Statement |
|---|---|
| `enc_spec` | `Spec ρ₀ S (enc k s S) (target ρ₀ S (run k s))` |
| `encode_fail_iff` | `truth (evalE ρ₀ (encode k p).fl) = true ↔ ∃ t, run k p ρ₀ = .fail t` |
| `encode_ub_iff` | `truth (evalE ρ₀ (encode k p).ub) = true ↔ run k p ρ₀ = .ub` |
| `encode_unwind_iff` | `truth (evalE ρ₀ (encode k p).uw) = true ↔ run k p ρ₀ = .unwind` |
| `bmc_sound` | `¬ Sat (vcBounded k p) → ∀ ρ₀, (∀ t, run k p ρ₀ ≠ .fail t) ∧ run k p ρ₀ ≠ .ub`: **unsat ⇒ no failure within the bound** |
| `bmc_complete` | `truth (evalE ρ₀ (vcBounded k p)) = true → (∃ t, BigStep p ρ₀ (.fail t)) ∨ BigStep p ρ₀ .ub`: every model is a real counterexample |
| `bmc_sound_unbounded` | `¬ Sat (vcFull k p) → ∀ ρ₀, (∃ o, BigStep p ρ₀ o) ∧ ∀ o, BigStep p ρ₀ o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub`: with the unwinding assertion, **unsat ⇒ every input terminates and no unbounded execution fails** |
| `bmc_complete_full` | A model of `vcFull` is a real counterexample, or `run k p ρ₀ = .unwind` (the bound is too small) |
| `bmc_complete_limit` | Every real failure is found by `vcBounded k p` for some `k` |
| `straightLine_encode_exact` | For `p.StraightLine`: `¬ Sat (vcBounded k p) ↔ ∀ ρ₀ o, BigStep p ρ₀ o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub` |
| `loopFree_encode_exact` | The same statement for `p.LoopFree` (branches) |
| `instr_encode_sound` | `¬ Sat (encode k (instr p)).fl → ∀ ρ₀, run k p ρ₀ ≠ .ub ∧ ∀ t, run k p ρ₀ ≠ .fail t`: checking the instrumented program is sound for the original |

`bmc_sound`, `bmc_complete`, `bmc_sound_unbounded` and `bmc_complete_full`
hold for all programs, loops included. (The `bmc_` names describe bounded
model checking in this model; they are unrelated to the deleted `bmc`
stage.)

### The memory encoder

The symbolic heap in `Mem.MSym` is fully symbolic, object ids included.
Each component is a function from symbolic indices to expressions, built
as read-over-write `select` chains:

* `alloc`: `size o := select (o = next) sz (size o)`, and the same for
  `live` and `data`; `next := next + 1`.
* `free`: `live o := select (o = obj p) false (live o)`.
* `store`: `data o i := select (o = obj p ∧ i = off p) v (data o i)`.
* Joins and loop exits merge every component with `select c`.

Every component built this way respects evaluation (`MSym.WF`, `menc_wf`),
which is what lets a lookup at a symbolic id denote the concrete heap at
that id.

| Lean name | Statement |
|---|---|
| `Mem.menc_spec` | `S.WF → MSpec ρ₀ S (menc k s S) (mtarget ρ₀ S (mrun k s))`: exact simulation with memory, branches and loops |
| `Mem.mencode_fail_iff`, `Mem.mencode_ub_iff`, `Mem.mencode_unwind_iff` | Each formula is exact against `mrun k p (MState.init ρ₀)` |
| `Mem.mbmc_sound` | `¬ Sat (mvcBounded k p) → ∀ ρ₀, (∀ t, mrun k p (MState.init ρ₀) ≠ .fail t) ∧ mrun k p (MState.init ρ₀) ≠ .ub`: no assertion failure, arithmetic UB, null dereference, use after free, out-of-bounds access, double free or invalid free within the bound |
| `Mem.mbmc_complete` | Every model of `mvcBounded` is a real counterexample in `MBigStep` |
| `Mem.mbmc_sound_unbounded` | `¬ Sat (mvcFull k p) →` every input terminates and no `MBigStep` execution fails or has UB |
| `Mem.mloopFree_encode_exact` | For `p.LoopFree`: `¬ Sat (mvcBounded k p) ↔ ∀ ρ₀ o, MBigStep p (MState.init ρ₀) o → (∀ t, o ≠ .fail t) ∧ o ≠ .ub` |
| `Mem.minstr_encode_sound` | Checking `minstr p` through the memory encoder is sound for `p` |

## Axioms

`check.sh` prints the axioms of the 47 theorems in `Audit.lean` and fails on
anything outside `propext`, `Classical.choice`, `Quot.sound`. The project
declares no axioms; there is no `sorryAx` and no `Lean.ofReduceBool`.

## The gap between this model and the C++ encoder

**The C++ encoder in `src/prism/pir/` is not proved to implement
`encode`/`mencode`.** Getting from these theorems to "PRISM's `PROVED` is
sound" still needs:

1. **C++ PIR ↔ `Stmt`/`MStmt`.** The C++ PIR is a CFG with phis
   (`include/prism/pir.hpp`). `proofs/refinement/PrismRefine/Pir.lean`
   models that CFG form and links it to this project only through the
   expression semantics (`evalBin`, `evalPred`, `ubBin`; theorem
   `checks_eq_ubBin`), not through a proof that CFG programs and
   `PrismSem.Stmt` programs are equivalent.
2. **C++ encoder ↔ `encode`.** The C++ encoder (`encode.cpp`) emits SMT
   terms with fresh SSA constants and defining equalities, and shares terms
   as a DAG. The Lean encoder substitutes definitions away and has no
   sharing. Closing this needs a proof that the SSA form with definitions is
   equisatisfiable with the substituted form, and either a proof of the C++
   code or per-run validation of the formula. Neither exists.
3. **Satisfiability over `Env` ↔ SMT satisfiability.** `Sat` quantifies
   over total environments. The link to SMT-LIB models of the free
   variables needs a "free variables only" lemma. It is not written.
4. **Bitvector semantics ↔ SMT-LIB.** Lean's `BitVec` and SMT-LIB agree on
   every operation except division and remainder by zero (SMT-LIB:
   `bvudiv x 0 = ~0`; Lean: `x / 0 = 0`). In this encoder such values only
   flow into formulas already guarded by the corresponding UB condition, so
   the difference should not matter. That is argued here, not proved.
5. **Solver.** An `unsat` answer is trusted unless certified mode checks it
   (`docs/TRUSTED_BASE.md`).
6. **LLVM IR → PIR.** Proved for a fragment of LLVM IR in
   `proofs/refinement` ([PROOFS_REFINEMENT.md](PROOFS_REFINEMENT.md)).
7. **Clang, the Lean kernel and compiler, hardware** stay trusted
   (`docs/TRUSTED_BASE.md`).

### Modelling simplifications

* **Types.** Only bitvectors: no IEEE floats, no aggregates. Pointers exist
  only in the memory layer, as a separate variable namespace. They cannot
  be stored in memory, compared, cast to or from integers, or passed as
  inputs; every pointer variable starts null and the program allocates what
  it uses.
* **Statements.** No calls, no `havoc`, no concurrency. Nondeterminism comes
  only from the inputs. With memory there is no `ret`.
* **UB not covered:** C's left shift of negative values or into the sign
  bit; invalid pointer arithmetic (`gep` is unchecked); misaligned access
  and mismatched `delete`/`delete[]` (no alloc kinds, no alignment); reads
  of uninitialised memory (fresh objects are zero-filled); leaks, strict
  aliasing and effective type; allocation failure; object-id wraparound
  after 2^64 allocations. (`proofs/refinement` covers several of these for
  its LLVM fragment.)
* **Memory access width.** Loads and stores move one byte (`i8`): no
  multi-byte access, no endianness.
* **Assertions.** Execution stops at the first failing assertion; the
  encoder's `fl` is "some assertion fails first". The soundness direction is
  the same as per-assertion reporting.
* **`select`** is eager: UB in either arm is UB, as in LLVM. A front end
  that lowered C `?:` to `select` without branches would report UB that C
  does not have.
* **Loop bound.** `k` applies per loop entry, as CBMC's `--unwind`.
  k-induction is modelled separately in `proofs/techniques`
  ([PROOFS_TECHNIQUES.md](PROOFS_TECHNIQUES.md)).
* **Sharing.** Formula size is not modelled: substitution duplicates
  terms. This affects performance, not meaning.
