# LLVM IR to PIR: refinement proof, correspondence checking, independent rechecking

This document covers roadmap Part 8.2 rows "LLVM IR to PIR translation"
and "Property instrumentation" (for the C++ instrumentation list itself),
the "Floating point" row (a first proved piece), the Part 8.3 mitigation
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
* Each run can check that the C++ translator's output **is** the proved
  translator's output: the pir stage exports (LLVM, PIR) pairs, and
  `pir_lean_check` compares them. On `tests/pir` and `testdata` there are no
  disagreements (numbers below).
* Every Lean project in `proofs/` is rechecked in CI by `leanchecker` (the
  toolchain's replay checker, formerly lean4checker) and by **nanoda**, an
  independent Lean kernel written in Rust, and every declaration's axioms
  are audited.

What is *not* proved, and the three gaps this work found in
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
| Size | about 4 000 lines of Lean, 44 audited theorems, 2 254 declarations (all axiom-audited) |

| File | Contents |
|---|---|
| `PrismRefine/Llvm.lean` | LLVM fragment syntax; LangRef semantics `lRun` (lazy poison); strict semantics `sRun` |
| `PrismRefine/Pir.lean` | PIR exactly as `include/prism/pir.hpp` represents it (CFG, phis, `check`/`assume`, var table) and the semantics of `prism::pir::interpret` |
| `PrismRefine/Translate.lean` | The translator, mirroring `translate.cpp` |
| `PrismRefine/Ops.lean` | The C++ test operators (`sadd.ovf`, `shl.nsw.ovf`, …) equal the LangRef conditions |
| `PrismRefine/Refine.lean` | Simulation: `translate_exact` |
| `PrismRefine/Lazy.lean` | Strict vs LangRef semantics: `strict_lazy` |
| `PrismRefine/Sound.lean` | Headline theorems; link to `PrismSem.ubBin` |
| `PrismRefine/Check.lean`, `CheckMain.lean` | `pir_lean_check`: the correspondence checker |
| `EvalMain.lean` | `llvm_eval`: runs the three semantics on concrete inputs (for `tools/llvm_sem_vs_lli.py`) |
| `AxiomReport.lean` | `axiom_report`: axioms of every declaration of a project (8.5) |
| `PrismRefine/Float.lean` | IEEE 754 binary formats, correctly rounded addition |
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
this fragment: there are no interaction trees and no memory, and
`undef`/`freeze` (nondeterminism) are outside the fragment.

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

Link to the PIR semantics: `checks_eq_ubBin` proves that, on the flags
`PrismSem.Flags` models (`nsw`/`nuw` on `add`/`sub`/`mul`, division and
shift-amount UB), these checks fire exactly when `PrismSem.ubBin` — the UB
definition the PIR instrumentation theorems (`PrismSem.instr_fail_ub_iff`)
use — holds, and `checks_cover_ubBin` that for every flag combination they
cover it. `PrismSem.ubBin` does not model `exact`, `disjoint`, `nneg`,
`shl nsw/nuw` or the C signed-shift rule; the C++ checks do, and those are
proved here against the LangRef conditions instead.

Not covered here (outside the fragment): `llvm.abs(x, true)`,
`llvm.ctlz/cttz(x, true)`, traps, `__assert_fail` / `reach_error` /
`abort`, clang-folded UB markers (`__prism.folded`, `__prism.poison`).

## Coverage of `translate.cpp`

By handler case (the branches of `Tr::operand`, `run_frame`,
`terminator`, `binop`, `inst`, `call`, `resolve_phis`): **31 of 57 cases
(54 %)** are in the proved fragment — all 13 integer binary operators with
all their flags, `icmp`, `select`, the three casts, `phi`, all
terminators, constant/register/`poison` operands, and the
uninitialised-local instrumentation (marker, shadow phis, read check). By
inserted-check site: **21 of 29 (72 %)**. Not covered: calls (inlining and
the 24 other intrinsic / library handlers), `undef` operands and phi inputs,
`freeze`, `extractvalue` / `*.with.overflow`.

By function, on the repository's own C/C++ corpus (translator output
compared with `pir_lean_check`):

| Input | Functions | C++ encodes | In the proved fragment, identical to the Lean translation | Both refuse | Outside the fragment | Mismatch |
|---|---|---|---|---|---|---|
| `tests/pir`, stage pipeline incl. uninit markers | 46 | 40 | **38** | 0 | 8 | **0** |
| `testdata`, plain clang + opt | 2 716 | 916 | **899** | 2 | 1 815 | **0** |
| `testdata` (C files that compile standalone), incl. uninit markers | 1 293 | 214 | **201** | 2 | 1 090 | **0** |

(Measured by calling `translate()` on IR produced by the pir stage's clang +
opt pipeline, with the stage's uninitialised-local markers reproduced where
stated; `tools/pir_lean_check.py` runs the same comparison through the
`prism` binary.) Of the functions the C++ translator encodes, 94–98 % are in
the proved fragment; the rest contain calls. Outside-fragment reasons in
`testdata`: calls, pointer types, `alloca`, `undef`, floats, memory.

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
theorems hold of the PIR PRISM verified), `agree-reject`, `outside`,
`MISMATCH` (exit 1). `tools/pir_lean_check.py [TREE…] --bin build/prism`
drives it end to end; `tests/test_pir_refinement.py` runs it when
`PRISM_BIN` is set and locks the check-name / operator-name parity between
`translate.cpp` and the Lean files without Lean. `check.sh` runs the
checker on `fixtures/` (real C++ output for `tests/pir`, plus a hand-made
dropped-check pair that must be reported as a mismatch).

## Testing the formal semantics against `lli` (8.3)

`tools/llvm_sem_vs_lli.py` runs every fragment function of the given C
files on edge-case and random inputs with `llvm_eval` (LangRef, strict and
PIR semantics), executes each input the LangRef semantics says is defined
with `lli` on the same IR, and compares the printed result. It also checks
the three Lean semantics agree as proved. On `tests/pir`: 430 runs, 395
compared with `lli`, **0 disagreements** (35 inputs have UB or poison and
are not executed). On `testdata/*.c` (6 inputs per function): 3 603
compared, **0 disagreements**; 1 602 inputs belong to modules `lli` cannot
run (unresolved externals elsewhere in the module) and are counted, not
compared.

## Floating point (8.2, first piece)

`Float.lean` models IEEE 754 binary formats (`binary32 = ⟨24, 8⟩`,
`binary64 = ⟨53, 11⟩`) at the bit level (sign, biased exponent, fraction),
with `decode` per IEEE 754-2019 §3.4. Every finite datum is an integer
multiple of the smallest subnormal, so exact rational arithmetic on finite
data is exact integer arithmetic in that unit. Proved for every format:

| Theorem (`PrismRefine.Float.`) | Statement |
|---|---|
| `roundU_repr`, `roundU_nearest`, `roundU_tie_even` | round-to-nearest-even with unbounded exponent returns a `p`-bit value at least as close as every `p`-bit value; ties give an even significand |
| `round_correct` | in the format: the result is the nearest finite datum when it does not overflow; overflow (to infinity, §7.4) only when the exact magnitude exceeds the largest finite one |
| `decode_encode` | every representable magnitude is encoded exactly |
| `add_correct` | bit-level `add` of two finite data decodes to the correctly rounded exact sum, with IEEE's sign of zero and overflow to infinity |

Not covered: other rounding modes, NaN propagation, operations other than
addition/subtraction, and any link to an SMT floating-point encoding —
PRISM does not encode floating point yet (`translate.cpp` refuses `fadd`
etc. as UNENCODED).

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

The proof attempt surfaced three places where the C++ translator is not a
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
   rarely; a fix is to add `check 1` (UB-POISON) on the poison edge, as
   `Tr::operand` does for a non-phi use.
2. **`icmp samesign` is ignored.** Newer LLVM (the flag was added after
   LLVM 18) makes `icmp samesign` poison when the operands' signs differ;
   `translate.cpp` drops the flag. Clang 18 (the pinned front end) does not
   emit it; the exporter refuses it so the checker cannot claim a proof.
3. **Checks at creation over-approximate.** PRISM fails a check where
   poison is *created* (`add nsw` that wraps), even if the value is never
   used. For C this is the C semantics (signed overflow is UB at the
   operation); for optimiser-introduced flags (`nuw`, `exact`, `disjoint`,
   `nneg`) it can report a failure LLVM would not have. `pir_faithful_fail`
   states this precisely: a PIR failure means UB *or poison creation*.

A value difference that is not a soundness issue: after a failed division
check the C++ interpreter uses Z3's `bvudiv x 0 = ~0`, the Lean model Lean's
`x / 0 = 0`; the value is unobservable because the check already failed.

## What remains

* **Calls.** 1 370 of the outside-fragment functions in `testdata` are
  outside because of calls: inlining (`Tr::inline_call`, including the
  continuation phi of an inlined `ret`) and the 25 intrinsic and library
  handlers (overflow intrinsics with `extractvalue`, `smax`… `bswap`,
  `abs`, `ctlz`/`cttz`, `assume`, traps, `__assert_fail`, `exit`, nondet
  sources, PRISM's `__prism.folded` / `__prism.poison` markers). Each is a small extension of
  `Translate.lean` and of `checks_bad`; inlining needs a frame-stack
  simulation.
* **Nondeterminism.** `undef` operands and `freeze` need the LLVM semantics
  to be a relation (or oracle-parameterised) and PIR `havoc` to be
  nondeterministic; the refinement statement then becomes "every PIR
  behaviour is an LLVM behaviour" over sets of behaviours.
* **Memory.** `alloca`/`load`/`store`/`getelementptr` are UNENCODED in the
  C++ translator today; the memory model of `proofs/semantics` is the
  target.
* **The PIR semantics used here is the C++ CFG form.** It is linked to
  `PrismSem` through `evalBin`/`evalPred`/`ubBin` (`checks_eq_ubBin`), not
  through a proof that the CFG and `PrismSem.Stmt` programs are equivalent.
* **The C++ encoder.** Relating `pir_vcs` (Z3) to this PIR semantics is the
  encoder row of 8.2 (proofs/semantics proves a model of it).
* **Fixing the three findings** in `translate.cpp` (not done here: the
  translator is outside this work's scope).
