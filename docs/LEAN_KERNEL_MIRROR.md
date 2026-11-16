# Lean mirror for the kernel-mode constructs (W6, phase 1 design)

Status: design, phase 1. Base: origin/integration-wave1 (main plus PRs #47-#50, not built as a whole).
Scope: every construct the integration branch adds in kernel mode (`--kernel-api`, `--pir-harness`,
kernel specs), what the exporter writes for it today, and the Lean extension it needs.

## 1. Baseline and the trust rule

* The proved fragment is `PrismRefine` (`proofs/refinement`, 96 audited theorems, `check.sh`).
  Its base theorems (`pir_sound`, `pir_faithful_*`) cover LLVM functions in the base fragment;
  the extended fragment (`XLlvm`, `XPir`, `XTranslate`, `XValid`) adds freeze, calls, stack memory,
  libc-model heap, pointers, certificates checked per function (`agree-ext`).
* A construct is **inside** the fragment when its LLVM semantics and its translator encoding are both
  defined in Lean and the translator mirror emits the same PIR (checked by `pir_lean_check`).
* A construct is **outside** when the exporter writes `L unsupported <why>` for the function, so it gets
  no Lean verdict. Nothing outside is claimed proved.
* A construct is **axiomatised** only as a named hypothesis. Preferred form: a Prop argument to the
  theorem (`(hSpec : KSpec_linux_kmalloc M)`), so the statement itself says what is assumed.
  Top-level `axiom` is banned by `check.sh` (the scan of `--toplevel axiom`), so no new `axiom`
  declaration is added unless the scan is extended by an explicit, reviewed allowlist entry, one line
  per axiom, that `Audit.lean` cross-checks against `axioms.txt`.

## 2. What the branch exports today

Taken from `git diff origin/main..origin/integration-wave1 -- src/prism/pir` (25 files, +2221/-53).

| Construct (source) | Translator encoding today | Lean export today |
|---|---|---|
| inline asm, outputs (`translate_kernel.inc` `asm_havoc`) | outputs havocked; memory clobber only on pointer operands; other memory "assumed unchanged" | `L unsupported kernel-mode model: asm-havoc ...` (export_lean.cpp rejects every `kernel_note`) |
| asm goto (`callbr`, `translate.cpp`) | asm havoc, then nondeterministic jump to one of its labels; one label only in PR #49 path, several labels UNENCODED | unsupported (`callbr` is not in the LLVM fragment) |
| atomics and barriers (`atomics-sequential`) | sequential plain instruction; `UNENCODED` without `--kernel-api` | unsupported (kernel_note) |
| volatile loads/stores, MMIO (`volatile-access`) | ordinary read/write, named assumption | unsupported (kernel_note) |
| kernel API specs (`models/kernel/*.cpp`, 150 specs in PR #49; `fpt.call` / `llvm.*` path) | model statements inserted for the callee, with its assumption | unsupported (kernel_note; the callee is not an LLVM body) |
| automatic pointer harness (`harness.cpp`, PR #50, `--pir-harness`) | object graph from IR types, `alloc_harness_node` | `throw Unsupported{"automatic pointer harness ..."}`; the parameter is written as `L ptrparam` only when the harness is off |
| unknown calls (`--assume-unknown-calls`, `unknown_call`) | result and pointee tails havocked, call assumed to return | unsupported unless the call has a spec |
| `ptrtoint` / `inttoptr` (W2, not yet on the branch) | UNENCODED | unsupported |
| bitfield load/store (W2, not yet on the branch) | shift/mask over integers | in principle inside the fragment once emitted as base ops |
| havoc-all-memory (W2, not yet on the branch) | none | none |
| atomics with real C11/LKMM order (W3, not yet) | none | none |
| asm instruction semantics (W4, not yet) | none | none |
| ops-table indirect calls (W5, not yet) | none | none |
| concurrency contexts (W7, not yet) | none | none |

## 3. Per construct: Lean extension, inside or axiom, proof obligation

### 3.1 Inside the fragment (Lean semantics plus translator mirror)

**a. asm havoc (outputs only).** LLVM side: an `asm` call returns values chosen by the environment.
Lean: an oracle `havoc` of width `w` (already in `XPir`, the oracle counter of `XMem`). Obligation: the
PIR havoc of each output variable over-approximates the set of LLVM results (trivially the full range).
Extension needed: an `asm` node in `XLlvm` with its output list, and the mirror of `asm_havoc`
statement by statement. Scope: outputs only; the memory part is 3.2.

**b. asm goto / callbr.** Lean: callbr is a `br` whose successor is an oracle choice among the listed
labels (the fall-through included), after the asm havoc of 3.1a. Obligation: the PIR choice set equals
the LLVM successor set. Extension: `callbr` in `XLlvm` with a label list, mirror of `callbr` in
`translate.cpp`. Several labels are needed for W2, so the proof is stated for n labels from the start.

**c. Volatile accesses (`volatile-access`).** Lean: a volatile load or store is an ordinary access in
the strict semantics (no reorder, no removal is already a fact of the sequential fragment). The read
value of MMIO is not a memory value; that part is an environment fact and is written as hypothesis
`VolatileIsOracle` only where the volatile location is MMIO (the exporter knows only the flag, so the
hypothesis lands in the theorem, not in the fragment). Obligation: volatile access is not elided
(translator emits it in place). Extension: a `volatile` bit on `load`/`store` in `XMem`. Phase 2, first.

**d. havoc-all-memory (W2).** Lean: a PIR statement that havocs every object of the `World` memory (XMem),
oracle-driven. Obligation: the PIR statement covers every object the LLVM asm could write, which is all
objects (the strongest form). This removes the "other memory assumed unchanged" gap in 3.1a. This is
the first construct that makes asm sound in the proof rather than in the assumption list.

**e. ptrtoint / inttoptr, error pointers (W2).** Phase 2, after the others. Lean: pointers are
`(object, offset)`; `ptrtoint` of an object pointer yields an opaque integer tagged with its provenance
in `World`. For the error-pointer idiom only, `inttoptr c` with `c` in the top 4095 values (constant)
and `ptrtoint` of that constant back are inside the fragment: IS_ERR is an integer comparison.
Obligation: the translator's IS_ERR/ERR_PTR encoding is the integer comparison of the constant. The general
case (`inttoptr` of a variable) stays outside (`L unsupported`) until a concrete-address model exists.

**f. Automatic pointer harness (PR #50).** Not an assumption about code: it is a precondition on the
environment (the parameter points at an object of this shape, to depth d, with `elems` elements and
`unknown_bytes` bytes). Lean: a predicate `HarnessOK M F plan world` as a hypothesis of the theorem.
The plan itself is computed in C++ and exported as data (`L harness ...` lines); the Lean side recomputes
it with the mirror of `plan_pointer_harness` and the correspondence checker compares them. Obligation:
Lean plan equals C++ plan on the fixtures (correspondence check), and the theorem is stated under
`HarnessOK`. Extension: the object-graph part of `XMem` (byval/sret harnesses already exist; nested
pointer objects and struct objects are new).

**g. Unknown calls (`--assume-unknown-calls`).** Inside the fragment as an oracle result plus havoc of
pointee tails, with one hypothesis `CalleeReturnsNoFree` per call site (the call is assumed to return and
free nothing). This is a hypothesis of the theorem, not an axiom. The exporter's `unknown-call` assumption
text maps 1:1 to that hypothesis name.

### 3.2 Outside the fragment or named as hypotheses (trust statements)

**h. Kernel API specs (150 now, the core set by W1).** A spec is a statement about the world (the real
kernel function meets the contract). It cannot be proved inside the LLVM semantics, because the callee is
not in the module. Plan:
  1. Each spec `S` gets a Lean contract `KContract_S : Args -> World -> Result -> World -> Prop` written in
     Lean (the source note of W1 becomes its docstring).
  2. The PIR model of `S` (the existing `models/kernel` statements) is mirrored in Lean as a PIR fragment
     and proved to satisfy `KContract_S` (a theorem, not an assumption). This is the validated-spec
     obligation of W1, carried into Lean. Specs whose model is not yet mirrored are marked UNVALIDATED and
     appear in the audit as hypothesis `KContract_S`.
  3. A theorem using a spec takes `hS : KContract_S` for each spec it uses as a named hypothesis, so
     `#print axioms` shows nothing new; the audit lists the hypothesis names per theorem instead.
Outcome: the verdict for a kernel-calling function is `PROVED-ASSUMING` with the list of
`KContract_*` hypotheses, exactly the list the assumption report already gives.

**i. Atomics (`atomics-sequential`) until W3.** A sequential model of an atomic RMW is sound for one
thread only. Inside the fragment as sequential code, plus hypothesis `AtomicsSequential` (no other
thread interleaves). This stays a named hypothesis until W3 (LKMM encoding) and W7 (contexts) replace it:
then the hypothesis is dropped and the theorem is stated over the W3 encoding.

**j. Asm instruction semantics (W4).** Per instruction, an ISA semantics in Lean is a separate project
(the Sail-derived or hand-written model checked against QEMU vectors by the W4 gate). Until a template
has a verified semantics it stays havoc (3.1a, sound with 3.1d), so its only hypothesis is the
havoc, which is not a hypothesis at all. Only W4-verified templates get a `ISA_<insn>` hypothesis, and
the vector checks stay outside Lean (they are tests, not proofs). Mark which in the audit.

**k. Ops tables (W5).** An indirect call `call (load ops->probe)` is an unknown call (3.1g) per target,
with the table's role as a hypothesis `OpsTableRole_<table>` that the target set is the registered one.
This is a trust statement about the registration code, not a proof.

**l. Concurrency contexts (W7).** Out of the Lean proof in phase 2. The lock and context rules are checked
by the PRISM stage; Lean proves only the sequential core. Named as `ContextRules` hypothesis per
theorem where a verdict depends on them.

## 4. Audit output (what must be shown)

* `check.sh` as today: `lake build` no warning; the escape scan with the same words; `#print axioms`
  for each audited theorem within `propext, Classical.choice, Quot.sound`; fixtures agree.
* New: the audit lists, per theorem, its hypothesis names from 3.2 (`KContract_*`, `HarnessOK`,
  `CalleeReturnsNoFree`, `AtomicsSequential`, `ISA_*`, `OpsTableRole_*`, `ContextRules`). A hypothesis
  is not an axiom, so `axioms.txt` stays on the standard three. If a real axiom ever becomes necessary, it
  is listed in `Audit.lean` by name and `check.sh` gets an explicit allowlist line for it.

## 5. Phase 2 order (simplest first)

1. volatile accesses (3.1c): smallest, one flag in `XMem`.
2. ptrtoint / inttoptr, error-pointer idiom only (3.1e): IS_ERR, ERR_PTR, PTR_ERR, container_of.
3. atomics-sequential as hypothesis 3.2i (no proof change, just the audit entry and correspondence).
4. asm havoc with havoc-all-memory (3.1a, 3.1d), then callbr (3.1b).
5. Harness plan mirror (3.1f), unknown calls (3.1g).
6. Kernel specs (3.2h) starting from the e1000e-reached specs.

Each step: GLM drafts Lean, the Lean checker is the gate (no sorry, no top-level axiom, `#print axioms`
listed), the correspondence checker is updated, fixtures added to `proofs/refinement/fixtures`.

## 6. Dependencies

* W1: spec list and source notes (for 3.2h).
* W2: ptrtoint/inttoptr, bitfields, callbr with several labels, havoc-all-memory (3.1b, 3.1d, 3.1e).
* W3: the atomics encoding (3.2i replacement).
* W4: which asm templates have verified semantics (3.2j).
* W5: ops-table role format (3.2k).
* W7: context rule form (3.2l).
Phase 2 starts only from the constructs whose PRs are on odin-loki/PRISM.

## 7. Phase 2 status (W6 first run)

Baseline (Lean 4.34.0, run in RAM): proofs/refinement/check.sh exits 0 with "refinement: OK (104 theorems
audited)", axioms only propext / Classical.choice / Quot.sound; proofs/check.sh exits 0 with "proofs: OK".
Two CRLF checkout artefacts (the refinement fixtures and testdata/fixtures/verdict_tables.json) were the
only failures; fixed by .gitattributes (LF) and a CR-tolerant fixture comparison in check.sh.

Volatile (3.1c) is NOT started and stays : export_lean.cpp rejects every flag other than
nsw/nuw/exact/disjoint/nneg (so a volatile load or store is "outside"). The correct change adds a volatile
bit to load/store in XLlvm, XMem and XPir and to every proof that case-splits on them (about 15 000 lines
of Lean under PrismRefine), plus the exporter and checker line format and a C++ rebuild through the shared
farm. Letting the exporter silently drop the flag would make the Lean verdict claim a volatile access is an
ordinary one without that being a theorem hypothesis, which the trust rule forbids. No proof was attempted
this run and no axiom was added. Next step: add the bit with the VolatileIsOracle hypothesis stated in the
theorem, as in 3.1c.
