# Box memory rules (15 GB RAM, no swap, shared by every worker)

1. Run every heavy PRISM command (prism on a file/dir, conformance, svcomp score,
   prism_tests) through `/workspace/bin/prism-run CMD...`:
   - at most 2 heavy runs box-wide (flock slots in /tmp/prism-run-locks; others wait),
   - exports PRISM_MEM_BUDGET_MB = 60% of MemAvailable / 2 (max 6000) -- PRISM
     (fix/memory-budget and later) ends a function as UNKNOWN past it,
   - kills the run's process tree at 1.25x budget RSS (reported, exit 137),
   - prlimit --as at 4x budget (backstop only: a tight --as crashes Z3 at low RSS),
   - oom_score_adj 800 so the kernel kills the prism run, not an agent session.
   Override: PRISM_RUN_SLOTS, PRISM_MEM_BUDGET_MB.
2. Builds: `-j4` max.
3. Do not use `prlimit --as=<small>` as a memory cap; it limits address space and
   makes Z3 fail at ~28 MB RSS (th_rewriter null deref). Use prism-run.
4. Profiling one file: `/workspace/bin/rsscap MB cmd...` (kills at MB, prints peak).

## PRISM side (branch fix/memory-budget, merge it into your branch)
- PRISM itself now enforces PRISM_MEM_BUDGET_MB (default 75% of MemAvailable at start):
  over 80% of it, or MemAvailable < 800 MB, the current function becomes UNKNOWN
  "memory budget reached ..." and later functions continue. No crash under prlimit --as=4G.
- `prism-qa conformance` now gives each run MemAvailable*0.75/jobs and cuts --jobs to fit
  (~1 GB/run). Still prefer `--jobs 2-3` on this box.

## prism-run enforces RSS, not address space (2026-10-29)
- prism-run no longer sets RLIMIT_AS: ASan builds reserve terabytes of virtual memory, so the
  6 ASan cases in prism_tests failed under the old 4x --as cap. Memory is enforced by the RSS
  watchdog (kills the tree at 1.25x budget) plus PRISM's own PRISM_MEM_BUDGET_MB.
- Opt back in for non-ASan binaries with PRISM_RUN_AS=1. Never put prlimit --as on ASan binaries.

## prism-run admission control (2026-10-29, after box-wide OOM kills)
- FIFO across workers: each call takes a ticket in /tmp/prism-run-locks/queue; only the oldest
  waiting ticket may start.
- It starts only if fewer than PRISM_RUN_SLOTS (2) jobs run and MemAvailable minus the running
  jobs' remaining headroom (budget - current RSS) >= its budget + 1.5 GB (PRISM_RUN_RESERVE_MB).
  On an idle box (no jobs) it starts anyway.
- Watchdog kills a job at 1.25x budget, or at 1x budget when MemAvailable < 1 GB.
- oom_score_adj = 500 x RSS/budget (min 100, max 1000), updated every 0.5 s: the kernel's victim
  is the job furthest over its budget, not a small run.
