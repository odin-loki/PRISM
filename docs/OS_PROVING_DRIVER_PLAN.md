# Plan: proving C drivers through clang, LLVM IR, PRISM and Lean (from the OS-Proving project)

Copied from odin-loki/OS-Proving docs/PLAN_C_DRIVERS_TO_LEAN.md on 2026-11-16. Status: plan. The Stage 0 numbers in this PRISM copy are the OS-Proving clang pass pilot figures and may be replaced by the final census. See docs/feedback/DRIVER-REQUIREMENTS.md for what PRISM needs.


## 1. What already exists (checked in the PRISM repo)

- PRISM `pir` stage: clang -O0 to LLVM IR, fixed `opt` passes, textual IR parser, PIR, Z3 bit-vectors, verdicts FAILED / PROVED / BOUNDED / PROVED-UNBOUNDED / NEEDS-HARNESS (docs/PIR.md). Libc models, a memory model, floating point, exceptions, setjmp, indirect calls and inline assembly each have some support.
- Lean side (proofs/semantics, proofs/refinement): PIR syntax, semantics, a UB predicate and a bounded encoder proved exact; a fragment of LLVM IR with formal semantics; the translator mirrored in Lean and proved: if the LLVM function reaches UB or creates poison, the PIR program fails a check (`pir_sound`). `export_lean.cpp` writes each translated function as an LLVM fragment plus its PIR (.pirl) and a correspondence checker confirms the C++ output equals the Lean translator's. The Lean projects ban `sorry`, `native_decide` and `bv_decide`; an audit script lists axioms.
- Our data: 1,135,593 functions with kinds, modules and a hierarchy (data/master, data/modules-all), contracts and their weaknesses (strength, mutation, S5 audit), module contracts for 8,367 driver modules (data/driver-full, REVIEWED only), and the clang pass in progress (compile commands, functions, ops tables, encodability census).

So the checked chain is: clang (trusted front end) -> LLVM fragment semantics in Lean (proved translator) -> PIR -> solver. What is missing is everything kernel-specific.

## 2. Proof levels (the settings Odin asked for)

| Level | Claim | Evidence | Trust |
|---|---|---|---|
| L0 | compiles under the real flags | clang pass | compiler |
| L1 | no UB or memory error on any input within a stated bound | PRISM BOUNDED | solver, bound |
| L2 | no UB or memory error on any input, unbounded | PRISM PROVED-UNBOUNDED / PROVED-CERTIFIED (QF_BV) | Lean-checked translator, certified solver, environment specs |
| L3 | function meets its contract | pir or wp with contract, strength class functional | as L2 plus the contract being the right one |
| L4 | module invariants and resource pairing hold on every entry-point sequence | module state machine plus function summaries plus a device model | as L3 plus the device model |
| L5 | refinement to an abstract spec (seL4 style) | not planned in this phase | |

Every result carries its level and the list of assumptions it used (the repo already tracks assumptions per proved unit).

## 3. Stages, each with a go/no-go gate

**Stage 0: baseline (in progress).** The clang agent reports, for sampled functions, how many encode in PRISM and the reasons for the rest. Gate: a per-reason table of what blocks encoding (inline asm, unknown calls, bitfields, atomics, indirect calls, container_of, packed structs, vector types). No later stage starts blind.

**Stage 1: Tier 1 targets, pure input-handling code.** Parsers, deserializers, scalar and bit-vector helpers (our parser, arith-helper, bitmask kinds; about 52,000 + 65,000 + 300 functions, of which only the clang-compilable ones count). Property: L2 "never UB, never out of bounds, never panics on any untrusted input" (the Binder-style claim). Pilot: 200 seeded functions. Report: encoded %, L2 %, counterexamples (each one triaged by hand: real bug, model artefact, or missing precondition), time per function, memory per process (2 GB cap). Gate: at least a third of the compilable Tier 1 sample reaches L1 or better without hand work, else fix encodability first. Part of this reuses the 5,855 functions already proved by Frama-C WP and CBMC: re-run them through PRISM as an independent second engine, which also cross-checks both.

**Stage 2: the kernel environment model.** A table of specs for the kernel API that driver code calls: allocation (kmalloc/kzalloc/kfree, devm_*), locking (mutex, spinlock, RCU), user access (copy_from_user/to_user, get_user), MMIO (readl/writel, ioread/iowrite), DMA (dma_alloc_coherent, dma_map), IRQ, clocks, workqueues, refcounts. Same style as PRISM's libc_models, each spec with a contract, a note of what it assumes and where the number came from. Untrusted-input model: user pointers and device register reads are arbitrary values within stated width, constrained only by the device model. Gate: specs for the API surface that the top 50 sampled drivers use (taken from the clang call graph, counted by script), each spec reviewed by hand; a wrong spec makes a wrong proof, so this is reviewed code, not generated.

**Stage 3: extend the encodable subset on kernel C.** In priority order by the Stage 0 table. For each construct added to PRISM's translator, extend the Lean mirror and its soundness proof and keep the audit green (no sorry, axioms listed). Inline asm and atomics get conservative models (havoc with stated effects) and are flagged as assumptions. Gate: encodability on the Tier 1 sample goes up measurably; every added construct has a Lean proof or is labelled an assumption.

**Stage 4: contracts (L3).** Take our mined contracts for the Tier 1 and Tier 2 functions, translate them into PRISM contract form, and check them. Use the strength data: range-only contracts are not worth proving as functional; fix weak ones first (the 2,933 range-only and 3,270 mutation survivors are the work queue). Gate: functional-strength contracts proved on a sample, with mutation survivors reported.

**Stage 5: resource pairing and entry-point skeletons (toward L4).** From the clang ops tables take each driver's entry points, build the module skeleton (probe, remove, irq, open, close, suspend, resume), and check on the CFG that every acquire has a release on every path including error paths and remove. Pilot on one small complete driver (candidates from the e1000e / e1000 pilot components or a smaller rtc or watchdog driver). Gate: pairing checked mechanically on 20 modules, with the leads from the GLM/DeepSeek run (data/driver-full) used as a check list, not as evidence.

**Stage 6: device model and the module state machine (L4).** For one small device write a register-level model in Lean (reset, ready, busy, error, the allowed register sequences), take the module skeleton from Stage 5, and prove invariants (no resource leak, no use after remove, interrupt handler safe during teardown) with function summaries from Stage 4. This is the research step. Gate: one complete driver at L4 with every assumption listed; then decide on scaling or on a second device.

**Stage 7: reporting.** Per function and per module: level reached, assumptions, which engine, replay result, mutation result. All in the master file as new columns. Human review of a random sample of each level (the S5 re-sample finding applies: model readers are not enough). The assurance statement is updated with level counts and no claim above what a level supports.

## 4. Resources and rules

- Compute: Z3 per function, at most 2 GB per process, up to 32 cores but the VM has 48 GB, so about 20 workers; scratch in RAM, results on D:. No paid API needed for stages 0 to 3 and 5 to 7; Stage 4 may use Sonnet or DeepSeek to draft contract translations, always followed by the checker.
- Where work happens: encoder and Lean changes in the PRISM repo through pull requests (its CI audit must stay green); extraction, census and data in this repo.
- Agents can do stages 0, 1 and 5 largely alone; stages 2, 3 and 6 need design decisions and reviewed specs, so they are done with Odin's review of each spec table and each Lean change.

## 5. Risks, stated plainly

- Specs of kernel APIs and of the device are assumed, not proved. A proof is only as good as them. Mitigation: list them, review them, test them against real executions where possible (PRISM's translation validation against lli).
- Encodability of real kernel code may be low at first; Stage 0 measures it before we promise anything.
- Concurrency (interrupts, locks, RCU) is hard; Stage 6 starts with one device and a sequential model of its entry points plus a lock discipline check.
- Loops and unbounded data structures need invariants; PRISM has Houdini but expect many functions to stay at L1.
- seL4 comparison: seL4 proves refinement of the full kernel; here L2 and L3 are per-function guarantees and L4 is per-module for a few drivers. The honest line is "machine-checked absence of runtime errors for a measured share of input-handling code, and a worked end-to-end driver proof", not "drivers proved".

## 6. First actions once the clang pass reports

1. Read data/clang results and the encodability table; fix the plan's Stage 1 sample size from it.
2. Run the 200-function Tier 1 pilot (Stage 1) and the 5,855-function PRISM cross-check.
3. Start the kernel API spec table from the call census (Stage 2), pick the pilot driver for Stages 5 and 6.
