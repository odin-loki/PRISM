# PRISM upgrade plan: faster, proving more, proof levels, GPU, and the road to seL4-level assurance

Written 13 November 2026 from the OS-Proving experience ([feedback](feedback/OS-PROVING-FEEDBACK.md), checked against this repository in [the review](feedback/REVIEW-OF-FEEDBACK.md)) and a read-only look at the GPU question ([GPU findings](feedback/GPU-FINDINGS.md)). Nothing here has been built or measured in this repository yet; every gain is a target with a gate, and the first phase is to measure the baseline. All the laws in CLAUDE.md stay: `PROVED` and `BOUNDED` are never merged, a missing tool is `NOTRUN`, a model can never make a proof, nothing is skipped quietly.

## 1. Goals, in the owner's words

1. Run faster (more functions proved per hour).
2. Prove more of real kernel code.
3. Settable levels of "proved".
4. Be more dynamic: choose engines and budgets per function instead of running a fixed pipeline.
5. Use the GPU for throughput where it really helps.
6. Reach seL4-level assurance: a machine-checked refinement proof of an implementation against an abstract specification, with a small trusted base.

## 2. Where PRISM stands (from the review)

Strong already: a verdict lattice proved in Lean with nine laws and a verdict audit; `PROVED-CERTIFIED` (QF_BV, CaDiCaL LRAT checked by cake_lpr and Lean's checker); k-induction and Houdini; a Lean-proved IR translator with a per-run correspondence check; counterexample reproducers; `compile_commands.json` ingestion; caches; memory budget; 0 wrong proofs on the conformance suite (pir proved 302 of 379 true tasks and refuted 165 of 364 false tasks with a replayed counterexample).

Weak for kernel code: unknown callees, volatile/atomic accesses, inline asm and integers wider than 64 bits are UNENCODED (so NEEDS-HARNESS); pointers are flat buffers only (no linked or nested objects, no `\separated`); no translation-unit slicing; functions in a unit run one at a time; no whole-function verdict cache; per-stage (not per-function) resume; no assumption registry, ledger or replay command; no end-to-end functions-per-hour figure.

## 3. Proof levels (the setting)

The lattice already has the levels; the setting chooses a target and a budget. No new verdicts are needed for L1 to L5. L6 and L7 are new families that need a Lean lattice extension (Phase 6).

| Level | Name | Verdict it needs | What it means | Engines |
|---|---|---|---|---|
| L0 | none | NOTRUN / UNKNOWN | nothing established | none |
| L1 | tested | CLEAN (never a proof) | no failure found on n concrete inputs, n stated | concrete execution (CPU, later GPU), fuzz |
| L2 | bounded | BOUNDED | no violation within bound k, k stated | pir unwinding, bmc |
| L3 | proved-assuming | PROVED-ASSUMING | proved under listed assumptions (assumption ids) | pir with harness or contract |
| L4 | proved | PROVED | proved, loops closed within the bound, no unstated assumption | pir |
| L5 | proved-unbounded | PROVED-UNBOUNDED | proved for all inputs and all loop counts | k-induction, Houdini |
| L5c | certified | PROVED-CERTIFIED | as L4/L5 with an independently checked certificate (QF_BV) | CaDiCaL LRAT + cake_lpr + Lean checker |
| L6 | refined | new (Lean lattice extension) | the implementation refines a stated abstract specification, with invariants preserved | refinement framework (Phase 6) |
| L7 | end-to-end | new | L6 plus compiler/binary translation validation and system properties (isolation, information flow) | Phase 6 later |

Use: `prism PATH --level proved --budget 5m` means "try to reach L4 on every function within the budget, report the highest level reached, and say why not higher". Running with `--level tested` is the cheap triage pass; `--level certified` adds certification and costs more. The report shows, per function, level target, level reached, the cheapest next step that would raise it, and the assumption ids behind it.

## 4. Phases

Each phase ends in a gate that is measured on one fixed benchmark so progress is not a matter of opinion.

### P0. Baseline and benchmark (first)

- Build the benchmark: the OS-Proving core set (24,806 functions, with a Frama-C/CBMC verdict and reasons per function), plus PRISM's own conformance suite for soundness. Add a runner that records per function: stage, engine, verdict, level, time split into clang, lower, encode, solve, certify, and peak memory (`PRISM_PIR_PROGRESS`, `extra.solver` already give most of it).
- Measure on this machine (64 threads, 48 GB): functions per hour with `--stage inventory,classify,pir --no-llm --jobs 32`, and the same with other stage sets. Record the share that PRISM proves, refutes, calls NEEDS-HARNESS, and cannot build, by function kind.
- Gate: a committed baseline table (functions/hour, share per verdict, time split). Everything below is measured against it.

### P1. Throughput on the CPU (no GPU needed)

1. Function-level parallelism inside a unit (work queue per function, not per unit), so a 5 MB kernel unit with 100 functions uses the cores.
2. Whole-function verdict cache keyed on (PIR hash, contract hash, every option that changes the answer, engine and solver versions), plus caching of repeated static inline functions across units.
3. Translation-unit slicer: keep the function, the types it needs, and the bodies of the callees that get inlined; verify with the conformance suite that slicing does not change a verdict.
4. A `--preset prove` that runs only the proving stages and no LLM.
5. Per-function done list, a `STOP` file, and a `memory-budget` reason on UNKNOWN.
6. `ledger.jsonl` (one row per function).
- Gate: at least 5x functions per hour on the baseline benchmark with identical verdicts on the conformance suite (target; set the real number after P0).

### P2. Prove more kernel code

1. A kernel model library beside `models/libc`: `kmalloc/kfree`, BSD `malloc`, spinlocks and mutexes, `copy_from_user/copy_to_user`, `list_*`, `container_of`, `div64_*`, atomics, `READ_ONCE/WRITE_ONCE`, `printk`. Models are written as C and verified like user code (the libc models already are).
2. Sound assigns-only stubs for unknown callees: havoc everything the callee can reach, as the indirect-call fallback already does; the stub is listed as an assumption id so the verdict stays PROVED-ASSUMING.
3. Volatile and atomic loads/stores, `__int128`, and inline asm with a contract.
4. Pointer graphs: nested and linked objects built from declared types, `\separated`, `container_of`, flexible arrays, in the harness synthesis (`--pir-drafts` becomes default for the prove preset, still reporting NEEDS-HARNESS when a failure appears only under a draft).
5. Loop summaries for counted loops, list walks, bit scans.
- Gate: the share of benchmark functions that reach L3 or better rises (target: at least double the P0 share; set after P0), with 0 wrong proofs on the conformance suite and no new unsoundness entry in `docs/CONFORMANCE.md`.

### P3. Dynamic scheduling and levels

1. The `--level` setting and per-function target/reached/next-step reporting (section 3).
2. A stage-level scheduler: cheap features (pointers, loops, size, callee count, kind, unit size) choose the order and time slice of interval, concrete execution, pir, k-induction and Houdini, and stop at the first conclusive result. Train it on P0 and P1 data from PRISM runs, not on other tools' labels. Keep the unwind-prediction lesson: a learned choice that loses verdicts is turned off (`docs/SOLVERS.md`), so every learned rule is gated by "no lost verdict on the benchmark".
3. Dynamic budgets: a function that is making progress (a closing invariant, shrinking counterexample space) gets more time; one that is not is cut early, instead of the fixed `max(300 s, 10 x timeout)`.
4. Assumption registry: fixed ids with a one-line statement and a checkable flag; verdicts carry ids, engine version, source and contract hashes.
5. Run-time cross-engine check: `bmc` FAILED against `pir` PROVED on the same function raises an incident finding.
6. `extra.int_semantics` on every verdict.
- Gate: at the same budget, more functions reach their target level than with the fixed pipeline, and no verdict is lost on the benchmark.

### P4. Proof quality

1. `prism replay RESULT` (fresh directory, caches off, compare verdict and assumptions).
2. Per-proof mutation: mutants are filtered by concrete execution first (CPU, then GPU in P5), survivors are re-proved, a kill rate is reported per proof; vacuity audit extended to pointer-extent preconditions.
3. Certified mode for more of the pipeline: certificates for k-induction steps if the solver supports it; a decision on arrays (certify via bit-blasted memory where the budget allows, otherwise state clearly that the verdict is not certifiable). Nothing here promises SMT proof output: only QF_BV certification exists today.
4. Pointer and struct reproducers in `prism regress`, with minimisation.
- Gate: every PROVED on the benchmark has a mutation kill rate and a replay result; none is vacuous.

### P5. GPU (details in section 5)

### P6. Toward seL4-level assurance (details in section 6)

## 5. GPU plan

Honest base: a GPU is a throughput machine for many identical, branch-light tasks. SMT and CDCL solving are irregular and sequential and do not move to a GPU with real gain; the review found most proof time is process start, encoding, and a heavy tail of wide arithmetic, memory-chain encodings, Houdini and certification, none of which is GPU-shaped. The GPU therefore helps with refutation workloads and with model inference, and it removes work from the solver queue.

| Step | What | Gate |
|---|---|---|
| G0 | Check WSL2 GPU access (`nvidia-smi`, `nvcc` from WSL), reconcile the docs' CUDA versions (13.2 vs 12.6.3), build with `PRISM_CUDA=ON`, run the existing ProbSAT doctest, and measure `mutate.cu` against the CPU havoc path | a measured table; decide whether `probsat.cu` and `mutate.cu` are kept |
| G1 | `PRISM_DEVICE=cuda` for the LLM features (needs FORGE's CUDA backend); measure decode speed with grammar masking (the mask is a CPU cost and may dominate) | measured tokens/s; if the mask dominates, precompute or cache it before claiming any gain |
| G2 | A concrete-execution engine on the CPU first: run a function against its contract on random and edge inputs, 64-way, using the existing concrete PIR interpreter. Used for L1, as the first filter in the scheduler (refute early, skip SMT), and as the mutation prefilter | inputs per second per thread measured; a buggy function is refuted in milliseconds on the benchmark |
| G3 | A GPU PIR bytecode interpreter, only if G2 shows concrete execution is the limit: one thread per input vector, a compact register VM for PIR, bounded memory, no unbounded loops without a step limit | at least 10x inputs per second over the 64-thread CPU engine on scalar, loop-light functions, and at least a 30% cut in SMT time on buggy functions |
| G4 | Batched LLM requests through FORGE, only if FORGE supports it and generation (not validation) is the limit | batch throughput vs one-at-a-time |
| G5 | Do not build: a GPU SAT/SMT solver (research-grade, no drop-in, certification needs CaDiCaL's LRAT anyway), explicit-state exploration on the GPU (no engine to port) | not applicable |

Rules for anything on the GPU: it may only refute or report CLEAN (L1); a GPU or fuzz result is never a proof (Law 3). A GPU-found candidate counterexample is replayed on the CPU interpreter before it becomes FAILED. Expected gain from G2/G3 is an estimate until measured; the sound bet is that it removes the buggy fraction of functions from the expensive queue early.

## 6. The road to seL4-level assurance

What seL4 has (from its papers): a complete abstract specification of the kernel; a refinement proof that the C code implements it, written in Isabelle/HOL (about 200,000 lines of proof for about 8,700 lines of C and 600 of assembler; about 12 person-years for the kernel proof); invariants that every operation preserves; a verified chain down to the binary; and a small stated trusted base.

PRISM already has the pieces that are hardest to retrofit: a Lean-proved verdict lattice, a Lean-proved translator from LLVM IR to PIR with a per-run check that the C++ translator agrees, a formal memory model (`proofs/semantics`), a certified QF_BV path with a small trusted base (`docs/TRUSTED_BASE.md`), and independent re-checking of Lean projects (`leanchecker`, `nanoda`). The refinement framework itself is what is missing.

Plan, to be done on a small component first (a pilot of about 1,000 to 2,000 lines of PBSD, for example a capability table, a fixed-size allocator, or a scheduler queue), not on a whole kernel:

1. **Specification layer.** A way to state an abstract specification (abstract state and operations) in Lean, tied to the C functions by a simulation relation.
2. **Memory model.** Extend `proofs/semantics` and the pir memory model from flat buffers to typed objects with separation (`\separated`, framing, linked structures, `container_of`), so a data structure invariant can be stated and preserved.
3. **Invariants.** Each operation must preserve the global invariants; generate those obligations automatically from the specification and discharge them with pir where they are QF_BV, and with Lean tactics where they are not.
4. **Refinement obligations.** For each C function: it terminates, it is free of undefined behaviour, and its effect refines the abstract operation. Automatic where the obligation is bitvector reasoning; interactive in Lean otherwise, with the automatic prover doing as much of the work as it can (this is where the effort per line of code should fall below seL4's).
5. **Fidelity of the C semantics.** Extend the Lean-proved translator fragment to the constructs the pilot uses; anything outside the fragment is UNENCODED and the pilot must avoid it.
6. **Below the IR.** Translation validation of the compiled binary against the IR for the pilot (the way seL4 bridges to the binary), for one target architecture first.
7. **System properties.** Isolation and information flow only after L6 holds for the pilot.

Gate for P6: the pilot component has an L6 theorem checked by the Lean kernel and re-checked by `leanchecker` and `nanoda`, the trusted base is written down, and the effort is recorded in person-weeks per thousand lines so the comparison with seL4's roughly 1.4 person-years per thousand lines is a measurement rather than a claim.

## 7. Risks

- **Soundness is the product.** Every speed or coverage change must keep 0 wrong proofs on the conformance suite and pass the verdict audit. Caches and slicing are the likeliest places to introduce a stale or unsound result; their keys must include every option that changes an answer.
- **Models of kernel code can be wrong.** Every model is code verified like user code, listed as an assumption id, and cross-checked with `pir-vs-bmc`.
- **The GPU may not pay.** G2 on the CPU comes first so the GPU work is justified by a measurement, not by an expectation.
- **seL4-level is years of work for a real kernel.** The honest route is a verified core component plus L1 to L5c labels for the rest, with each claim stated at the level it actually has.
- **Lattice changes are expensive.** Each new verdict needs a Lean change and regenerated tables; prefer `extra` fields and finding classes unless a level is genuinely new (L6, L7).

## 8. Order of work

P0 (baseline) first, then P1 and P2 in parallel with the ledger and registry from P3, then the level setting and scheduler, then P4, G0 to G2, and finally G3 and P6 on measured need. Each phase is complete only when its gate is met on the benchmark.
