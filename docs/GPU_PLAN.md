# GPU and FORGE plan: faster, wider proving at kernel scale

Written 13 November 2026, after the OS-Proving feedback ([feedback](feedback/OS-PROVING-FEEDBACK.md),
[review](feedback/REVIEW-OF-FEEDBACK.md), [GPU findings](feedback/GPU-FINDINGS.md)) and the
[upgrade plan](UPGRADE_PLAN.md). That plan stays the master list (P0 to P6). This file says how the GPU
and FORGE carry as much of it as they soundly can, and what is built so far.

The goal behind it is the owner's: prove as much of the Linux kernel as can be proved, at the level it really
has, quickly. The machine is an RTX 3090 (10,496 CUDA cores, 82 SMs, 24 GB) beside 64 CPU threads; the
design must scale to a card with twice the cores (about 20,000) or to several cards without changes.

## 1. The rule that decides what goes on the GPU

A GPU runs many identical, branch-light tasks. SMT and CDCL solving are neither, so the solver stays on the
CPU (findings section C.4). What a GPU does well for a prover is **execute the program under check on very many
inputs at once**. That gives four things, each sound by construction:

1. **Refutation.** A concrete input that violates a checked property, re-run on the CPU interpreter
   (`pir::interpret`, the same judge that replays every SMT counterexample), is a FAILED verdict with a
   concrete witness. The SMT queries for that function are skipped. Bugs past the unwind bound are found too,
   because execution has no bound.
2. **Exhaustion.** When the inputs of a function have few bits in total (two `u16`, one `u32`, a `u8` and a
   `bool` ...), every input can be executed: 2^32 inputs of a small function take seconds on the 3090. That
   decides functions SMT times out on (16-bit division and multiplication, bit tricks, CRC steps).
   Today it is recorded as evidence and cross-checked against SMT; it becomes a proof only through G2 below.
3. **Pruning.** Concrete states at loop heads falsify candidate invariants before Houdini spends solver
   rounds on them (G3). Dropping a candidate that a reachable state falsifies can never make a wrong proof.
4. **Measuring proofs.** Mutants x inputs is a grid: the GPU kills mutants so only survivors need the prover
   (feedback item 7).

Laws unchanged: a GPU result is never merged with a proof, a missing GPU is `NOTRUN` (here: the CPU twin runs
and says so), nothing is skipped quietly, and nothing from the scanned tree is executed (the VM interprets
PIR, PRISM's own translation; Law 9 is untouched).

## 2. FORGE is the platform for all of it

PRISM already runs on FORGE for logging, processes, settings, the thread pool, the cache and inference. This
plan uses FORGE for the GPU side too, so PRISM does not grow a second runtime:

| FORGE part | Used for | Status |
|---|---|---|
| `forge::task::pool` (via `prism::pool`) | the CPU twin of the GPU VM; function-level parallelism inside a unit (P1.1) | VM: **done**; P1.1 planned |
| `forge/core/dl.hpp` (`dlopen` on every OS, `FORGE_DLNAME`) | loads the CUDA **driver** (`libcuda.so.1` / `nvcuda.dll`) at run time; no cudart or nvcc at link time, one binary runs with or without a GPU | **done** |
| `forge::mem::governor` pool `cuda:0` | every device allocation of the VM is charged to the same pool FORGE inference charges, so a model and the sweeps share the card without OOM | **done** |
| `forge::gpu::probe_backends` | which runtimes exist (CUDA, HIP, Level Zero, Vulkan, OpenCL); the same VM source is planned for HIP next (G9) | used for the report; HIP planned |
| `forge::cache::store` | whole-function verdict cache keyed on PIR hash, contract hash, options and engine versions (P1.2) | planned |
| `forge::gpu::kernel_cache` | caches the driver-JIT'd cubin of the embedded PTX across runs | planned (the driver's own JIT cache covers it today) |
| `forge::task::sched` | stage scheduler: deadlines and priorities per function (P3.2, P3.3) | planned |
| FORGE inference, `PRISM_DEVICE=cuda`, grammar-mask cache (FORGE #72) | LLM features on the GPU; batched requests for corpus-scale contract and invariant drafting (G8) | config works; batching needs FORGE API |
| `forge::prof` | per-phase timings for the ledger (P0) | planned |

## 2a. Everything that can go on the GPU, stage by stage

The owner's rule is "everything that is possible on the GPU". This is the whole pipeline against that rule.
"Sound" means a GPU answer can never turn into a wrong proof: either it is a model or witness that the CPU
re-checks, or it only removes work whose removal cannot change a verdict.

| Pipeline part | GPU work | Sound because | Status |
|---|---|---|---|
| pir functions (scalar) | input sweep: refute, or run every input (G1) | witness replayed in `pir::interpret` | **built** |
| pir functions (memory) | the same with a device memory model (G4) | same | planned |
| Z3 / portfolio: satisfiable VCs | GPU local search on the bit-blasted CNF (`probsat.cu`, G7), started next to Z3 for queries predicted slow; FastFourierSAT-style continuous search later | every model is validated in Z3 before it counts | kernel exists; wiring planned |
| Z3 / portfolio: candidate models | batch evaluation of many candidate assignments of one VC (the VM evaluates the formula's PIR, not the SMT term) | evaluation only proposes; Z3 validates | planned with G7 |
| Z3 / portfolio: UNSAT VCs | none: no GPU CDCL beats CPU CDCL on UNSAT (PARALLEL_SOLVING_PLAN section 1) | n/a | **not planned, by evidence** |
| Z3 bit-blasting / CNF preprocessing | GPU variable elimination for very large CNFs (ParaFROST's idea; ParaFROST itself is GPL and cannot be linked) | the simplified CNF is equisatisfiable; certified mode keeps the original CNF for LRAT | research; measure on the tail first |
| Houdini, k-induction | concrete loop-head states falsify candidate invariants before any solver round (G3) | a falsified candidate is not an invariant | planned |
| Mutation of proofs | mutants x inputs grid (G6) | only filters; survivors are re-proved on the CPU | planned |
| fuzz stage | batched havoc mutation (`mutate.cu`, needs `nseeds > 1` to pay) | fuzzing is never a proof (Law 3) | kernel exists; batching planned |
| AI features | FORGE inference on CUDA, batched decoding (G8) | output is HYPOTHESIS (Law 4) | config works; batching needs FORGE |
| Triage embeddings | FORGE embedding model on CUDA | not a verdict | config works |
| Corpus scheduling | persistent kernel fed by the CPU workers (G5) | scheduling only | planned |

Z3's own search (CDCL, propagation, clause learning, the SMT core) stays on the CPU; what the CPU side does
to Z3 to make it faster and parallel is in [PARALLEL_SOLVING_PLAN.md](PARALLEL_SOLVING_PLAN.md).

## 3. Workstreams

Each has a gate measured on the P0 benchmark (OS-Proving core set plus the conformance suite), with 0 wrong
proofs as a hard condition.

### G1. PIR bytecode VM on GPU and CPU (built in this change)

- `pir::vm::compile` flattens a scalar PIR function (integer ops up to 64 bits, phis, branches, checks,
  assumes) into 16-byte instructions. Anything else is refused with a reason: memory statements, floating
  point, havoc and nondet sources, more registers than the device kernel holds.
- One VM source, `src/prism/pir/vm_core.inc`, is compiled twice: into PRISM (the CPU twin, on the FORGE pool)
  and by clang to PTX for `sm_86` (`src/cuda/pir_vm.cu`, no CUDA SDK needed at build time). The PTX is
  embedded in the binary and JIT-compiled by the driver, so newer cards (sm_89, sm_90, sm_120) run it too.
- Inputs are generated on the device from the lane number: edge values per width (0, 1, 2, -1, -2, MIN, MAX,
  MIN+1) in a full cartesian block first, then edge-biased random values; or every input when the parameters
  have at most `PRISM_GPU_EXHAUSTIVE_BITS` bits in total. Nothing is uploaded per input.
- The smallest violating lane wins (deterministic across runs and devices). Its input is regenerated on the
  host and replayed in `pir::interpret` on the function as the SMT stage sees it (`--pir-props` filter
  applied). Only a replayed violation becomes FAILED, with `extra.found_by = sweep` and the engine named.
- In the pir stage the sweep runs before `check_function`. A FAILED skips the solver for that function;
  otherwise the solver runs as before and the sweep's coverage is recorded in `extra.sweep`.
  `PRISM_GPU_CROSSCHECK=1` runs the solver anyway and records any disagreement in `extra.engine_disagree`
  (feedback item 8, at run time).
- Gate: the CPU twin equals `pir::interpret` on every function of `testdata/` it accepts; the GPU equals the
  CPU twin lane for lane; the conformance suite has the same verdicts except FAILED rows that now come from
  the sweep, which must carry the same class.

### G2. Exhaustion as an admitted proof origin (the big speed-up for hard small-domain functions)

An exhaustive run where every lane returned, stopped or failed an assumption, with zero step-limit and zero
unmodelled lanes, is a proof for that function's PIR, provided the VM is the PIR semantics. To admit it:

1. A Lean model of the bytecode and `vm_step`, with a theorem that `compile` preserves the PIR semantics of
   `proofs/semantics` (or a per-run correspondence check, the way `pir_lean_check` checks the translator).
2. A new origin `exhaustive` in `proofs/Prism/Verdict.lean`, admitting `PROVED` only with
   `extra.sweep_complete = all` and the lane counts; regenerate `testdata/fixtures/verdict_tables.json`.
3. A second, independent run of a random 1/1024 of the lanes on the CPU twin per proof, recorded.

Until then the exhaustive result is evidence only. Gate: `divmod16`, `mulimpl10` and similar SMT time-outs
become PROVED by exhaustion in seconds, and no PROVED-by-exhaustion contradicts an SMT verdict on the suite.

### G3. Invariant pruning for Houdini and k-induction

Run the loop on the GPU, snapshot the state at each loop head (bounded number of snapshots per lane), evaluate
every Houdini candidate on every snapshot, drop the falsified ones before the first solver round. Houdini is
the measured time sink (`npr` 250 s, `strcat` 181 s, BOUNDED in the end). Gate: Houdini solver rounds and time
cut by at least half on the loop corpus, identical verdicts.

### G4. Memory in the VM

A per-thread object table and byte cells in a device arena (the `ConcMem` model: size, live, kind, align,
init bit), sized per function with a fall back to the CPU when it does not fit. This extends G1 to flat-buffer
pointer functions under their contracts (`\valid(p+(0..n-1))`), to the libc and kernel models, and to
`container_of` and list walks once P2.4 builds pointer graphs. Gate: the share of benchmark functions the
sweep accepts rises from scalar-only to include flat-buffer functions.

### G5. Corpus scheduling across 20,000 cores

One persistent kernel takes functions from a device work queue so the card stays full across a corpus
(today one launch per function chunk). CPU workers do clang, opt and translation and feed the queue; results
stream back. With two cards, one queue per card (FORGE probe enumerates them). Gate: GPU busy above 80% on
the benchmark with `--jobs 32`, functions per hour measured against P0.

### G6. Mutation kill rate on the GPU

Mutate PIR (operator swap, constant off by one, inverted comparison, dropped statement), kill mutants by
concrete execution, re-prove only survivors on the CPU. Report `extra.mutation_kill` per PROVED function.

### G7. GPU ProbSAT, measured, then wired or deleted

Move `src/cuda/probsat.cu` onto the same driver path (PTX, FORGE `dl`, governor), run it on bit-blasted VCs
from real functions against the CPU walker, CaDiCaL and Kissat. Wire it as a portfolio member only if it wins
a VC class; the portfolio already validates every model in Z3.

### G8. AI on the GPU through FORGE

`PRISM_DEVICE=cuda` when FORGE's probe finds CUDA (it is `cpu` by default today). Batched decoding of many
functions' invariant and contract prompts needs a FORGE batch API; output stays HYPOTHESIS and is validated by
Houdini or the solver (Law 4).

### G9. Other GPUs

The VM source has no CUDA dependency beyond the thread index and two atomics. Build it for HIP (AMD) and
Level Zero (Intel) through the backends FORGE already probes, so the sweep runs on whatever card exists.

## 4. Toward the Linux kernel

Proving "the kernel" is a ladder, not one theorem (upgrade plan section 6). The GPU work moves the bottom
rungs fast, so the expensive top rungs get the time:

1. **Triage the whole tree** (L1): lower every function the kbuild flags allow, sweep every one the VM
   accepts, refute the buggy ones in milliseconds. OS-Proving's 24,806 functions are the first benchmark.
2. **Close the small-domain functions** (L4 by exhaustion, after G2): bit operations, `lib/` helpers,
   checksums on bytes, flag decoders.
3. **Prove the rest with SMT** (L3 to L5c) on the CPU, with the solver time no longer spent on refutable or
   exhaustible functions; kernel models and assigns-only stubs (P2) widen what pir can encode.
4. **Refinement for a core** (L6, P6): a verified component with a stated trusted base.

## 5. Order

G1 (done here), then P0 baseline with the sweep on and off, then G3 and G4 (largest measured wins), G2 (needs
Lean), G5, G6, G7, G8, G9. Each step is complete only when its gate is met.
