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
- conf5 (memfix binary, 2026-10-30 02:12): pir libc 176/203 proved (lm-conf5 175/202), 0 wrong.
  3 priority_queue functions "exit -9": kernel global OOM (prism 2.0 GB killed while
  retdec-decompiler held 4-6.7 GB outside prism-run), not PRISM. Others: 14 unbounded_string +
  6 unordered_map TIMEOUT, 3 sstream clang timeouts (loaded box), fs one memory-budget UNKNOWN.
- prism-run admission now reserves half the RSS of >1 GB processes outside prism-run trees.
- Registration-file loss: not reproduced (own trap/cleanup keep the file in tests); no inotify on
  the box to catch the deleter. Mitigated: wrapper re-registers every 0.5 s.

## Handoff 2026-10-30 03:55 AEDT
- c6ac46893: prism-qa run_prism prefixes `env PRISM_FUNCTION_BUDGET=max(120, timeout/4)` unless set. us2 test (budget 120): unbounded_string 0 -> 8 _true PROVED.
- conf6 running (setsid, detached): `prism-run prism-qa conformance --prism /tmp/mf/prism-fix --jobs 1 --out /tmp/mf/conf6`, log /tmp/mf/conf6.log. Check `pgrep -af prism-fix` before restarting.
- TODO: compare conf6 vs conf5 (176/203, 0 wrong); unordered_map profile; peak overshoot (~13%, conf5 3851MB/3400).

## conf6 result (2026-10-30 ~04:30 AEDT), head c6ac46893 + prism-fix
- release gate PASS, 0 wrong proofs. pir libc-models proved true 188/203 (conf5: 176), refuted 199/203.
- unbounded_string now finishes. unordered_map_contracts: all 8 functions "never reported" by harness/pir (whole file lost: per-function budget 600s x 8 > file timeout). Next: lower budget for that file or profile umap_insert_true.
- peak tree RSS 4000MB vs budget 3400 (+18%, includes prism-qa). Overshoot still open.
- Full log: /tmp/mf/conf6.keep

## Handoff 2026-10-30 06:00 AEDT (umap)
- fe579ccde: prism_hash_table.h zeroes buckets with memset (the 13-bucket loop exceeded unwind 4/8).
- Single-file qa run with it (/tmp/mf/um1, binary /tmp/mf/prism-umap): unordered_map_contracts still
  "missing" (all 8 x 3 stages, 900 s file timeout). Not fixed yet. Profiling pir alone with
  PRISM_FUNCTION_BUDGET=60: /tmp/mf/up1.log.
- cxx_unordered_map_{true,false} pir: memory-budget UNKNOWN at 432/579 MB RSS of 2550 (false trip? same
  pattern as vector_insert 765 MB in conf4): investigate.

## Handoff 2026-10-30 07:00 AEDT (umap, cont.)
- c3a008af8: rehash policy takes an exact integer path at max_load_factor 1.0 (n < 2^53, bit-identical
  to libstdc++'s double math); clear() memsets. Binary frozen at /tmp/mf/prism-umap2.
- Still no answer: unordered_map_contracts never answered in any earlier conformance run either
  (libc-conf-full, lm-conf..lm-conf5), so this is not a memfix regression.
- Minimal repro /tmp/mf/t1.cpp (one insert of symbolic int, assert .second): 8 loops, Z3 memory blows past
  1 GB within ~60 s (budget-UNKNOWN, no wrong verdict). Equivalent C shape (/tmp/mf/t2.c: 13 buckets,
  h % 13, chain walk, malloc node) encodes in 1 s at 148 MB, so the cost is in the C++ layer
  (operator new/bad_alloc landing pads, pair/node construction, _M_owner iterator bookkeeping): bisect
  those next by trimming the model (each build-mf2 rebuild ~20 min under load).
- prism-run queue: full of 4 svcomp/astlint jobs for hours; budget <= 1000 MB bypasses it for profiling.

## Handoff 2026-10-30 ~08:30 AEDT (umap ablation, no rebuild needed)
- Ablation harness: copy the model headers to /tmp/mf/abl/<v>/ and include them by absolute path from the
  test (the include guards keep the embedded model out). Each variant runs in about 1 min at a 1 GB budget,
  which skips the prism-run queue. Kept in /workspace/logs/umap-abl (run.sh, mkh.sh, h8, h9).
- Single insert (/tmp/mf/abl/v3): base UNKNOWN (Z3 > 1 GB). Variants changed one thing each:
  without _M_prepare_insert PROVED 10 s; without find_node, iter-link or the destructor still UNKNOWN; % 13
  fast path still UNKNOWN; __builtin_trap before the prime-table slow path PROVED 38 s.
  => CAUSE: _M_next_bkt's std::lower_bound over the 304-entry prime table (symbolic index into a
  constant array). It is only taken for n >= 14, but n arrives through memory, so it is always encoded.
- h8 (297-way if-chain of constant compares): one insert PROVED 7-48 s, but the full file hits
  "unrolling exceeds 6000 block instances". h9 (straight-line masks, no branches): one insert PROVED
  111 s, but the full file runs out of budget at 1 GB. Neither is committed yet. Both match the original
  on n in [0, 2e5], 2^k +- 3 and every prime +- 1, including _M_next_resize (eq_*.cpp).
- Next: test h9 at the real budget (3400) once a prism-run slot frees; or cut the number of _M_next_bkt
  call sites (prepare_insert re-calls it on a prime it already returned; do_rehash can skip it when
  bkt >= bkt_for_elements(n+1)); or two levels (branch on about 18 coarse ranges, masks within one).

## Handoff 2026-10-30 ~10:00 AEDT (after box restart)
- /workspace/bin/prism-run was already restored and is byte-identical to tools/prism-run (1e9e53bcd).
  Smoke test passes. To reinstall: cp tools/prism-run /workspace/bin/.prism-run.new && mv it into place
  (atomic; never edit in place).
- build-mf2's cache points at the removed /opt paths (libomp): use build/ (Release, PRISM_Z3=ON). An
  incremental `cmake --build build --target prism` takes about 10 s.
- 2c73112db: h10 is in the model. tools/models/gen_prime_lb.py regenerates it (idempotent);
  tools/models/check_prime_lb.sh <orig> <new> checks equality (201360 inputs IDENTICAL).
- Running: full unordered_map_contracts pir at 3000 MB, binary /tmp/mf/prism-h10, log /tmp/mf/u3.log.
