#pragma once

// PRISM's parallelism, all on FORGE (support/task/):
//
//   parallel_for / for_each_index  a data-parallel loop on forge::task::pool.
//       One process-wide pool per worker count (prism::clamp_jobs); the caller
//       participates; a call nested inside another one runs inline.
//   TaskGraph                      a task graph on forge::task::scheduler
//       (work stealing). Tasks may spawn tasks and may run after other tasks
//       (spawn_after), so a unit can fan out into one task per function and
//       join into a finishing task without any thread blocking on a nested
//       loop. wait() helps: the waiting thread runs tasks too.

#include "prism/export.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace prism::pool {

using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;

// f(begin, end) over [0, n) in claims of at least `grain` items.
PRISM_API void parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn);
// The same with every claim clamped to [lo, hi] items (the last may be shorter).
PRISM_API void parallel_for(int jobs, std::size_t n, std::size_t lo, std::size_t hi, const RangeFn& fn);
PRISM_API unsigned size();

// One call per index, each claim exactly one index: items here are coarse
// (a file, a unit, a tool run) and uneven, so a long claim would hold
// finished-early workers idle behind one slow item.
inline void for_each_index(int jobs, std::size_t n, const std::function<void(std::size_t)>& fn) {
    parallel_for(jobs, n, 1, 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) fn(i);
    });
}

enum class Priority { Low, Normal, High };

class PRISM_API TaskGraph {
public:
    // A handle to a spawned task (copies share it). Default: no task.
    class Task {
    public:
        Task() = default;
        bool valid() const { return h_ != nullptr; }
        bool done() const;

    private:
        friend class TaskGraph;
        std::shared_ptr<void> h_;
    };

    // jobs as for --jobs (prism::clamp_jobs): that many threads run tasks,
    // the thread in wait() included.
    explicit TaskGraph(int jobs);
    ~TaskGraph();  // waits for every task (errors are dropped; call wait() to see them)
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // Safe to call from any thread, inside a task too.
    Task spawn(std::function<void()> fn, Priority p = Priority::Normal);
    // fn runs once every task in deps has finished (failed ones included).
    Task spawn_after(const std::vector<Task>& deps, std::function<void()> fn, Priority p = Priority::Normal);
    // Blocks until every task spawned so far, and every task they spawn, has
    // finished, running tasks on this thread meanwhile. Rethrows the first
    // exception a task threw. Not for use inside a task.
    void wait();
    unsigned threads() const;  // threads that run tasks, the waiting one included

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace prism::pool
