#pragma once

// Internal to src/prism/solver/: a cancellable process runner and small helpers.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prism::solver::detail {

struct Proc {
    std::string out;  // stdout and stderr, merged (capped)
    int rc = -1;
    bool failed = false;     // could not start
    bool timed_out = false;
    bool cancelled = false;  // stop flag raised
    bool unreaped = false;   // killed but not reaped within 2 s: left to a detached reaper (watchdog)
    bool mem_exceeded = false;       // killed: resident memory passed mem_cap
    std::uint64_t mem_cap = 0;       // the cap that applied (bytes; 0: none)
    std::uint64_t peak_rss = 0;      // bytes, sampled (Linux /proc; 0 elsewhere)
    double secs = 0.0;
};

// posix_spawn in its own process group; the whole group is killed on timeout
// or when *stop becomes true. Never throws.
// stdin_path: the child's stdin (default /dev/null); stdout_path: the child's
// stdout goes to that file instead of `out` (stderr is still captured).
// mem_cap > 0: the child's resident memory is sampled (every ~100 ms, Linux
// /proc/<pid>/statm) and the process group is killed once it passes the cap
// (mem_exceeded; out_of_memory() then reports it).
Proc run(const std::vector<std::string>& argv, double timeout_s,
         const std::atomic<bool>* stop = nullptr, std::size_t cap = 64u << 20,
         const std::string& stdin_path = {}, const std::string& stdout_path = {},
         std::uint64_t mem_cap = 0);

// "ran out of memory (...)" when a finished process shows it (CakeML heap or
// stack exhausted, bad_alloc, "out of memory", or SIGKILL that was not ours).
std::optional<std::string> out_of_memory(const Proc& p);

std::string home_dir();
std::string read_file(const std::filesystem::path& p);
bool write_file(const std::filesystem::path& p, std::string_view data);
double now_s();

}  // namespace prism::solver::detail
