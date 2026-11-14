# Work log

What was done, decided and measured, newest first. Plans live in [UPGRADE_PLAN.md](UPGRADE_PLAN.md),
[GPU_PLAN.md](GPU_PLAN.md) and [PARALLEL_SOLVING_PLAN.md](PARALLEL_SOLVING_PLAN.md); this file records what
actually happened.

## 2026-11-13: GPU proving, any-AI providers, solver profiling

Input: the OS-Proving feedback merged in PR #45 (`docs/feedback/`), the owner's requests: use the GPU (a
~20,000-core target; the machine has an RTX 3090 with 10,496 cores) for much faster and more extensive
proving, use FORGE as much as possible, FORGE as the default AI with any other AI possible, a parallel SMT /
CDCL plan, profile and optimise Z3 and make it parallel (editing `third_party/z3` is authorised), and put
everything possible on the GPU. Goal stated by the owner: prove the Linux kernel.

### Built

- **PIR bytecode VM and input sweep (GPU_PLAN G1).** `include/prism/pir_vm.hpp`, `src/prism/pir/vm.cpp`,
  `src/prism/pir/vm_core.inc` (the one VM source for host and device), `src/cuda/pir_vm.cu` (kernel),
  `cmake/embed_ptx.cmake`, CMake block `PRISM_GPU_PTX`. The kernel is PTX from clang's nvptx64 target
  (`--cuda-device-only -nocudainc -nocudalib`), embedded in the binary and JIT-compiled by the driver; the
  driver is loaded at run time through FORGE `dl`; device memory is charged to FORGE's governor pool
  `cuda:0`. CPU twin on FORGE's task pool. Hooked into the pir stage before `check_function`
  (`sweep_function` in `src/prism/pir/stage.cpp`): a violation replayed in `pir::interpret` is FAILED and
  skips the solver; otherwise `extra.sweep` records coverage (never a proof). Settings `PRISM_GPU`,
  `PRISM_GPU_EXHAUSTIVE_BITS`, `PRISM_GPU_SAMPLES`, `PRISM_GPU_BUDGET_MS`, `PRISM_GPU_CROSSCHECK`.
  Docs: `docs/PIR.md` "Input sweep on the bytecode VM".
- **AI providers (FORGE default, any AI).** `include/prism/ai_providers.hpp`, `src/prism/ai/providers.cpp`:
  `PRISM_AI` = forge | openai | ollama | vllm | llamacpp | lmstudio | anthropic | cmd, wired into
  `ai::connect_backend`, the stage LLM engine (`src/prism/stages/llm.cpp`) and the Lean prover
  (`resolve_prover`). HTTP through curl with the key on stdin. Docs: `docs/AI.md` "Providers".
  An OpenAI-compatible client layer inside FORGE itself is being written in the FORGE repository in a
  separate session; PRISM will move onto it when it lands.
- **Profiler.** `PRISM_PROFILE=1` (or a path): inclusive wall time per `membudget::Phase` at exit. New phases
  in the portfolio: features, normalize (cache key), cache load, work dir, new z3 context, translate.
  `membudget::Phase` no longer takes a global mutex and a map insert per call (thread-local slots).
- **Plans.** `docs/GPU_PLAN.md` (GPU and FORGE, stage-by-stage GPU table), `docs/PARALLEL_SOLVING_PLAN.md`
  (from a literature survey of parallel SAT/SMT; sources inline).

### Measured

- `prism_tests -tc='pir vm*,ai providers*,*env*'`: 225,208 of 225,210 assertions passed on the first run;
  the two failures were a test bug (the CPU twin's 20-bit exhaustive default against a 24-bit program),
  fixed. Every integer operator agrees with `pir::eval_op` and `pir::interpret` at widths 1, 8, 13, 16, 32,
  33, 63 and 64 on edge and random inputs; a loop with phis agrees with the interpreter on all 256 inputs;
  the GPU kernel equals the CPU twin lane for lane.
- RTX 3090 (WSL2, driver 610.88): a 24-bit-input scalar function, all 2^24 inputs, 0.003 s on the GPU.

### Profile of the pir stage (perf, conformance corpus, `--jobs 32`, sweep off)

`perf record` on the running `prism` (184k samples flat; a 12 s DWARF call-graph sample). Shares of CPU:

| Where | Self | Inclusive | Finding |
|---|---|---|---|
| `solver::features(expr)` | ~5% | **25.9%** | the scheduler's feature walk: a `z3::expr` wrapper per node and edge (inc_ref, dec_ref, error check), `arg(i)` twice per edge, `std::unordered_set` dedup; and it ran twice per query (again in `not_certifiable_reason`) |
| `forge::task::pool::run_` | 12.5 to 17% | same | FORGE pool workers spinning `FORGE_SPIN_US` (200 us) after each job: burnt cores, not work |
| encoder memory model (`SymMem::read_cell_log`, `havoc_read`, `mk_app`, AST hash tables) | | 18% | building read-over-write chains; Z3 `chashtable<ast*>` insert/expand |
| Houdini's own Z3 queries | | 16% | on Z3's SMT core (`smt::context::bcp`, relevancy, congruence closure hash tables, `theory_bv`): incremental queries on `z3::solver(c)` without a logic |
| portfolio Z3 member SAT search | | 16.6% | already bit-blast + SAT (`tactic2solver` → `sat_tactic`) |
| `pir::analyze(Function)` | 10% | | dominators as a dense n x n matrix iterated to a fixed point, inner loop per predecessor, `.at()` on every element |

### Optimised (measured below)

- `solver::features` rewritten on the raw C API (no ref counting; the formula keeps its subterms alive),
  visited set a bitmap over AST ids, edges pushed once; the portfolio computes features once per query and
  `not_certifiable_reason(const Features&)` reuses them.
- `FORGE_SPIN_US` defaults to 20 us in `prism` scans (kept if the user sets it; not for `prism prove`).
- Houdini's incremental solvers are created with logic `QF_BV` when the base assertions are pure QF_BV, so Z3
  uses `inc_sat_solver` (incremental bit-blasting into CDCL, learned clauses kept across assumption checks)
  instead of the SMT core (`incremental_solver` in `src/prism/pir/encode.cpp`; `PRISM_Z3_INCREMENTAL=smt`
  restores the old solver).
- Functions of one unit are checked in parallel (`run_function_checks` in `src/prism/pir/stage.cpp`,
  `PRISM_PIR_FN_JOBS`, default 8 per unit, at most the hardware threads across units); translation and the
  folded-UB bookkeeping stay in order.
- `membudget::Phase` uses thread-local slots (no global mutex per phase) and times phases under
  `PRISM_PROFILE`; new phases `pir: houdini`, portfolio features / normalize / cache load / work dir / new z3
  context / translate.

### GPU SAT member (subagent)

`include/prism/cuda_driver.hpp`, `src/prism/cuda_driver.cpp` (generic CUDA driver loader on FORGE `dl`,
buffers leased from FORGE pool `cuda:0`, streams, PTX module cache), `src/cuda/probsat.cu` rewritten for the
clang-PTX path (9 B per clause per walker instead of 12, no 64-literal cap, one walker per warp, launches of
about 30 ms), `src/prism/solver/gpu_sls.cpp`, `tests/cpp/test_gpu_sls.cpp` (3 cases pass on the 3090).
Measured on planted 3-SAT against one CPU walker thread (busy machine): GPU total throughput 50 to 260 M
flips/s (25 to 140x one thread), but each walker is about 10x slower than a CPU core, so time to solution is
mixed: GPU faster on one 10k-variable instance (3.7 s vs 9.6 s), slower on another (9.7 s vs 3.1 s) and at 100k
variables (22 s vs 6.6 s). Not yet measured on real bit-blasted VCs; per GPU_PLAN G7 it is wired into the
portfolio only if it wins a VC class.

### Decided

- Z3's CDCL/SMT search stays on the CPU: the survey found no GPU method that beats CPU CDCL on UNSAT
  queries, and most VCs are UNSAT. The GPU takes the satisfiable side (local search on the bit-blasted CNF,
  models validated in Z3), concrete execution, invariant pruning and mutation filtering.
- A sweep result is never a proof. Exhaustion becomes a proof origin only after a Lean model of the VM and a
  lattice extension (GPU_PLAN G2).

### Problems found

- Every WSL build tree pointed at a deleted pinned clang (`~/.cache/prism/tools/llvm-23.1.4`); the pinned
  clang 23.1.4 exists at `~/opt/llvm23`. FORGE main refuses clang 18.
- `scripts/wsl_sync_from_windows.sh` ran `rsync --delete` excluding only `build`, `build-wsl` and
  `build-werror`, so any other `build-*` tree in `~/PRISM` was deleted on the next sync. Fixed: `build-*` is
  excluded.
- WSL has no `perf`; Z3 internals are sampled with gdb stacks instead.
