# Memory rules for heavy PRISM runs

A PRISM run can need gigabytes: Z3 queries, the pir encoder and solver
children all grow. On a shared machine with no swap, one run that grows too
far gets the kernel's OOM killer, and the victim may be another program.
This page has two parts:

- the rules for running heavy work on such a machine (`tools/prism-run`,
  `tools/rsscap`);
- how PRISM bounds its own memory (`src/prism/membudget.cpp`), so that
  running out of memory is a reported `UNKNOWN`, never a crash (Law 7).

## Rules for a shared machine

1. Run every heavy command through `prism-run`: `prism` on a file or a tree,
   `prism-qa conformance`, `prism svcomp score`, `prism_tests`.
2. Limit build parallelism (for example `-j4`) on a shared machine.
3. Do not cap memory with `prlimit --as` (RLIMIT_AS). It limits address
   space, not memory. Sanitizer (ASan) builds reserve terabytes of address
   space, and glibc reserves a 64 MB heap arena per thread. Under a tight
   RLIMIT_AS, Z3 failed an allocation at low resident memory and crashed.
   Use `prism-run`, or set `PRISM_MEM_BUDGET_MB`.
4. To measure the peak of one command, use `rsscap MB CMD...`.
5. Install `prism-run` by copying it to a temporary file next to its target
   and then `mv` it into place. Never edit an installed copy in place: a
   running wrapper reads its script while it runs.

## `tools/prism-run`

`prism-run CMD...` runs `CMD` as a budgeted job. It is a bash script for
Linux. Jobs coordinate through `$PRISM_RUN_DIR` (default
`/tmp/prism-run-locks`), so every user of the machine must use the same
directory.

**Budget.** `PRISM_MEM_BUDGET_MB` if it is set. Otherwise 30% of
MemAvailable (or of the cgroup `memory.max`, if smaller), at most 6000 MB.
Never less than 512 MB. The wrapper exports `PRISM_MEM_BUDGET_MB` to the
job, so PRISM enforces the same number itself (next section).

**Admission.** Each call takes a ticket (time and pid) in the queue. A job
starts only when all of these hold:

- fewer than `PRISM_RUN_SLOTS` (default 4) jobs run;
- free memory is at least its budget plus `PRISM_RUN_RESERVE_MB` (default
  1536). Free memory is MemAvailable, minus the headroom the running jobs
  may still use (budget minus current RSS), minus half the RSS of large
  (over 1 GB) processes that are not under `prism-run`;
- it is the oldest ticket that fits. A ticket that has waited
  `PRISM_RUN_HOL_S` (default 600) seconds blocks every younger one, so
  memory drains for it.

A job whose budget is at most `PRISM_RUN_SMALL_MB` (default 1024) ignores
the slot count and the queue order, so small test runs do not wait behind
large ones. A budget that never fits waits forever; lower it. A waiting job
prints a `prism-run: queued ...` line every minute.

**While the job runs.** Every 0.5 s the wrapper:

- measures the RSS of the whole process tree, by parent pid, including
  children that start their own session;
- kills the tree at 1.25 × budget, or at 1 × budget when MemAvailable is
  under 1 GB. The kill is printed, never silent;
- sets `oom_score_adj` of every process in the tree to `500 × RSS / budget`
  (between 100 and 1000), so a kernel OOM kill picks the job furthest over
  its budget;
- re-registers the job if its registration file was removed.

If the wrapper is killed (timeout, ^C), it kills the job tree too. At the
end it prints the exit code and the peak tree RSS.

**No address-space cap.** `PRISM_RUN_AS=1` adds a backstop of
`prlimit --as` at 4 × budget. Never use it for ASan builds.

**What it cannot do.** It does not put jobs in their own cgroup, so the
kernel does not enforce the budget. Programs not started through it are
seen only through MemAvailable and the large-process reserve.

## `tools/rsscap`

`rsscap MB CMD...` runs `CMD` in its own process group. It kills the group
when the group's total RSS passes `MB`, and prints
`rsscap: peak_tree_rss_mb=N rc=R` at the end. Use it to profile one file.
It has no queue and no budget export.

## PRISM's own memory budget

`include/prism/membudget.hpp`, `src/prism/membudget.cpp`. Tests:
`tests/cpp/test_membudget.cpp`.

**The budget** is fixed on first use for the whole process:

- `PRISM_MEM_BUDGET_MB` (or `PRISM_PIR_MEM_MB`) when set; `0` means no
  budget;
- otherwise 75% of the smallest of: the cgroup limit minus what the cgroup
  already uses, MemAvailable at start, and half of RLIMIT_AS.

The budget is for the process tree: PRISM plus every process it started
(solver children, `prism z3-worker`). On Linux, PRISM also sets glibc's
`M_ARENA_MAX` to 2 unless `MALLOC_ARENA_MAX` is set.

**Over budget** (`over_budget()`) means any of:

- the tree's RSS is at least 80% of the budget (headroom for the stop
  latency);
- the RSS grows faster than about 1 GB/s and is at least 70% of the budget;
- MemAvailable is under `PRISM_MEM_RESERVE_MB` (default 1500; 0 turns it
  off) and the tree holds at least half the budget. Other programs grow too,
  and the start-time budget cannot see them.

**The watchdog.** While any computation is registered (`membudget::Watch`),
one thread per process polls every 50 ms, and every 20 ms from 60% of the
budget. Over budget, it calls every registered stop function (for example
`Z3_interrupt`). If the process stays over budget for `PRISM_MEM_STUCK_MS`
(default 1000 ms), it calls the stuck hook once: the running call ignores
interrupts. `PRISM_MEM_TRACE=1` prints each trip with the phase every thread
is in (`membudget::Phase`).

**What a trip means.** The function being checked ends as `UNKNOWN` with
"memory budget reached ... (process RSS, budget, source); no proof", or
"machine memory low ..." for the MemAvailable case. Freed heap is returned to
the kernel (`malloc_trim`), and later functions are checked normally. How
the pir stage uses the budget (Z3 soft and hard limits, fresh-process
re-checks) is in [PIR.md](PIR.md#memory-budget).

**Solver children** ([SOLVERS.md](SOLVERS.md)). Under a budget:

- a query with more than 1000 nodes per budget MB is `Unknown` up front;
- large queries run Z3 in a killable child (`prism z3-worker`);
- each solver child gets a resident cap from the room left in the budget.

## `prism-qa` and the budget

- `prism-qa conformance` splits the budget among its parallel runs and sets
  `PRISM_MEM_BUDGET_MB` for each. The total is `PRISM_MEM_BUDGET_MB` from
  the environment (for example from `prism-run`), else 75% of the tightest
  limit. Each run gets at least 2500 MB; `--jobs` is cut to fit. Under an
  inherited budget each run gets 3/4 of its share, because a run can
  overshoot its own budget before the stop lands and `prism-run` kills at
  1.25 ×.
- Each task run gets `PRISM_FUNCTION_BUDGET=max(120, file timeout / 4)`
  seconds unless it is set, so one slow function cannot turn a whole file
  into a timeout with no report (`src/tools/qa/support/engine.cpp`; not on
  Windows).
- `prism-qa conformance` runs `--prism PATH`, else `$PRISM_BIN`, else
  `build/prism`. Pass `--prism` when you test a different build.

## Environment variables

The `PRISM_MEM_*` variables, `PRISM_PIR_MEM_MB`, `PRISM_FUNCTION_BUDGET` and
`PRISM_Z3_WORKER` are in the generated table in
[USER_GUIDE.md](USER_GUIDE.md) ("Environment variables"). The `PRISM_RUN_*`
variables belong to the `prism-run` script only:

| variable | default | meaning |
|---|---|---|
| `PRISM_RUN_SLOTS` | 4 | jobs that may run at once |
| `PRISM_RUN_SMALL_MB` | 1024 | budgets at or under this skip the slot count and queue order |
| `PRISM_RUN_RESERVE_MB` | 1536 | free memory kept beyond a job's budget at admission |
| `PRISM_RUN_HOL_S` | 600 | seconds before the oldest waiting ticket blocks younger ones |
| `PRISM_RUN_DIR` | `/tmp/prism-run-locks` | queue and registration directory |
| `PRISM_RUN_AS` | 0 | 1: add `prlimit --as` at 4 × budget |
