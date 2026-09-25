# LLVM IR to PIR: refinement proof, correspondence checking, independent rechecking

This document covers roadmap Part 8.2 rows "LLVM IR to PIR translation"
and "Property instrumentation" (for the C++ instrumentation list itself),
the "Floating point" row (IEEE operations, flags and PRISM's floating-point
checks; see "Floating point" below for what is not covered), the Part 8.3 mitigation
"formal LLVM semantics … tested against `lli`", and Part 8.5 (independent
proof rechecking in CI).

The short version:

* A fragment of LLVM IR is given a formal semantics in Lean, twice: the
  LangRef semantics with lazy poison, and the strict semantics PRISM's
  instrumentation implements.
* The translator `src/prism/pir/translate.cpp` is mirrored in Lean,
  check for check, and proved correct: for every function it accepts, every
  input and every bound on executed blocks, **if the LLVM function reaches
  undefined behaviour or creates poison, the PIR program fails a check**
  (`pir_sound`), and every PIR outcome is an LLVM outcome (`pir_faithful_*`).
  Loops are covered for any number of iterations: the theorems hold for
  every fuel bound, so they are not bounded results.
* An **extended fragment** adds `freeze`, `undef` under `freeze`, direct
  calls (inlined as `translate.cpp` inlines them), a stack memory
  fragment (`alloca`, integer `load`/`store`, `getelementptr`, with the
  bounds, lifetime, alignment, read-only and uninitialised-read checks the
  translator inserts), the intrinsics `translate.cpp` translates specially
  (`llvm.smax`/`smin`/`umax`/`umin`, `abs`, `ctlz`/`cttz`, `ctpop`, `bswap`,
  `expect`, the six `*.with.overflow` with `extractvalue`,
  `lifetime.start`/`end`, `memcpy`/`memmove`/`memset` with constant or
  variable lengths) and globals (read-only data, the globals of `main`, and
  mutable or external globals of arbitrary contents). It is proved for every
  function whose translation carries a certificate the checker validates
  (`agree-ext`), for every choice of the values LLVM leaves open.
* Each run can check that the C++ translator's output **is** the proved
  translator's output: the pir stage exports (LLVM, PIR) pairs, and
  `pir_lean_check` compares them. On `tests/pir` and `testdata` there are no
  disagreements (numbers below).
* Every Lean project in `proofs/` is rechecked in CI by `leanchecker` (the
  toolchain's replay checker, formerly lean4checker) and by **nanoda**, an
  independent Lean kernel written in Rust, and every declaration's axioms
  are audited.

What is *not* proved, and the gaps this work found in
`translate.cpp`, are listed in [What remains](#what-remains) and
[Findings](#findings-in-translatecpp).

## Project

| | |
|---|---|
| Location | `proofs/refinement/`, a Lake project |
| Toolchain | `leanprover/lean4:v4.34.0`, core Lean only (no Mathlib) |
| Dependencies | `proofs/semantics` (PIR expression semantics: `PrismSem.evalBin`, `evalPred`, `ubBin`) and `proofs/techniques` (`PrismTechniques.FloatRound.rne`), both as Lake *path* dependencies: imported, not copied |
| Build | `cd proofs/refinement && lake build` |
| Audit | `./check.sh`: build with no warning, no escape hatch in any source (`sorry`, `admit`, `native_decide`, `bv_decide`, `implemented_by`, `extern`, `axiom`, `unsafe`), every theorem in `Audit.lean` within `propext` / `Classical.choice` / `Quot.sound`, checker fixtures |
| Size | about 11 700 lines of Lean, 89 audited theorems |

| File | Contents |
|---|---|
| `PrismRefine/Llvm.lean` | LLVM fragment syntax; LangRef semantics `lRun` (lazy poison); strict semantics `sRun` |
| `PrismRefine/Pir.lean` | PIR exactly as `include/prism/pir.hpp` represents it (CFG, phis, `check`/`assume`, var table) and the semantics of `prism::pir::interpret` |
| `PrismRefine/Translate.lean` | The translator, mirroring `translate.cpp` |
| `PrismRefine/Ops.lean` | The C++ test operators (`sadd.ovf`, `shl.nsw.ovf`, …) equal the LangRef conditions |
| `PrismRefine/Refine.lean` | Simulation: `translate_exact` |
| `PrismRefine/Lazy.lean` | Strict vs LangRef semantics: `strict_lazy` |
| `PrismRefine/Sound.lean` | Headline theorems; link to `PrismSem.ubBin` |
| `PrismRefine/XMem.lean` | Extended fragment: the byte memory and the `World` (oracle counter + memory) both sides share |
| `PrismRefine/XLlvm.lean`, `XPir.lean` | Extended LLVM syntax (`freeze`, calls, memory), strict and LangRef-side semantics on a frame stack; PIR with oracle `havoc` and memory statements |
| `PrismRefine/XTranslate.lean`, `XValid.lean` | The translator with inlining and `MemTr` mirrored, its certificate, the executable check `validB` |
| `PrismRefine/XMemSim.lean` | The memory checks and `getelementptr` arithmetic compute the semantics' conditions |
| `PrismRefine/XMemOps.lean` | `llvm.memcpy`/`memmove`/`memset`: the guarded access checks and the overlap check compute `cpyBad` for every length |
| `PrismRefine/XRefine.lean`, `XStep.lean`, `XRun.lean`, `XValidSpec.lean` | Simulation: `translateX_exact` |
| `PrismRefine/XLazy.lean`, `XSound.lean` | Strict vs LangRef side (`strict_lazyX`); headline theorems `pir_sound_x` … |
| `PrismRefine/Check.lean`, `CheckMain.lean` | `pir_lean_check`: the correspondence checker |
| `EvalMain.lean` | `llvm_eval`: runs the three semantics on concrete inputs (for `tools/llvm_sem_vs_lli.py`) |
| `AxiomReport.lean` | `axiom_report`: axioms of every declaration of a project (8.5) |
| `PrismRefine/Float.lean` | IEEE 754 binary formats, correctly rounded addition with special values |
| `PrismRefine/FloatOps.lean` | Correctly rounded subtraction, multiplication, division; IEEE exception flags; PRISM's FLOAT-* conditions against IEEE 754 and C11 |
| `recheck.sh` | Independent recheck of any of the four Lean projects (8.5) |

## The LLVM fragment and its semantics

The fragment is everything `translate.cpp` handles in a single function
without calls whose values are all integers:

* SSA registers of type `iN`, `1 ≤ N ≤ 64`; integer constants; `poison`;
* `add`/`sub`/`mul` with `nsw`/`nuw`; `udiv`/`sdiv` with `exact`;
  `urem`/`srem`; `shl` with `nsw`/`nuw`; `lshr`/`ashr` with `exact`; `and`;
  `or` with `disjoint`; `xor`;
* `icmp` (ten predicates), `select`, `zext` (with `nneg`), `sext`, `trunc`,
  `phi`;
* `br label`, `br i1`, `ret`, `ret void`, `unreachable`;
* `call iN @__prism.uninit.iN()`, the marker the pir stage inserts before
  mem2reg for every scalar local (and every parameter slot). Almost every
  real function contains it, so without it the fragment would only cover
  hand-lowered IR.

Control state is (previous block, current block, register file); a run is a
deterministic interpreter whose fuel counts executed blocks. Values are
naturals read as `BitVec.ofNat w` by an instruction of width `w`; the value
of every operation is `PrismSem.evalBin` / `evalPred` from the PIR
semantics.

**LangRef semantics (`lRun`)** — the documented assumptions:

| Construct | Semantics |
|---|---|
| `add/sub/mul nsw` | poison if the mathematical signed result is out of range |
| `add/sub/mul nuw` | poison if the mathematical unsigned result is out of range |
| `udiv/urem`, `sdiv/srem` | UB if the divisor is 0; UB for `INT_MIN / -1` and `INT_MIN % -1`; UB if the divisor is poison; with a poison dividend, UB when the divisor makes some dividend UB (0, or −1 for the signed forms) |
| `udiv/sdiv exact` | poison if the dividend is not a multiple of the divisor |
| `shl/lshr/ashr` | poison if the amount is `≥ w` |
| `shl nuw` / `shl nsw` | poison if `a·2^b` is out of the unsigned / signed range |
| `lshr/ashr exact` | poison if a set bit is shifted out (`a mod 2^b ≠ 0`) |
| `or disjoint` | poison if the operands share a set bit |
| `zext nneg` | poison if the operand is negative |
| arithmetic, `icmp`, casts | poison in, poison out |
| `select` | poison condition gives poison; otherwise the chosen operand (poison or not) |
| `phi` | copies the incoming value, poison included |
| `br i1 poison` | UB |
| `ret poison` | UB — PRISM's rule, matching clang's `noundef` on C return values |
| `unreachable` | UB |
| `@__prism.uninit.iN()` | an *indeterminate* value; using it (any operand except a phi input) is UB (C11 6.3.2.1p2: an automatic object whose address is never taken, read while indeterminate); phis copy it |
| `shl` marked `csigned` | UB if the base is negative or `a·2^b ≥ 2^(w-1)` (C11 6.5.7p4). Not an LLVM flag: the pir stage learns from clang which `shl` came from a C signed `<<` (`TranslateOptions::signed_shl`) and the exporter passes that fact per instruction |
| undefined register, missing phi entry, branch to a missing block | *stuck* (IR the verifier rejects; no claim is made) |

The run also reports whether poison was ever **created** (a flag violation,
or the literal `poison` read).

**Strict semantics (`sRun`)** — identical, except that creating poison is
itself UB. This is what PRISM checks: `translate.cpp` inserts every check at
the instruction, not at the use.

Modelled on Vellvm's approach (a formal operational semantics of LLVM IR
with poison and UB as explicit outcomes) but written directly in Lean for
this fragment: there are no interaction trees. Memory, calls and
`undef`/`freeze` are in the extended fragment (next sections but one).

## The translator and the theorems

`Translate.lean` mirrors `translate.cpp` statement for statement: variable
numbering (parameters, then one variable per SSA result in textual order,
phis first, `icmp` results `i1`, then temporaries in allocation order);
block `i` of the LLVM function is PIR block `i`; each operand, check and
assignment is emitted in the same order with the same arguments, argument
widths, property name and taxonomy class; `unreachable` becomes
`check 1; stop`; unresolvable phi predecessors are dropped as
`resolve_phis` does. Uninitialised locals are mirrored from
`Tr::enter_frame` / `Tr::operand`: the marker becomes `havoc`; the
registers that may carry its value are the least fixpoint over phi inputs;
each gets a shadow — the constant `1` for the marker's result, a fresh `i1`
variable (numbered after the SSA results, before temporaries) plus a shadow
phi for such a phi — and every use of a register with a shadow is preceded
by `check shadow` (UNINIT-READ). The simulation relation carries the
shadows: a defined register has its value in its variable and a clear
shadow, an indeterminate one a set shadow (`ShOK`, `ShWF`). Where the C++ translator would throw `UNENCODED`, or
the input is outside the fragment, `translate` returns an error.

| Theorem (`PrismRefine.`) | Statement |
|---|---|
| `translate_exact` | For every accepted `F`, inputs, fuel: strict LLVM returns `v` ⇒ PIR returns `v`; strict LLVM UB ⇒ PIR fails a check; strict out of fuel ⇒ PIR out of fuel (strict *stuck*: no claim) |
| `strict_lazy` | Strict returns / runs out of fuel / is stuck ⇒ LangRef does the same and created no poison; strict UB ⇒ LangRef is UB, creates poison, or is stuck |
| **`pir_sound`** | LangRef run has UB or creates poison within `n` blocks ⇒ PIR fails a check within `n` blocks |
| `pir_sound_all` | If no PIR run fails for any bound, no LangRef run has UB or creates poison, for any bound |
| `pir_faithful_ret` | PIR returns `v` ⇒ LangRef returns `v` with no UB and no poison (or LLVM is stuck) |
| `pir_faithful_fail` | PIR fails a check ⇒ LangRef has UB or creates poison (or is stuck) |
| `pir_faithful_fuel`, `pir_no_stop` | PIR out of fuel ⇒ LangRef out of fuel; PIR never stops or blocks on its own |

"Straight-line code, then branches/phi, then loops (bounded)" is covered by
one proof: blocks, phis (parallel assignment from the predecessor) and
back edges are all handled by `run_sim`, by induction on fuel.

## The extended fragment: `freeze`, `undef`, calls, memory

The theorems above are unconditional: they hold for every function the
base translator accepts. The extended fragment adds four constructs and is
proved *per function, under a certificate*: `pir_lean_check` re-runs a
second Lean translator (`translateX`) that also returns a certificate, demands
that the C++ PIR be exactly that translator's output, and evaluates the
executable check `validB M F P C`. Every theorem below is stated for every
module `M`, function `F`, PIR function `P` and certificate `C` with
`validB M F P C = true`; the verdict for such a function is `agree-ext`.

| Construct | Accepted | Semantics (strict / LangRef side) |
|---|---|---|
| `freeze` | of any fragment operand, of `poison`, of `undef` | the operand's value; `freeze undef` and `freeze poison` draw one arbitrary value from the oracle (reading the literal `poison` still counts as creating poison, see finding 5) |
| `undef` | **only** as the operand of `freeze` or of `store` | anywhere else it is refused (finding 4) |
| `call @f(…)` | direct calls to a function defined in the same file that `Tr::call` hands to `Tr::inline_call` (not a model, not an intrinsic, not `__prism.*` / `__cxa_*`), up to the translator's inline depth, non-recursive | a new frame; parameters bound to the argument values; `ret` resumes the caller after the call |
| `alloca` | constant size below 2^47 bytes, in the analysed function (not in an inlined callee) | a new live stack object, every byte uninitialised |
| `load iN` / `store iN` | `N ≤ 64`, alignment none or a power of two below 2^32, pointer a register; not an integer load of a whole aggregate | UB through null, a pointer to no object, a freed object, out of bounds, misaligned (address or object), or (store) into a read-only object; reading an uninitialised byte is UB in the strict semantics (PRISM's rule) and *flagged* on the LangRef side, like poison creation; storing `undef`, `poison` or an indeterminate register writes uninitialised bytes |
| `getelementptr [inbounds]` | constant struct field indices, integer (≤ 64-bit) first and array indices | the offset is accumulated in signed 64 bits, each scaling and addition overflow-checked (C17 6.5.6p8); an array index must stay within its array (one past the end for an address that is not dereferenced; also in nested arrays); arithmetic on null, `inbounds` leaving the object or its address range, any other move to another object: UB (strict) / poison (LangRef side) |
| `llvm.smax`/`smin`/`umax`/`umin` | integer, ≤ 64 bits | the signed / unsigned maximum or minimum (`Op::SMax` …) |
| `llvm.abs(x, f)`, `ctlz(x, f)`, `cttz(x, f)`, `ctpop`, `bswap` | integer, ≤ 64 bits (`bswap`: a multiple of 16) | the LangRef values (`ctlz`/`cttz` of 0 is the width); with the flag `f` set, `abs(INT_MIN)` and `ctlz`/`cttz` of 0 are poison (strict: UB); PRISM checks `x == INT_MIN` (INT-SIGNED-OVF) and `x == 0` (INT-CLZ-ZERO) |
| `llvm.expect` | integer | its first operand |
| `{iN, i1} llvm.{s,u}{add,sub,mul}.with.overflow` and `extractvalue` of its fields | the call's two fields are two registers | the wrapped result and the overflow bit (`ovfTest`, proved equal to the LangRef conditions in `Ops.lean`); never poison |
| `llvm.lifetime.start(n, p)` | `1 ≤ n ≤ 8` (the translator refuses larger objects) | PRISM's model: a store of `n` uninitialised bytes through `p`, with a write's checks (so an object whose lifetime ended stays dead: see finding 7) |
| `llvm.lifetime.end(n, p)` | | the object's lifetime ends (`Stmt::Free`, no check) |
| `llvm.memcpy` / `memmove` / `memset` | pointer registers or entry globals; length a register or constant of ≤ 64 bits; the `memset` byte `i8` | with a non-zero length: UB if either access is bad (null, wild, freed, out of bounds, read-only destination; byte alignment) or — `memcpy` — the ranges overlap (C17 7.24.2.1; see finding 6); then the bytes (initialised or not) are copied, reads before writes, or `n` initialised bytes are written; a zero length does nothing |
| a global `@g` named by the analysed function | not thread-local; not the C++ runtime's objects or `stdin`/`stdout`/`stderr`; an initialiser of numbers, strings and zeros | a fresh object allocated at the start of the entry block (which has no predecessors): read-only data (kind 4) and every global of `main` zero-filled then written with the initialiser's non-zero stores; any other mutable global, an external object and a large table: arbitrary initialised bytes drawn from the oracle (`MemTr::global`'s choices) |

**Nondeterminism.** Both semantics take an oracle `ω : Nat → Nat` and read it
in execution order (`World.t`): `freeze undef` / `freeze poison`, a stored
`undef`/`poison` (and `llvm.lifetime.start`) and the uninitialised-local
marker each draw one value, a global of arbitrary contents one per byte, and
the PIR semantics (`XPir.lean`) gives the `t`-th `havoc` the value `ω t`
(reduced to its width). The theorems quantify over every `ω`, the same on
both sides: for every choice of the values LLVM leaves open, PIR making the
same choices has the same outcome. `pir_sound_all_x` quantifies over all
of them.

**Calls.** The LLVM machine is a stack of frames; a block is split at its
calls into segments, and one machine step runs one segment. The translator
mirrors `Tr::inline_call`: each inlined call is an *instance* of the callee
with its own names, shadows and variable range `[lo, hi)`, its blocks
appended to the caller's PIR, its `ret` values flowing into a continuation
phi. The certificate records, per instance, the function, the name and
shadow maps, the variable range, the return target and the PIR block of
every segment; `validB` checks what the proof needs of it (per instance: `lo ≤ hi`,
every name and shadow below `hi`, one entry per block and segment, and per
segment that its PIR block holds exactly the translated instructions, that
the call's callee instance starts at or above the caller's `hi`, and that
terminators and continuation phis point where the machine goes).

**Memory** (`XMem.lean`). The object/offset model of `proofs/semantics`
(`PrismSem/Memory.lean`: objects with a size and a liveness bit, object 0 is
null, allocation takes the next id) extended to what the C++ engine models
(`ConcMem`): each object also has a kind (stack, static, read-only, …) and
a base alignment, each byte is initialised or not, and a pointer is one
64-bit value with the object id in bits 63..48 and the offset in bits
47..0. The LLVM semantics and the PIR semantics (`alloc`, `load`, `store`,
`obj.size/live/kind/align`) share this memory and thread it with the oracle
counter as one `World`.

| Theorem (`PrismRefine.`) | Statement |
|---|---|
| `translateX_exact` | strict LLVM returns `v` ⇒ PIR returns `v`; strict UB ⇒ PIR fails a check; out of fuel ⇒ out of fuel (same oracle, every fuel bound) |
| `strict_lazyX` | the strict semantics refines the LangRef-side semantics (as `strict_lazy`) |
| **`pir_sound_x`** | LangRef-side run has UB, creates poison or reads uninitialised memory within `n` segments ⇒ PIR fails a check within `n` blocks |
| `pir_sound_all_x` | no PIR run fails for any oracle and bound ⇒ no LLVM run is bad for any oracle and bound |
| `pir_faithful_ret_x` / `_fail_x` / `_fuel_x`, `pir_no_stop_x` | every PIR outcome is an LLVM outcome for the same oracle (or LLVM is stuck) |
| `accessChecks_run` | the statements of `MemTr::access_checks` fail exactly when the access is bad (`accessBad`) |
| `gLoop_sim`, `gEnd_sim`, `gFin_run` | the statements of `MemTr::gep` compute the `getelementptr` result and fail exactly where it is undefined |
| `storeVal_sim`, `idxOps_sim` | the stored value and its "initialised" bit; the index operands |
| `unChecks_bad` | the checks inserted for `llvm.abs` / `ctlz` / `cttz` fail exactly when the LangRef result is poison |
| `accChkG_run`, `overlapChk_run` | `MemTr::access_checks` under the guard `n ≠ 0` (byte alignment) and the `memcpy` overlap check fail exactly when `n ≠ 0` and the access is bad / the ranges overlap, for every 64-bit length |
| `memcpy_run`, `memset_run` | the statements of `MemTr::memcpy_` / `memset_` after the operands (the widened length, the guard, the checks, the copy or fill) fail exactly on `cpyBad` and otherwise produce the LLVM memory |
| `store_simX`, `globStores_run` | a store (and `llvm.lifetime.start`); a global's allocation and initialiser stores |
| `sinstX_sim`, `phisX_sim`, `enter_sim`, `step_sim`, `run_simX` | one instruction, the phis, a call, one segment, a run |

What this does **not** say: the memory model is PRISM's, shared by both
sides, so the theorem is "PIR agrees with LLVM *in this memory model*", not a
proof that the model is LLVM's (a flat 48-bit offset per object, at most
2^16 objects, provenance only through the object id). The exporter computes
allocation sizes, field offsets and array lengths with the translator's own
`pirmem::Layout`, and a global's size, alignment, kind and initialiser
stores with the functions the translator uses (`pirmem::entry_global`,
`flat_init`); those computations are trusted, as is the exporter's
rendering of LLVM into the fragment syntax.

**Globals.** `MemTr::global` allocates a global lazily, when an operand first
names it, so its PIR variable used to fall among the temporaries of that
instruction, where no instance owns it. The translator now allocates the
variables of the globals the analysed function names directly
(`pirmem::entry_globals`, the same choices as `MemTr::global`) with the
function's own values, before its results (`Tr::enter_frame`), and
initialises them first thing in the prologue (`Tr::run_frame`,
`MemTr::emit_entry_globals`); the statements are the ones `MemTr::global`
emitted before, only the variable numbering changed. The Lean model treats
each such global as an instruction at the start of the entry block that
allocates and initialises it (`SInst.glob`): the entry block has no
predecessors, so this runs once, before anything reads the global. Globals
named only inside an inlined callee, or through a constant expression, stay
lazy and outside the fragment.

## Property instrumentation: the C++ list, proved

Every check `translate.cpp` inserts in the fragment, the condition it tests
(the C++ `eval_op`, written in Lean in `Pir.lean`), and the lemma proving it
is the LangRef condition. `checks_bad` combines them: the checks inserted for
one instruction fire exactly when the strict semantics has UB there.

| Instruction | C++ check (`Op`, prop, class) | Proved equal to | Lemma |
|---|---|---|---|
| `add nsw` | `SAddOvf`, `ovf+`, INT-SIGNED-OVF | signed sum out of range | `test_sadd` |
| `add nuw` | `UAddOvf`, `wrap+`, UB-POISON | unsigned sum `≥ 2^w` | `test_uadd` |
| `sub nsw` / `nuw` | `SSubOvf` `ovf-` / `USubOvf` `wrap-` | signed difference out of range / `a < b` | `test_ssub`, `test_usub` |
| `mul nsw` / `nuw` | `SMulOvf` `ovf*` / `UMulOvf` `wrap*` | signed / unsigned product out of range | `test_smul`, `test_umul` |
| `udiv`, `urem` | `Eq b 0`, `div0`/`mod0`, INT-DIV-ZERO | divisor 0 | `checks_bad` |
| `udiv exact` | `InexactU`, `exact`, UB-POISON | `a mod b ≠ 0` | `test_inexactU` |
| `sdiv`, `srem` | `Eq b 0` `div0`/`mod0`; `SDivOvf` `divovf` INT-SIGNED-OVF | divisor 0; `INT_MIN / -1` | `checks_bad` |
| `sdiv exact` | `InexactS`, `exact`, UB-POISON | `b ∤ a` (integers) | `test_inexactS` |
| `shl`, `lshr`, `ashr` | `ShiftOob`, `shift`, INT-SHIFT-UB | amount `≥ w` | `checks_bad` |
| `shl` (C signed `<<`) | `ShlSOvf`, `shift-base`, INT-SHIFT-UB | base negative or `a·2^b ≥ 2^(w-1)` | `shlS_core`, `test_shlS` |
| `shl nsw` | `ShlNswOvf` (`(a<<b)>>s b ≠ a`), `shl-nsw` | `a·2^b` out of the signed range | `shlNsw_core`, `test_shlNsw` |
| `shl nuw` | `ShlNuwOvf` (`(a<<b)>>u b ≠ a`), `shl-nuw` | `a·2^b ≥ 2^w` | `shlNuw_core`, `test_shlNuw` |
| `lshr exact` | `LostBitsL` (`(a>>u b)<<b ≠ a`), `exact` | `a mod 2^b ≠ 0` | `lostL_core`, `test_lostL` |
| `ashr exact` | `LostBitsA` (`(a>>s b)<<b ≠ a`), `exact` | `a mod 2^b ≠ 0` | `ashr_shl_eq`, `test_lostA` |
| `or disjoint` | `And` then `Ne 0`, `disjoint`, UB-POISON | common set bit | `checks_bad` |
| `zext nneg` | `Slt a 0`, `nneg`, UB-POISON | operand negative | `nneg_bad` |
| `poison` operand | `check 1`, `poison`, UB-POISON | use of poison | `opnd_sim` |
| `unreachable` | `check 1`, `unreachable`, CXX-UNREACHABLE | reached | `term_sim` |
| use of a register carrying `@__prism.uninit` | `check shadow`, `uninit`, UNINIT-READ | indeterminate value used | `opnd_sim`, `phis_sim` |
| `llvm.abs(x, true)` | `Eq x INT_MIN`, `abs`, INT-SIGNED-OVF | the LangRef poison condition | `unChecks_bad` |
| `llvm.ctlz/cttz(x, true)` | `Eq x 0`, `clz0`, INT-CLZ-ZERO | the LangRef poison condition | `unChecks_bad` |
| `llvm.memcpy/memmove/memset` | `MemTr::access_checks` under `n ≠ 0` (null, wild, uaf, oob, write-const) and, for `memcpy`, `overlap` MEM-OVERLAP | `cpyBad` | `accChkG_run`, `overlapChk_run`, `memcpy_run`, `memset_run` |

Link to the PIR semantics: `checks_eq_ubBin` proves that, on the flags
`PrismSem.Flags` models (`nsw`/`nuw` on `add`/`sub`/`mul`, division and
shift-amount UB), these checks fire exactly when `PrismSem.ubBin` — the UB
definition the PIR instrumentation theorems (`PrismSem.instr_fail_ub_iff`)
use — holds, and `checks_cover_ubBin` that for every flag combination they
cover it. `PrismSem.ubBin` does not model `exact`, `disjoint`, `nneg`,
`shl nsw/nuw` or the C signed-shift rule; the C++ checks do, and those are
proved here against the LangRef conditions instead.

Not covered here (outside the fragment): `llvm.assume` (finding 8), traps,
`__assert_fail` / `reach_error` / `abort`, clang-folded UB markers
(`__prism.folded`, `__prism.poison`). The intrinsic checks above are in the
extended fragment (`agree-ext`), not the unconditional base fragment.

## Coverage of `translate.cpp`

By handler case (the branches of `Tr::operand`, `run_frame`,
`terminator`, `binop`, `inst`, `call`, `resolve_phis`): **31 of 57 cases
(54 %)** are in the proved fragment — all 13 integer binary operators with
all their flags, `icmp`, `select`, the three casts, `phi`, all
terminators, constant/register/`poison` operands, and the
uninitialised-local instrumentation (marker, shadow phis, read check). By
inserted-check site: **21 of 29 (72 %)**. Not covered by the base fragment: calls (inlining and
the 24 other intrinsic / library handlers), `undef` operands and phi inputs,
`freeze`, `extractvalue` / `*.with.overflow`.

With the extended fragment, also in the proved fragment: `freeze`,
`undef` under `freeze` or `store`, `Tr::inline_call` (argument binding,
callee instances, the continuation phi of an inlined `ret`), and from
`translate_mem.cpp` `alloca`, `load`, `store`, `getelementptr` with
`MemTr::access_checks` (null, wild, freed, bounds, alignment, read-only),
`MemTr::gep` (overflow, C array bounds, `inbounds`, leaving the object) and
the uninitialised-memory read check; from `Tr::call` the handlers of
`llvm.smax`/`smin`/`umax`/`umin`, `abs`, `ctlz`/`cttz`, `ctpop`/`bswap`,
`expect`, the overflow intrinsics with `extractvalue`, `lifetime.start`/`end`
and `memcpy`/`memmove`/`memset` (`MemTr::memcpy_`/`memset_` with their
guarded access checks and the overlap check), and `MemTr::global` for the
globals the analysed function names. Not covered: `llvm.assume`, traps,
`stacksave`/`stackrestore`, the floating-point intrinsics, every library
model.

By function, on the repository's own C/C++ corpus
(`python tools/pir_lean_check.py tests/pir testdata --bin <build>/prism`:
the real pir stage with its uninitialised-local and folded-UB
instrumentation and C signed-shift locations), before and after the
extended fragment:

| Input | Functions | `agree` | `agree-ext` | `agree-reject` | `outside` | Mismatch |
|---|---|---|---|---|---|---|
| `tests/pir`, before | 226 | 46 | — | 0 | 180 | **0** |
| `tests/pir`, after | 226 | 46 | **8** | 1 | 171 | **0** |
| `tests/pir`, after merging the latest mainline (2 more functions) | 228 | 46 | **9** | 1 | 172 | **0** |
| `testdata`, before | 2 506 | 915 | — | 2 | 1 589 | **0** |
| `testdata`, after | 2 506 | 915 | **21** | 4 | 1 566 | **0** |
| `testdata`, after the merge (one file not exported in that run) | 2 502 | 914 | **21** | 4 | 1 563 | **0** |
| `tests/pir`, before the intrinsics and globals (this commit's base) | 228 | 46 | 9 | 1 | 172 | **0** |
| `tests/pir`, with intrinsics, `memcpy`/`memset`, lifetime markers, globals | 228 | 46 | **29** | 1 | 152 | **0** |
| `testdata`, before the intrinsics and globals | 2 506 | 915 | 21 | 4 | 1 566 | **0** |
| `testdata`, with intrinsics, `memcpy`/`memset`, lifetime markers, globals | 2 506 | 915 | **32** | 4 | 1 555 | **0** |

The new `agree-ext` functions: array initialisers (`memcpy` from a
`@__const` global, `memset` of a zeroed array), `memcpy`/`memmove`/`memset`
with correct and incorrect lengths and an overlapping copy, tables and
string constants read by index, `llvm.abs`, `llvm.ctlz` with and without
the zero guard, `__builtin_add_overflow`-style checks, and functions reading
a mutable global. The fixture `fixtures/intrinsics.pirl` (22 functions, one
per intrinsic kind, and the IR of the doctest "pir: lean export of
intrinsics, …") checks as 1 `agree` + 21 `agree-ext`; `dropped_intr_check`
removes the overlap, INT-CLZ-ZERO, MEM-UAF and signed-overflow checks from
four of them and must be (and is) 4 mismatches.

`agree-ext` in `tests/pir`: two inlined calls, a C++ template call and a
`constexpr` call, and four stack-memory functions (a struct on the stack,
an uninitialised stack read and its fixed twin, a pointer to a local pair);
in `testdata`: three inlined calls and 18 stack-memory functions (designated
initialisers, packed structs, one-sided index checks, an out-of-bounds
write, a lambda capture, object slicing). The new `agree-reject`s are
recursive calls, which both translators refuse. What keeps the other
`testdata` functions outside (before the intrinsics and globals): a call to
a library function or an intrinsic (1 340), a pointer-typed parameter,
return, phi, select or comparison (209), a global (6), floating point (5),
`volatile` (3), `undef` outside `freeze` (2). After: a call to a library
function (1 330; the exporter now names the callee: libc and C++ runtime
models, `__assert_fail`, nondet sources; one `llvm.trap`), pointers (209),
floating point (5), `volatile` stores (3), a global named only through a
constant expression or in a callee (3), `undef` (2), `alloca` in an inlined
callee (2). The intrinsics themselves were rarely the only obstacle: most
functions that call them also call a library function or take a pointer.

The pir stage has a time budget; on a loaded machine a run can leave a
few files unexported (two `testdata` files in one of the runs above), so
the counts are per exported file; the mismatch count was 0 in every run.

## Per-run correspondence checking (8.2, 2.4)

`src/prism/pir/export_lean.cpp`: when `PRISM_PIR_LEAN_EXPORT` is set (`1`
for `<out>/pir-lean/`, or a directory), the pir stage appends, per
function, the LLVM function in fragment syntax and the PIR `translate()`
produced (or its UNENCODED reason) to `<unit>.pirl`. Anything outside the
fragment is written as `L unsupported <why>`.

`pir_lean_check FILE.pirl…` (proofs/refinement) re-translates the LLVM side
with the proved translator and demands the C++ PIR be *exactly* its output
(variables and widths, blocks, phis, statements, operators, argument
widths, check names and classes, terminators). Verdicts: `agree` (the
theorems hold of the PIR PRISM verified), `agree-ext` (the function is in the
extended fragment, its PIR is exactly `translateX`'s and the certificate
checks: the `_x` theorems hold of it), `agree-reject`, `outside`,
`MISMATCH` (exit 1). `tools/pir_lean_check.py [TREE…] --bin build/prism`
drives it end to end; `tests/test_pir_refinement.py` runs it when
`PRISM_BIN` is set and locks the check-name / operator-name parity between
`translate.cpp` and the Lean files without Lean. `check.sh` runs the
checker on `fixtures/` (real C++ output for `tests/pir`, plus a hand-made
dropped-check pair that must be reported as a mismatch; for the extended
fragment `calls_freeze.pirl` and `memory.pirl`, and `dropped_call_check`,
`dropped_mem_check` with an inlined callee's check, an uninitialised-memory
check and a bounds check removed).

## Testing the formal semantics against `lli` (8.3)

`tools/llvm_sem_vs_lli.py` runs every fragment function of the given C
files on edge-case and random inputs with `llvm_eval` (LangRef, strict and
PIR semantics), executes each input the LangRef semantics says is defined
with `lli` on the same IR, and compares the printed result. It also checks
the three Lean semantics agree as proved. On `tests/pir` through the `prism` binary: 417 runs, 378
compared with `lli`, **0 disagreements** (39 inputs have UB or poison and
are not executed). On `testdata/*.c` (6 inputs per function): 3 603
compared, **0 disagreements**; 1 602 inputs belong to modules `lli` cannot
run (unresolved externals elsewhere in the module) and are counted, not
compared.

## Floating point (8.2)

`Float.lean` models IEEE 754 binary formats (`binary32 = ⟨24, 8⟩`,
`binary64 = ⟨53, 11⟩`, `binary16 = ⟨11, 5⟩`; any precision `p ≥ 2` and
exponent width `ew ≥ 2`) at the bit level (sign, biased exponent, fraction),
with `decode` per IEEE 754-2019 §3.4. Every finite datum is an integer
multiple of the smallest subnormal `2^-D` (`Fmt.D`; binary32: 149), so a
finite magnitude is a natural number in that unit. Sums are exact integers;
products (`nx·ny / 2^D`) and quotients (`nx·2^D / ny`) are rationals `a / b`,
rounded by `roundQ`/`roundF`, with nearness proved on distances scaled by
`b` (`dist (r·b) a = b·|r − a/b|`). Round to nearest, ties to even
throughout.

| Theorem (`PrismRefine.Float.`) | Statement |
|---|---|
| `roundU_repr`, `roundU_nearest`, `roundU_tie_even` | round-to-nearest-even of an integer with unbounded exponent returns a `p`-bit value at least as close as every `p`-bit value; ties give an even significand |
| `roundQ_repr`, `roundQ_nearest`, `roundQ_tie_even` | the same for a rational `a / b` (`roundQ_one`: with `b = 1` it is `roundU`) |
| `round_correct`, `roundF_correct` | in the format: the nearest finite datum when it does not overflow; overflow (to infinity, §7.4) only when the exact magnitude exceeds the largest finite one |
| `decode_encode` | every representable magnitude is encoded exactly |
| `add_correct`, `sub_correct` | bit-level `add` / `sub` of two finite data decode to the correctly rounded exact sum / difference, with IEEE's sign of an exact zero and overflow to infinity |
| `mul_correct`, `div_correct` | bit-level `mul` / `div` (non-zero divisor) of finite data decode to the correctly rounded exact product / quotient with sign `sx xor sy`: a signed zero on underflow to zero, infinity on overflow |

`add`, `sub` (`x + (−y)`), `mul` and `div` also define every special case as
IEEE 754 §6–7: NaN operands give the default quiet NaN, `∞ − ∞`, `0 × ∞`,
`0 / 0` and `∞ / ∞` are NaN (invalid), a finite non-zero `x / ±0` is an exact
signed infinity, `∞ / y` is infinite, `x / ∞` is a signed zero.

**PRISM's floating-point checks.** `FloatOps.lean` mirrors, operator by
operator, the conditions `FpTr::checks` (`src/prism/pir/translate_fp.cpp`,
`--fp-checks`) builds for `fadd`/`fsub`/`fmul`/`fdiv`, and the FLOAT-CAST-OVF
condition the encoder builds for `fptosi`/`fptoui` (`Op::FToSIOvf` /
`Op::FToUIOvf` in `src/prism/pir/encode.cpp`; the interpreter in `fp.cpp`
computes the same). `tests/test_proofs_float_conc.py` locks every C++
expression to its Lean definition: changing either side fails the test. The
IEEE side (`ieeeOverflow`, `ieeeInvalid`, `ieeeDivByZero`, `inRange`) is
written from IEEE 754-2019 §7.2–7.4 and C11 6.3.1.4, not from the code.

| Theorem | Statement |
|---|---|
| `prism_overflow_eq` | FLOAT-OVERFLOW (`isInf(r) ∧ both operands finite`, and `¬isZero(y)` for `fdiv`) **equals** IEEE overflow, for every operation and every pair of operands, special values included |
| `prism_invalid_eq` | FLOAT-INVALID (`isNaN(r) ∧ no NaN operand`) **equals** IEEE invalid operation on quiet operands (`∞ − ∞`, `0 × ∞`, `0 / 0`, `∞ / ∞`) |
| `ieee_invalid_eq` | IEEE invalid = FLOAT-INVALID **or** a signalling-NaN operand. PRISM does not report the signalling-NaN case: Z3's FP theory has one NaN and LLVM's default floating-point environment does not preserve signalling NaNs |
| `prism_divzero_eq` | FLOAT-DIV-ZERO (`isZero(y) ∧ ¬isNaN(x)`) = IEEE divide-by-zero **or** `0 / 0` **or** `±∞ / 0`. It is wider than IEEE: `0 / 0` is IEEE invalid (PRISM also reports it as FLOAT-INVALID), and `∞ / 0` raises no IEEE exception (the result is an exact infinity). `prism_divzero_extra` shows both extra cases are real |
| `ieee_divzero_imp_prism` | every IEEE divide-by-zero is reported |
| `cast_ovf_iff` | for any format, any `k`, signed or unsigned, and any well-formed datum: FLOAT-CAST-OVF (`isNaN(x) ∨ isInf(x) ∨ t < lo ∨ t ≥ hi`, `t` = `x` truncated toward zero, `lo = −2^(k−1)` or `+0`, `hi = 2^(k−1)` or `2^k` as numerals of the format) holds **iff** the value is outside the range of `iK` (C11 6.3.1.4). This includes formats where the bound is not representable (binary16 and `k ≥ 17`: `hi` rounds to `+∞`, and only NaN and infinities are out of range) |

Trusted, not proved: that Z3's floating-point theory (`Z3_mk_fpa_*`)
implements IEEE 754, and that `Z3_mk_fpa_numeral_double` rounds a numeral to
nearest even (`pow2Val` models `fnum` that way). The model's `k` is a natural
number; LLVM has no `i0`.

Not covered: the other rounding modes; `frem`, `sqrt`, `fma`/`fmuladd` and
libm (they get the same `--fp-checks` conditions, but their IEEE semantics is
not modelled here); NaN payloads; the underflow and inexact flags (PRISM
does not check them); `fptrunc` (see below); and the connection between these
Lean definitions and the Z3 terms beyond the text-level lock of the test.

A finding from this work: `fptrunc` called `checks(cur, Op::FConv, w, {}, r,
c.line)` with no operands, and `checks` returns early when no operand is a
floating-point value of width `w`. So FLOAT-OVERFLOW was never reported for a
`double`→`float` truncation that overflows, even with `--fp-checks`. This is
fixed: `fptrunc` now checks "finite operand, infinite result" directly
(doctest "pir3 fp: --fp-checks reports a double->float narrowing that
overflows"). The Lean overflow theorem covers `fadd`/`fsub`/`fmul`/`fdiv`;
the `fptrunc` condition is the same shape, but it is not yet a Lean theorem.

## Independent proof rechecking (8.5)

`proofs/refinement/recheck.sh PROJECT ROOT…` runs, for one Lean project:

1. `lake build`;
2. a source scan (no `sorry`, `native_decide`, `bv_decide`, top-level
   `axiom` or `unsafe`);
3. `axiom_report`: the axioms of **every** declaration under the root
   modules (not only curated theorems); fails on anything but `propext`,
   `Classical.choice`, `Quot.sound` — so `sorryAx`, `Lean.ofReduceBool`
   and `Lean.trustCompiler` all fail;
4. `leanchecker` (lean4checker, upstreamed into the toolchain from v4.28;
   the separate repository is deprecated and has no v4.34 tag): replays every
   declaration of every `.olean` module into a kernel environment;
5. **nanoda** (independent Rust kernel) type-checks the `lean4export` NDJSON
   export of the roots and all their dependencies from scratch.
   `Lean.trustCompiler` is permitted for the *environment* only (core `Init`
   declares it); step 3 already rejects any PRISM declaration that uses it.

`lean4export` (v4.34.0) and `nanoda` (0.4.19) are pinned in
`third_party/MANIFEST.toml` as external tools and built by
`python scripts/fetch_deps.py --tool lean4export` / `--tool nanoda`
(recipes `lake` and `cargo`). A missing checker is NOTRUN (exit 3), never a
pass. `.github/workflows/proofs-recheck.yml` runs the recheck for
`proofs/`, `proofs/semantics`, `proofs/techniques` and `proofs/refinement`
on every change under `proofs/`, uploads each axiom list, and attaches them
to every published release.

Local run (this machine, 4 cores):

| Project | Declarations audited | Axioms used | leanchecker | nanoda (declarations checked incl. dependencies) |
|---|---|---|---|---|
| `proofs` (`Prism`) | 492 | propext, Classical.choice, Quot.sound | ok | 60 118, no errors |
| `proofs/semantics` (`PrismSem`) | 1 455 | propext, Classical.choice, Quot.sound | ok | 60 989, no errors |
| `proofs/techniques` (`PrismTechniques`) | 746 | propext, Classical.choice, Quot.sound | ok | 172 973, no errors |
| `proofs/refinement` (`PrismRefine`) | 2 254 | propext, Classical.choice, Quot.sound | ok | 62 092, no errors |

## Findings in `translate.cpp`

The proof attempts surfaced five places where the C++ translator is not a
sound over-approximation of the LangRef semantics. The Lean translator
refuses them (the checker reports `outside`, not `agree`), so no theorem
is claimed for them:

1. **Poison incoming to a phi is havocked without a check.** `resolve_phis`
   turns a `poison` (or `undef`) incoming value into an unconstrained
   `havoc` on that edge. If that value then reaches a `br` or a `ret`, LLVM
   has UB but the PIR program takes an arbitrary branch or returns an
   arbitrary value and fails no check. Clang at `-O0` + mem2reg produces
   `undef` phi inputs for uninitialised locals (those are covered by the
   separate uninitialised-read instrumentation), and `poison` phi inputs
   rarely. **Fixed in `translate.cpp`:** a `check 1` (UB-POISON) is now
   emitted on the poison edge, as `Tr::operand` does for a non-phi use
   (doctest "pir: poison flowing into a phi is a checked violation"). The
   Lean translator still refuses poison phi inputs, so such functions stay
   `outside` the proved fragment until it is extended to match.
2. **`icmp samesign` was ignored (fixed).** Newer LLVM (the flag was added
   after LLVM 18) makes `icmp samesign` poison when the operands' signs
   differ; `translate.cpp` dropped the flag, so `icmp samesign ult i32 %a, %b`
   with `a = -1`, `b = 1` was PROVED. It now fails a `UB-POISON` check (prop
   `samesign`) where the poison is created, like `nneg` (finding 3); on
   pointers it is `UNENCODED`. Clang 18 (the pinned front end) does not emit
   it. The Lean model has no such flag: the exporter still refuses it, so
   such functions stay `outside` (doctest "pir: LangRef-defined IR is not
   reported, samesign poison is ...").
3. **Checks at creation over-approximate.** PRISM fails a check where
   poison is *created* (`add nsw` that wraps), even if the value is never
   used. For C this is the C semantics (signed overflow is UB at the
   operation); for optimiser-introduced flags (`nuw`, `exact`, `disjoint`,
   `nneg`) it can report a failure LLVM would not have. `pir_faithful_fail`
   states this precisely: a PIR failure means UB *or poison creation*.

4. **`undef` outside `freeze` is havocked once.** `Tr::operand` turns an
   `undef` operand into one `havoc`. In the LangRef a value computed from
   `undef` is a *set*: every use of it may observe a different member
   (`%a = xor i32 undef, 0` compared with itself may be false), so fixing
   one value under-approximates LLVM's behaviours; and `br i1 undef`,
   `switch undef` and `ret undef` from a `noundef` function are UB, which
   the havoc hides (it takes an arbitrary branch, returns an arbitrary
   value). The Lean model accepts `undef` only under `freeze` and as a
   stored value, where one arbitrary value is the LangRef semantics; the
   exporter prints `undef` and the checker refuses it elsewhere (2 functions
   in `testdata`). **Fixed in `translate.cpp`:** a static may-undef analysis
   gives every value computed from `undef` (through phis, `select`, casts,
   arithmetic, non-inlined calls) a one-bit undef shadow, set per path; each
   non-phi use of such a value reads a fresh havoc (per use site and per
   execution; `freeze` picks one value), a store of it writes uninitialised
   bytes (as does an `undef` global initialiser), and `br` on it, a `noundef`
   return or a `noundef` argument fails a `UB-POISON` check (prop `undef`).
   An undef argument to an inlined callee without `noundef`, or an undef
   return from one, is refused (`UNENCODED`). A division, load or store
   through such a value fails its existing check, since the fresh value
   ranges over everything. Before the fix, 7 of the adversarial IR cases of
   the doctest "pir: undef is a fresh value at every use ..." were wrong
   `PROVED`s (`x - x` of one undef-derived value, of an undef phi, `br` on
   undef and on `icmp undef`, `ret` from a `noundef` function, a `noundef`
   argument, and a stored-then-loaded undef); all are now `FAILED`, and the
   path-sensitive, `freeze` and non-`noundef` return cases stay `PROVED`.
   The Lean side is unchanged (it still accepts `undef` only under
   `freeze`/`store`, whose C++ translation did not change), so functions
   with `undef` elsewhere stay `outside`; `tools/pir_lean_check.py` after
   the fix: `testdata` 915 `agree` + 21 `agree-ext`, `tests/pir` 46 + 9,
   0 mismatches.
5. **`freeze poison` was reported (fixed).** `freeze` translated its operand
   with `Tr::operand`, which fails `UB-POISON` on a literal `poison`; in LLVM
   `freeze poison` is defined (an arbitrary, fixed value). `translate.cpp`
   now translates `freeze poison` exactly as `freeze undef`: one `havoc`, no
   check. The Lean checker reads a literal `poison` operand of `freeze` (and
   the value of a `store`) as `undef` (`Check.lean` `fopnd?`): the LangRef
   defines `freeze poison` as `freeze undef`, and a stored `poison` or `undef`
   both write uninitialised bytes in both semantics (`sStoreVal`,
   `trStoreVal`), so no theorem changed; the fixture function `frz_poison`
   (`calls_freeze.pirl`) is `agree-ext`. Clang does not emit `freeze poison`
   for C.
6. **`llvm.memcpy` with the same source and destination was reported
   (fixed).** The LangRef lets `llvm.memcpy` copy an object onto itself
   exactly (clang emits it for a struct assignment `*p = *q` that may be a
   self-assignment); `MemTr::memcpy_` reported every overlap, `d = s`
   included, which is C's rule for the `memcpy` *function* (C17 7.24.2.1),
   and a C `memcpy` call was lowered to the same intrinsic. The pir stage now
   compiles with `-fno-builtin-memcpy`, so a C `memcpy` call stays a call and
   reaches its library model (`__prism_memcpy`, C's rule: any overlap,
   `d = s` included, is `MEM-OVERLAP`), while the intrinsic follows the
   LangRef: its overlap check also requires the offsets to differ
   (`tests/pir/mem_str.c` `struct_self_assign_ok` PROVED, was a false
   `MEM-OVERLAP`; `memcpy_self_bad` still FAILED). `__builtin_memcpy` in C is
   still the intrinsic, so `__builtin_memcpy(p, p, n)` is no longer reported.
   The Lean side changed with it: `cpyBad` (`XLlvm.lean`) excludes
   `ptrOff d = ptrOff s` in one object, `overlapChk` (`XTranslate.lean`) has
   the two extra statements the C++ emits, and `overlapChk_run` / `cpyBad_eq`
   (`XMemOps.lean`) are re-proved; no `sorry`, same three axioms.
7. **`llvm.lifetime.start` on an object whose lifetime ended was reported
   (fixed in the C++ translator).** In the LangRef it begins a new lifetime;
   PRISM stored uninitialised bytes with a write's checks, so the object
   stayed dead and MEM-UAF failed (and objects larger than 8 bytes were
   UNENCODED). A new PIR statement `revive` (`Stmt::Revive`) makes a *stack*
   object live again (other objects keep their liveness, as the LangRef says
   for non-stack objects), then the `size` bytes (`-1`: to the end of the
   object) are copied from a fresh uninitialised object, with a write's
   checks; any size is encoded. The loop invariant and k-induction paths
   treat `revive` like `free`. The Lean PIR has no such statement, so the
   Lean translator now refuses `llvm.lifetime.start` (`outside fragment`):
   functions with it left the extended fragment (in `fixtures/intrinsics.pirl`
   `life` and `life_bad`, now `agree-reject`). The pir stage compiles at
   `-O0`, where clang emits no lifetime markers, so this concerns coroutine
   code and IR given directly. Still open: `llvm.lifetime.end` on a pointer
   into a non-stack object (a coroutine frame) ends that whole object, where
   the LangRef only makes its bytes poison.
8. **A false `llvm.assume` was not reported (fixed).** The LangRef makes
   `llvm.assume(false)` undefined behaviour, and so is a false
   `__builtin_assume` in C. Clang emits `llvm.assume` for `__builtin_assume`
   even at `-O0`; only `[[assume(...)]]` is dropped there
   (`testdata/cxx_assume.cpp` has no call). `Tr::call` used to turn it
   straight into a PIR `assume`, which discards the path, so
   `f(int x) { __builtin_assume(x > 0); return 100 / x; }` was PROVED
   although `f(0)` is undefined. It is now a `FUNC-CONTRACT` check of the
   condition (prop `assume`), then the `assume`.
   While fixing it, a wider bug in the encoder showed up, soundness bug S9 in
   `docs/CONFORMANCE.md`: every `assume` was a global axiom of its node, so
   it also constrained checks *before* it in the same block. A division by a
   nondet value followed by `__VERIFIER_assume(x != 0)` was PROVED. An
   `assume` now strengthens only the guard of the statements after it and the
   node's outgoing edges (`exit_reach` in `encode.cpp`). The Lean translator
   still refuses `llvm.assume`, so no theorem covers it.

In the Lean model, `Stmt::Alloc` with `init = 2` (arbitrary initialised
bytes, `ConcMem`) was read as uninitialised memory; it is now arbitrary
bytes drawn from the oracle (`World.allocW`), as the C++ engine models it.
It was never in a proved function before (only mutable globals use it).

A value difference that is not a soundness issue: after a failed division
check the C++ interpreter uses Z3's `bvudiv x 0 = ~0`, the Lean model Lean's
`x / 0 = 0`; the value is unobservable because the check already failed.

## What remains

* **Library calls.** Most `testdata` functions outside the fragment call a
  function PRISM models rather than inlines (1 329 of 1 560): libc and C++
  runtime models, `__assert_fail`, `exit`, nondet sources, traps, the
  `__prism.folded` / `__prism.poison` markers; also `llvm.assume`
  (finding 8) and the intrinsics `translate.cpp` refuses (`fshl`/`fshr`, …:
  both translators refuse them). Each model needs its semantics stated in
  Lean; the libc models also take pointer parameters (next item).
* **More memory.** In the proved fragment: stack objects of constant size,
  integer loads and stores, `getelementptr`, `llvm.memcpy`/`memmove`/
  `memset`, `llvm.lifetime.*`, globals the analysed function names. Not yet:
  the heap (`malloc`/`free`, `new`/`delete`), pointer-typed parameters (the
  harness objects of `Tr::enter_frame`), pointers stored in memory and
  pointer phis, selects and comparisons (the largest group after library
  calls: 209 `testdata` functions), globals whose initialiser holds
  pointers, globals named only in an inlined callee or through a constant
  expression, variable-size `alloca`, aggregate loads (raw byte copies with
  per-byte shadows), `llvm.stacksave`/`stackrestore`.
* **Nondeterminism beyond `freeze`.** `undef` elsewhere (finding 4, fixed
  in the C++ translator by per-use fresh values) needs set-valued registers
  in the Lean model; the oracle semantics covers exactly the places where
  LLVM makes one arbitrary choice.
* **Certificates, not a translator theorem.** The extended theorems hold for
  every function whose certificate `validB` accepts, which the checker
  evaluates at each run; there is no theorem that `translateX` always
  produces a valid certificate. Such a theorem is a proof about the stateful
  mirror itself (`enterFrame`/`runFrame`: `for` loops in `StateT` over
  `Except`, arrays of blocks and instances updated in place, variables
  numbered as they are pushed), not about the translation, and was not
  attempted here; measured instead: every function whose Lean and C++
  translations agree also passed `validB` (0 certificate failures on
  `tests/pir` and `testdata`). `validB` was shrunk where the proof never
  used a check: the lengths of the per-block first-temporary and
  child-instance lists (`segOK` already fails when an entry it reads is
  missing).
* **The PIR semantics used here is the C++ CFG form.** It is linked to
  `PrismSem` through `evalBin`/`evalPred`/`ubBin` (`checks_eq_ubBin`), not
  through a proof that the CFG and `PrismSem.Stmt` programs are equivalent.
* **The C++ encoder.** Relating `pir_vcs` (Z3) to this PIR semantics is the
  encoder row of 8.2 (proofs/semantics proves a model of it).
* **Modelling what the C++ fixes added**: `icmp samesign` (finding 2) and the
  `revive` statement of `llvm.lifetime.start` (finding 7) in the Lean model,
  so those functions come back into the fragment; `undef` outside
  `freeze`/`store` (finding 4); `llvm.assume` (finding 8).
