// One process-wide memory budget (Law 7: running out of memory is a reported
// UNKNOWN, never the kernel's OOM killer). docs/PIR.md "Memory budget".
#pragma once
#include "prism/export.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace prism::membudget {

// The budget in MB, fixed on first use: $PRISM_MEM_BUDGET_MB (or the older
// $PRISM_PIR_MEM_MB), else 75% of the smallest of the cgroup limit
// (memory.max / memory.limit_in_bytes, minus what the cgroup already uses),
// MemAvailable at start, and half of RLIMIT_AS (address space is not memory,
// so it only bounds, it is never the measure). 0: no budget.
PRISM_API uint64_t budget_mb();
// The computed (or environment) budget, ignoring a test override: for
// limits fixed once per process (Z3 memory_max_size).
PRISM_API uint64_t base_budget_mb();
// Where the number came from (for messages).
PRISM_API const std::string& budget_source();
// The share of one of `jobs` concurrent workers.
PRISM_API uint64_t per_job_mb(int jobs);
// Resident set of this process now (/proc/self/statm), MB; 0 if unknown.
PRISM_API uint64_t rss_mb();
// RSS at or over 80% of the budget (headroom for the stop latency), or
// system_low(). Cheap enough to call every few thousand steps.
PRISM_API bool over_budget();
// Same, but reads /proc only every `every` calls on this thread.
PRISM_API bool over_budget_sampled(unsigned every = 2048);
// Give freed heap back to the kernel (after a function's state is gone),
// so the next function's RSS check does not count the last one's garbage.
PRISM_API void release_free_memory();
// The message of a per-function UNKNOWN.
PRISM_API std::string over_message(const char* where);
// Live headroom: the box's MemAvailable under this many MB (default 800,
// $PRISM_MEM_RESERVE_MB) also counts as over budget, so PRISM gives a
// function up before the kernel OOM-kills the process (other programs on a
// shared machine grow too; the start-time budget alone cannot see them).
PRISM_API bool system_low();

// While alive, a watchdog thread (one per process, 100 ms) calls `stop`
// whenever over_budget() || system_low(): e.g. Z3_interrupt on a context in
// the middle of a query that would otherwise grow past the budget.
class PRISM_API Watch {
public:
    explicit Watch(std::function<void()> stop);
    ~Watch();
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    // the watchdog fired for this watch
    bool fired() const;
private:
    unsigned long id_;
};

// How often the watchdog has fired in this process.
PRISM_API uint64_t trips();

// For tests: override the budget (0 restores the computed one).
PRISM_API void set_budget_mb_for_test(uint64_t mb);

}  // namespace prism::membudget
