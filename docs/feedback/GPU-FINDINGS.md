# GPU throughput for proving many functions: what exists, where time goes, what could move

Machine (as stated by the requester, not probed here): NVIDIA GeForce RTX 3090 (24 GB, compute capability 8.6 = sm_86),
64 CPU threads, 48 GB RAM, WSL2 on Windows 11, CUDA 13.2 installed on Windows. Whether `nvcc` or the CUDA runtime is
usable from inside WSL was NOT checked (see D.1). Repository: `C:\ParanoidBSD\PRISM`, read only; nothing was built or run.
Every number below is a figure quoted from this repository's docs (with the file) unless marked ESTIMATE or UNVERIFIED.
Estimates are my own reasoning and must be measured before anyone relies on them.

## A. What the existing CUDA code does

Build: `CMakeLists.txt:28` option `PRISM_CUDA` defaults ON (but `CLAUDE.md` builds with `-DPRISM_CUDA=OFF`). With ON,
`CMakeLists.txt:625-694` finds nvcc (host compiler forced to gcc because "nvcc cannot use clang++ as host"; no
`-allow-unsupported-compiler`), sets `CMAKE_CUDA_ARCHITECTURES` 86, C++17 for CUDA, and builds one shared library
`prism_cuda` from `src/cuda/mutate.cu` and `src/cuda/probsat.cu`, linked into `prism_core`, defining `PRISM_HAS_CUDA`.
A missing nvcc is a CMake WARNING and the kernels are skipped ("NOTRUN, not a silent disable").

1. `src/cuda/mutate.cu` (67 lines): `prism::cuda::havoc(host, seed_len, nseeds, tick)` copies a byte buffer to the
   device, runs one thread per byte applying one of 8 AFL++-style mutations (xor, random, 0xFF, 0x00, INTERESTING_8/16/32
   overlays, swap), copies back, frees. Caller: `src/prism/havoc.cpp:28-34`, used by the `fuzz` stage's mutation step
   (`docs/MINED.md:92`: "Mutate (CUDA havoc when the kernel built, xsimd / Python otherwise)"). The call site passes
   `nseeds = 1` and one input at a time, with a `cudaMalloc`, two `cudaMemcpy` and a free per call. For one input of a
   few hundred bytes this is dominated by launch and copy latency. ESTIMATE: slower than the CPU path in
   `havoc.cpp:35-` for such sizes; nobody has measured it. The mutation is a tiny part of fuzzing cost anyway
   (the executions are the cost).
2. `src/cuda/probsat.cu` (186 lines): `prism::solver::cuda_probsat(cnf, seed, max_flips, walkers, assignment)`; one CUDA
   thread = one ProbSAT walker (polynomial break-only, cb 2.38 to 5.4, eps 1) over one CNF in CSR form. Per-walker
   state `ntrue`, `unsat`, `where` is `3 x 4 bytes x num_clauses` in global memory per walker (so 1024 walkers on a
   1M-clause CNF would need about 12 GB). Clauses longer than 64 literals make it return false. The first walker to reach
   zero unsatisfied clauses writes its assignment; the host re-checks all clauses. Doc: header comment of the file,
   `docs/SOLVERS.md:216-227`, `docs/TRUSTED_BASE.md:246-249`.
   **It has never been compiled by nvcc or run**: the file header and `docs/SOLVERS.md:222-227` say it was only
   type-checked with clang's CUDA front end against a stub header; the roadmap says "NOT DONE on GPU"
   (`docs/ROADMAP_STATUS.md:33,88`). **It is not called by the portfolio**: the only caller of `cuda_probsat` outside
   its definition and its declaration (`include/prism/solver.hpp:269-275`) is one doctest,
   `tests/cpp/test_main.cpp:4631-4670` ("solver: GPU ProbSAT finds a planted assignment": 800 variables, 3,200 clauses,
   planted solution, 1,024 walkers x 200,000 flips; it writes wall times to `/tmp/prism-gpu-probsat.txt`). The portfolio's
   `sls` member is the CPU walker (`docs/SOLVERS.md:86`, `src/prism/solver/cnf.cpp`).
   Soundness handling is already right for a GPU member: it can only return a candidate assignment, which `solve()` maps
   back and validates against the original formula in Z3 before reporting (`docs/TRUSTED_BASE.md:246-249,296`).
3. AI path: FORGE inference (external repo `odin-loki/FORGE`, fetched by `cmake/forge.cmake`, not vendored here) runs the
   `.fpk` models in process. Device: `PRISM_DEVICE`, else `FORGE_DEVICE`, else FORGE's choice; the docs say the default is
   `cpu` (`docs/AI.md` "Backend" paragraph; `include/prism/forge_llm.hpp:49`; env listing `src/prism/config.cpp:492`).
   So on this machine the first no-code change for any AI feature is `PRISM_DEVICE=cuda` (needs the FORGE CUDA backend to
   be built into the FORGE checkout PRISM links: UNVERIFIED). `forge_llm.hpp:41-42` says "FORGE serialises runs on a model";
   each call gets a fresh session, so PRISM issues no batched requests. Features that call a model: invariant proposal for
   loops, harness drafting, CEX explanation, contract drafting, assumption audit, Lean proof search (`prism prove`),
   triage embeddings, Fuzz4All/ChatFuzz-style generation and repair (`docs/AI.md`, `docs/PLAN.md`). None is on the
   deterministic proof path; `--no-llm` removes them, and Law 4 says a model can never make a proof.
   AI quality and speed on a real model were never measured (`docs/AI.md:384-402`).
4. Doc drift to fix before building: `docs/PLAN.md:52` says CUDA 13.2; `docs/DEPENDENCIES.md:17` and `CMakeLists.txt`
   comments pin CUDA 12.6.3 (nvcc 12.6.85); `CMakeLists.txt` comment says "CUDA 12.0 nvcc rejects gcc 13". CUDA 13.x
   toolchains drop some older architectures but keep sm_86; whether PRISM's `.cu` files compile unchanged with a 13.2
   nvcc is UNVERIFIED.

## B. Where the time goes when proving many functions

The proving stage is `pir` (Clang IR to PIR to Z3 bitvectors, `docs/PIR.md`), plus the older `bmc`/`contracts`/`wp`
stages. Stage order: `include/prism/pipeline.hpp:16-20`. A default `prism PATH` run executes every stage that can run, so a
corpus run that only wants proofs should select stages (`--stage inventory,classify,pir`, `--no-llm`), because the
others cost far more than they give: on zlib the full scan was 698 s wall; "with `--skip pir` zlib finishes in ~1365 s
(fuzz 763 s, optional 280 s, bmc 260 s)" (numbers from different loaded runs, `docs/EVALUATION.md:233-247`).

Measured numbers the docs give, by phase of one pir function check:

| phase | evidence | what it says |
|---|---|---|
| clang + opt (per unit, a process each) | "The pir stage there [testdata, 1,069 functions] is dominated by the Clang front end, not by Houdini" (`docs/PIR.md:1184-1191`); library models lowered once per run and cached on disk (`docs/PIR.md:451-465`) | For small functions, process start and whole-unit lowering dominate. A unit with 1 function costs a full clang run; a 5 MB kernel TU costs clang time once but is then checked function by function in one thread (`stage.cpp:2755`, `docs/EVALUATION.md:244-245`). |
| translate + encode (C++ building Z3 terms, value sets, memory chains) | map erase at input-dependent key + destructor: did not finish in 900 s, now 1.5 s after value sets (`docs/PIR.md:325-345`); `vector::insert` at a symbolic position 1800 s+ then ~24 s and 1.5 GB (`docs/PIR.md:793`); function budget exists because "each query is bounded but a function has many of them" (`docs/EVALUATION.md:238-242`) | Heavy tails come from memory-model shape, not from SAT hardness. CPU and RAM bound; Z3 expression DAG building is pointer chasing and not GPU shaped. |
| solving, typical VC | median 19-29 ms per member, p90 33-63 ms; 4,179 conformance VCs took 211-229 s total (about 50 ms each, loaded 4 cores, `docs/SOLVERS.md:245-285,304-307`); Z3 gets a 0.15 s head start because most VCs are answered in ms | The solver is not the bottleneck for ordinary VCs; process spawns and bit-blasts are what the portfolio must avoid paying. |
| solving, hard VC | wide multiplication / division / popcount: `mulimpl10`, `divmod16` time out at 30 s in Z3 and in the portfolio; `long_mul_true` times out in every member (`docs/SOLVERS.md:471-540,302-303`); a linked-list file of 7 functions 378-404 s before, 100-134 s after property grouping (`docs/PIR.md:1331-1340`); zlib `crc32.c` dominated by Z3 simplifier/solver on 64-bit braided CRC VCs (`docs/EVALUATION.md:233-239`) | Real but rare; this is general bitvector arithmetic reasoning. |
| loops: unwind, deepening, k-induction, Houdini | tinyexpr `npr` spends most of 250 s in Houdini and ends BOUNDED; `strcat` 181 s BOUNDED, 293/489 s at timeout 100 with no proof (`docs/EVALUATION.md:243-245`, `docs/PIR.md:1192-1200`) | The per-loop search is repeated solver calls on small, related queries: CPU incremental solving. |
| certified mode | 11,160 s over 489 tasks at `-j 2` (about 23 s per task) on a loaded machine; one proof 862 MB and cake_lpr needs 4.1 GB for it; `long_mul_true` 184 s CPU with no CaDiCaL answer (`docs/CONFORMANCE.md:205-222`) | LRAT proof generation and checking: memory and CPU bound, sequential checkers. |
| translation validation (`--allow-exec`) | `lli` on 64 input vectors per function, sandboxed (`docs/PIR.md:1449-1470`) | A process spawn per function; this is a concrete-execution workload (candidate for the GPU, section C.2). |
| LLM features | not measured in the repo (`docs/AI.md:384-402`). Outside figure from the draft feedback, UNVERIFIED here: about 118 s per `prism prove`, grammar mask about 95% of decode time | The grammar mask is the CPU sampler; a GPU for the forward pass does not shorten it (C.1). |

Budgets (`src/prism/config.cpp:486-532`, `docs/PIR.md:1348,1395-1415`): `--timeout` 30 s per VC; `PRISM_FUNCTION_BUDGET`
default `max(300 s, 10 x timeout)` per function; `PRISM_HOUDINI_BUDGET` run-wide invariant search; certification budget
`max(240 s, 8 x timeout)`; memory budget 75% of the smaller of cgroup limit, MemAvailable and half RLIMIT_AS
(`docs/PIR.md:1543-1563`, `PRISM_MEM_BUDGET_MB`). Parallelism: `--jobs N` (default half the cores, so 32 here) over units;
inside a unit functions are sequential; each query gets `max(2, cores/jobs)` solver members (`docs/PIR.md:1348`).
The learned scheduler was trained and measured on a 4-core shared machine (`docs/SOLVERS.md:290-350`), so its head start
and member limits have not been tuned for 64 threads (UNVERIFIED effect).

Conclusion on the bottleneck for "many functions": (1) per-unit clang/opt and one-function-at-a-time checking inside a
unit (CPU parallelism and slicing, no GPU), (2) the heavy tail of functions whose encoding or solving takes minutes
(Z3 memory chains, wide arithmetic, Houdini), (3) certification if `--certified`, (4) anything else in the default
pipeline that is not a proof stage. No end-to-end figure of "functions proved per hour" exists in this repository.

## C. What could move to the GPU

Honest framing: a GPU is a throughput machine for thousands of identical, branch-light, memory-regular tasks. General
CDCL/SMT solving is the opposite (irreducible irregular memory, clause learning, deep sequential dependency). The
numbers above say most time is process, encoding and a heavy solver tail. Realistic GPU gain is therefore limited to
(a) refutation workloads and (b) AI inference. Everything in the table is a candidate; expected gain is ESTIMATE.

| # | Candidate | Fit | Expected gain (ESTIMATE) | Main risks |
|---|---|---|---|---|
| C.1 | LLM inference on the GPU (`PRISM_DEVICE=cuda`, FORGE `.fpk`, Qwen 3.5 9B Q4 fits in 24 GB) | Good for the forward pass | Large for the features that call a model (a 9B model on a 3090 vs CPU is typically one order of magnitude or more in decode; not measured in this repo). Zero effect on proof throughput, since proofs never use a model. | Grammar masking is CPU-side per token and was reported as 95% of decode time (outside claim, UNVERIFIED): GPU forward does not help until the mask is cached/precomputed. FORGE serialises runs per model and PRISM issues one request at a time, so there is no batching; batching would need FORGE changes (FORGE source is not in this checkout: UNVERIFIED what it supports). CUDA backend of FORGE must be built. LLM output stays `HYPOTHESIS` (Law 4). |
| C.2 | Massively parallel concrete execution of a function against its contract on random + edge inputs (one thread = one input vector) | Best fit. Embarrassingly parallel; refutes without SMT; produces concrete counterexamples that `prism regress` / the replay path can use | A 3090 has 10,496 CUDA cores. If a CPU interpreter does about `X` statements/s/thread (not measured), GPU vs the same code on 64 threads is plausibly 3x-20x for loop-light scalar functions, falling to about 1x for memory-heavy or divergent ones. ESTIMATE only. The real win is in what it avoids: a function with a bug, found in milliseconds by 10^6 inputs, never needs the 30-300 s SMT budget; Houdini/unwinding is skipped for FAILED. It cannot make a PROVED. | Never a proof (Law 3: `CLEAN` is not a proof; `docs/VERDICTS.md` `admit_fuzzer_never_proof`). Random inputs rarely hit narrow bugs such as `x == 0x1234`; value-set/edge-case seeding needed (PRISM already generates `INT_MIN, -1, 0, 1, INT_MAX` per width, `docs/PIR.md:1451`). Needs a GPU-interpretable IR (section D). The OS-Proving feedback reports 436 random-test counterexamples wrong only at integer limits: a harness must use exact C semantics or the reports mislead (`REVIEW-OF-FEEDBACK.md` item 2). |
| C.3 | GPU-parallel stochastic local search SAT (`probsat.cu` exists) | Only SAT-side counterexample finding on bit-blasted CNF | Probably small on this corpus: the docs measured ProbSAT as fastest alone on 46 of 2,060 VCs, and the real portfolio answered 7 of 4,179 conformance VCs with `sls` while Z3 answered 3,923-4,179 (`docs/SOLVERS.md:259-283,298-299`). Where it can matter: large satisfiable arithmetic VCs (factoring-type) where Kissat/CaDiCaL already win. A cheap measurement exists: build with `PRISM_CUDA=ON` and run the existing doctest. | Never built or run; memory O(walkers x clauses); one walker per thread has poor coalescing and warp divergence; bit-blasted multipliers make hard satisfiable CNFs for SLS; wins only against the CPU walker, not against CaDiCaL/Kissat (not measured: "GPU exit criterion not measured", `docs/SOLVERS.md:226-227`). |
| C.4 | GPU bit-blasting / GPU parallel portfolio | Bit-blasting itself is cheap (Z3 tactics, milliseconds); a GPU SAT/SMT solver competitive with CaDiCaL/Kissat/Bitwuzla is not available as a drop-in (research-grade, UNVERIFIED) | Likely zero or negative. | High build cost; certified mode needs LRAT from CaDiCaL anyway (`docs/SOLVERS.md:132-200`). Do not build. |
| C.5 | Batched LLM inference through FORGE (many functions' invariant/contract prompts at once) | Plausible if FORGE supports batched/continuous decoding | Could raise token throughput several-fold for corpus-scale contract/harness drafting. | Every output is a `HYPOTHESIS` re-validated by Houdini/BMC (CPU), so the pipeline is bounded by the validation, not by generation. Requires a FORGE API change; PRISM's `complete()` is one request per call (`forge_llm.hpp:41-42`). UNVERIFIED. |
| C.6 | Explicit-state exploration | `conc` is lazy sequentialisation + Z3, not explicit state (`docs/CONCURRENCY.md`); `ltl` extracts state machines | Niche: small finite state machines only. | No existing explicit-state engine to port. Not worth it now. |
| C.7 | Mutation testing of proofs/tests (mutants x inputs) | Good if C.2 exists: kill tests are concrete executions. PRISM already has a `muttest` stage on the CPU concrete interpreter (`docs/PLAN.md:137`, `src/prism/stages/rapid.cpp`) | Large relative to CPU for many mutants (mutants x inputs is a 2-D grid). | A mutant that survives concrete tests is not shown vacuous or weak by a proof; the draft's "break the code, the proof must fail" needs the prover re-run on the mutant, which is the SMT cost again (CPU). The GPU only prefilters: mutants killed by concrete inputs need no prover run. |
| C.8 | CPU: 64-way process portfolio and per-function parallelism | Fits the actual bottleneck | Near-linear in the number of independent functions until RAM: `--jobs 32` default today; functions inside a unit run sequentially, which kernel TUs make a long tail. ESTIMATE: 4-8x on units with many functions. RAM: Z3 caps and the memory budget exist (`docs/PIR.md:1543-1563`); worst measured single functions 1.5 GB (`vector_insert`), certified checker 4.1 GB. | Memory (48 GB shared by jobs); the solver-member count per query shrinks as jobs grow (`cores/jobs`); the learned scheduler untuned for 64 threads. |
| C.9 | CPU: result caching | Fits repeated work | A whole-function verdict cache keyed on (`extra.pir_hash` = SHA-256 of the PIR text, options, engine version) is missing (CONFIRMED, `REVIEW-OF-FEEDBACK.md` item 5); the VC cache exists but only after clang+encode (`docs/SOLVERS.md:27-75`). Static inline functions repeated across units are the likely win. Gain: proportional to duplicate rate, UNVERIFIED for the mined corpus. | Cache keys must include every option that changes the answer (unwind, memory model, strict-aliasing, `--pir-drafts`, `--fp-checks`, solver versions) or a stale proof is reported. |
| C.10 | CPU: slice the translation unit to the function and its types before clang | Fits "dominated by the Clang front end" | Cuts the per-function lowering cost on big TUs. No slicer exists. | Slicing must preserve types, layout and called-function bodies or soundness claims about inlining change (`docs/PIR.md:174`). |

Ranking by gain per effort for a corpus prover on this machine: C.8 and C.9 and C.10 (CPU, no new infrastructure),
then C.2 (new, one concrete-execution engine, used both on CPU first and then GPU), then C.1 (config), C.7 (follows
C.2), C.3 (measure before investing), C.5 (needs FORGE work). C.4 and C.6: do not.

## D. What would have to be built, and in what order

D.0 Establish a baseline. Before any GPU work: run the existing CPU path on a few thousand OS-Proving functions with
`--stage inventory,classify,pir --no-llm --jobs 32` and record per-function wall time split into clang, encode, solve
(PRISM already writes `PRISM_PIR_PROGRESS` lower-start/done lines, `progress_note` in `src/prism/pir/stage.cpp`, and `PRISM_TIMING`-style
diagnostics for models, `src/prism/config.cpp:527`; `extra.solver` holds per-VC solver data). Without this table no
GPU gain can be stated. The numbers in section B come from 4-core shared machines.

D.1 WSL2 GPU access (not verified here, standard practice only). WSL2 exposes the Windows NVIDIA driver as
`/usr/lib/wsl/lib/libcuda.so`; do not install a Linux display driver inside WSL. Install a WSL-flavoured CUDA toolkit
(nvcc, cudart) matching the driver and a host gcc nvcc accepts (PRISM forces gcc as the nvcc host and refuses
`-allow-unsupported-compiler`, `CMakeLists.txt:625-660`). `nvidia-smi` and a trivial `cudaMalloc` program prove access.
Pinned toolchain fetcher: `scripts/fetch_toolchain.py cuda_nvcc` (pin CUDA 12.6.3 in `docs/DEPENDENCIES.md:17`; reconcile with 13.2).
Build on WSL ext4, not `/mnt/c` (`AGENTS.md`). Vendored Z3 compilation is slow and fills disk; reuse an existing
build tree. GPU memory: 24 GB; the Windows desktop shares the card.

D.2 Make the existing CUDA code real (cheap, bounded): build `PRISM_CUDA=ON`, run the one GPU doctest, then add a
benchmark on bit-blasted CNFs from real VCs (`prism --pir-vcs`, `prism --solve-smt2`, `prism-qa solver-bench`,
`docs/SOLVERS.md:245-285`) comparing GPU walkers, CPU walker, CaDiCaL, Kissat. Time-box. If the GPU walker does not win
on any VC class, delete or leave it unwired. If it does, wire it as one more portfolio member (`sls-gpu`): the portfolio
already validates every SAT model in Z3, so no soundness work is needed. Also measure `havoc` per call vs the CPU
path; if CPU wins, batch seeds (`nseeds > 1`) or drop the GPU call.

D.3 CPU concrete-execution engine over PIR first (the prerequisite for C.2 and C.7). `pir::interpret(fn, args,
step_limit)` and `eval_op` already exist in `src/prism/pir/pir.cpp` (`include/prism/pir.hpp:427-446`), with a concrete
memory `ConcMem` (`docs/PIR.md` "tests/cpp/test_pir_mem.cpp" paragraph) and tests that it reproduces each counterexample.
Build a driver: generate inputs (edge values per width, random, boundary of preconditions), run the function on many
inputs on 64 threads, report the first `Violation`/`AssumeFailed` as a `FAILED` finding with `extra.cex` only when the
violation is a checked property under stated preconditions (assume failures are discarded, matching how the
translation-validation step excludes them, `docs/PIR.md:1451-1458`). Output stays a refutation or silence, never `CLEAN`
as a proof (Law 3). Run it before the SMT stage; if a counterexample is found, still replay it in the SMT model for the
report if cheap, or report `FAILED` with the concrete witness. Measure: fraction of FAILED functions found by this path
and time saved. This step alone may remove most of the benefit of the GPU for refutation (ESTIMATE); decide on the GPU
only from its measured throughput.

D.4 GPU interpreter, only if D.3 shows the CPU engine is the limit. Design constraints taken from the PIR data model
(`include/prism/pir.hpp:172-346`, `274-340`):
- Bytecode: flatten `pir::Function` (vars with widths up to 64 bits, blocks of phis/stmts/terminators) into a compact
  array of fixed-size instructions (opcode, dst, 3 operand slots, width, aux) in a structure of arrays, one program per
  function, uploaded once; ~100 `Op` values (`pir.hpp:172-205`) are mostly integer ALU ops that map 1:1 to CUDA
  integer intrinsics (add/sub/mul/shift/clz/popc/bswap, overflow predicates via `__umul64hi` and carry). Floating point
  (`FAdd..FConv`, bit-exact IEEE soft-float in `src/prism/pir/fp.cpp`) and 128-bit math would need a device soft-float
  port (377 lines) or be left to the CPU path: mark such functions "GPU not supported" (Law 7: say so).
- Memory model: per-thread object table (id, size, live, kind, align) and a byte-cell store with an initialised bit,
  as in `ConcMem`. Fixed small capacity per thread (for example 64 objects, 4 KB of cells; ESTIMATE of what fits in
  local memory or a pre-allocated global pool); a function that exceeds it returns "capacity" and falls back to the CPU.
  Pointers are packed as `obj << 48 | offset` (`docs/PIR.md:215-222`), which is already a flat 64-bit value.
- Control flow: one thread per input, SIMT divergence on branches; programs are shared so instruction fetch is
  coalesced. Loops are bounded by a step limit (the CPU one is 2,000,000, `pir.hpp:445`); use a much smaller device limit
  and report `StepLimit` as "not decided".
- Equivalence: the GPU result must equal `pir::interpret` on every input (differential test on the same
  `testdata/pir` corpus and on the TV test vectors); this is the same discipline PRISM uses for the interpreter against
  `lli` (`docs/PIR.md:1449-1470`). Division by zero follows Z3's total `bvudiv` (`pir.hpp:440`).
- Host: batch size 10^5-10^6 inputs per launch per function; keep one CUDA context and persistent buffers (the current
  code allocates and frees per call); stream the results back; multiple functions can be batched into one launch by
  program offset. CPU 64 threads then feed GPU (preprocessing, clang) while it runs.
- Build: a new `src/cuda/pir_exec.cu` under the existing `prism_cuda` target, behind `PRISM_HAS_CUDA`, with a CPU
  fallback that is the D.3 engine (never a silent disable: say `GPU NOTRUN, ran on CPU` in `extra`).
D.5 C.7 mutation prefilter on top of D.3/D.4 (mutate PIR statements: replace an operator, off-by-one a constant, invert a
comparison, drop a statement; a mutant not killed by a property or assertion on the inputs is reported as a candidate
weak property, never as a proof defect). Cap and sample mutants; mutant kill rate is a new `extra` field.
D.6 AI on the GPU: set `PRISM_DEVICE=cuda` and measure token rate on a real `.fpk` (`prism ask`, `prism prove`); only if
the FORGE build has CUDA. Batched decoding needs FORGE work and is outside this repository.

Order summary: D.0 baseline, D.1 WSL GPU smoke test (hours), D.2 build and measure the existing kernels (1-2 days),
C.8/C.9/C.10 CPU levers in parallel with D.3 CPU concrete engine (days), D.4 GPU interpreter (weeks, only if D.3 shows
benefit), D.5 mutation (follow-up), D.6 config and measurement (hours; independent).

## E. Caveats and facts not to forget

- The biggest caveat: **solving is not the throughput problem for typical functions, and the heavy-tail solving cannot
  be moved to a GPU.** A GPU cannot make `crc32`, wide multiplication, memory-chain encodings or Houdini faster; at best it
  removes functions from the SMT queue by refuting them early.
- GPU results are refutations only. A GPU concrete run that finds nothing is `CLEAN` at best; it must never be merged
  with or upgraded to a proof (`docs/VERDICTS.md`, laws 2-4, the verdict audit, Lean theorems `admit_fuzzer_never_proof`).
- The CUDA kernels in the repo are untested on hardware; treat them as unreviewed until built and run. Their
  soundness posture (host re-check, Z3 validation) is correct for SAT candidates.
- GPU time is wasted if the feeding CPU stage (clang, encode) is the limit. With 64 threads and `--jobs 32`, and
  Z3 contexts using up to the memory budget, RAM (48 GB) may bind before cores do. The memory budget is per process
  tree and defaults to 75% of the smallest limit (`docs/PIR.md:1543-1563`).
- Anything quantitative here marked ESTIMATE is not evidence. The only measured GPU data point that the repository
  asks for is the output of `/tmp/prism-gpu-probsat.txt` from the existing doctest, on a random planted 3-SAT that is not
  a PRISM VC.
