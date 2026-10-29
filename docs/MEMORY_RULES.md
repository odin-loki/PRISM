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
- It starts only if fewer than PRISM_RUN_SLOTS (4) jobs run (jobs with budget <= 1 GB skip the
  count and the queue order) and MemAvailable minus the running
  jobs' remaining headroom (budget - current RSS) >= its budget + 1.5 GB (PRISM_RUN_RESERVE_MB).
  Always, even with no other job registered (fixed 2026-10-29 late). Install prism-run only via temp file + mv.
- Watchdog kills a job at 1.25x budget, or at 1x budget when MemAvailable < 1 GB.
- RSS, kills and oom_score_adj cover all descendants by parent pid, including children that
  setsid into new sessions (svcomp score).
- oom_score_adj = 500 x RSS/budget (min 100, max 1000), updated every 0.5 s: the kernel's victim
  is the job furthest over its budget, not a small run.

## Handoff (fix/memory-budget, 2026-10-29 late)
- Done: FIFO+admission prism-run (strict MemAvailable >= budget+1.5 GB), descendant tracking,
  oom_score_adj by RSS/budget; Z3 hard limit = budget (soft 80%); low-box stop only at >= half
  budget; prism-qa conformance: no default --as cap, 2.5 GB floor, inherited budget split among
  --jobs at 3/4 share.
- Not possible: per-job cgroup (agent cgroup root-owned with processes; no user systemd).
- prism-run admission is first fit in ticket order; a ticket waiting > 600 s gets priority.
  Wrappers started before 72be5de8d keep the old strict-FIFO loop: restart queued ones.
- Watchdog polls 20 ms from 60% of budget. Z3 hard limit stays 1.5x (1.0x segfaulted Z3 at 2.8 GB).
  Peaks at budget 3000 (single file): priority_queue_contracts 3343 MB (was 3609; UNKNOWNs, no kill),
  vector_insert 208 MB (correct verdicts), unbounded_string: 900 s timeout, cxx_priority_queue_true:
  segfault rc 139 under the 1.0x Z3 limit (re-measure with 1.5x).
- Open: overshoot now ~11%; libc conformance
  conf4 (/tmp/mf/conf4, log /workspace/logs/memfix-conf4.log, PRISM_MEM_BUDGET_MB=3400 --jobs 1)
  vs lm-conf5 175/202, 0 wrong. Peaks for unbounded_string / vector_insert not yet measured.
  test_pir_refinement "memory checks in XTranslate.lean" fails after the libc-models merge (not memfix).

- conf4 (build-mf2, PRISM_MEM_BUDGET_MB=3400 --jobs 1, 2026-10-30 ~00:55): 172/202, 0 wrong, no
  process killed (old: 26 exit -9). Peak tree 3938 MB. vs lm-conf5 175: lm-only regex_owns,
  stacktrace_current_bound, typeindex_same_type (UNENCODED here: check whether lm-conf5's build had
  uncommitted code) and vector_insert_true (memory budget at 765 MB RSS of 2550 after 232 s, though
  standalone it proves in 5 s at 208 MB: investigate the trip). memfix-only: priority_queue_push_true.
  Remaining misses: 14 unbounded_string + 6 unordered_map TIMEOUT (300 s file limit).
- prism-run: up to 4 jobs; budget <= 1 GB bypasses slots and queue order.
- CORRECTION: conf2-conf4 ran build/prism (old, 20:07 Oct 29), not build-mf2/prism: prism-qa
  conformance defaults to build/prism. Always pass --prism <build>/prism. conf5 (/tmp/mf/conf5,
  log /workspace/logs/memfix-conf5.log) is the first run on the memfix binary.
- 2026-10-30 ~01:50: cxx_priority_queue_true at 1.5x Z3 limit, budget 3400: rc 0, peak 3451 MB.
  libc commits 1c507b08b/c8345e526 are in this branch and in build-mf2/prism; conf4 missed them only
  because it ran build/prism. conf5 (memfix binary) running: check
  grep '| pir | libc' /tmp/mf/conf5/metrics.md (target >= 175, 0 wrong, no exit -9).
- Box OOM kills seen while conf5 ran come from outside PRISM too: retdec-decompiler
  (/workspace/rd-core-build, ~4.2 GB RSS) runs without prism-run.
