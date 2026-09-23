#pragma once

// Internal to src/prism/solver/: a cancellable process runner and small helpers.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace prism::solver::detail {

struct Proc {
    std::string out;  // stdout and stderr, merged (capped)
    int rc = -1;
    bool failed = false;     // could not start
    bool timed_out = false;
    bool cancelled = false;  // stop flag raised
    double secs = 0.0;
};

// posix_spawn in its own process group; the whole group is killed on timeout
// or when *stop becomes true. Never throws.
// stdin_path: the child's stdin (default /dev/null); stdout_path: the child's
// stdout goes to that file instead of `out` (stderr is still captured).
Proc run(const std::vector<std::string>& argv, double timeout_s,
         const std::atomic<bool>* stop = nullptr, std::size_t cap = 64u << 20,
         const std::string& stdin_path = {}, const std::string& stdout_path = {});

std::string home_dir();
std::string read_file(const std::filesystem::path& p);
bool write_file(const std::filesystem::path& p, std::string_view data);
double now_s();

}  // namespace prism::solver::detail
