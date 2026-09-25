#pragma once

// Internal to prism_core: the adapter process runner (src/prism/adapters.cpp)
// for stages that live in other translation units.

#include <filesystem>
#include <string>
#include <vector>

namespace prism::detail {

struct ProcOut {
    std::string text;  // stdout and stderr, merged
    int rc = -1;
    bool timed_out = false;
    bool failed = false;  // could not start
};

ProcOut run_process(const std::vector<std::string>& args, double timeout_s,
                    const std::filesystem::path& cwd = {});

// Child process groups (POSIX). Every runner that starts a child in a process
// group of its own (run_process, stages run_argv, solver::detail::run)
// registers the group while the child runs, so that a SIGINT / SIGTERM /
// SIGHUP to PRISM (Ctrl-C, a scorer's timeout) kills those groups too: they
// are not in PRISM's own process group and would otherwise keep running
// (CaDiCaL, cake_lpr, clang, the LRAT checkers). Async-signal-safe, lock-free;
// no-ops on Windows.
void track_child_group(int pgid) noexcept;
void untrack_child_group(int pgid) noexcept;
// SIGKILL every registered group (what the signal handler does first).
void kill_child_groups() noexcept;
// Installs the SIGINT/SIGTERM/SIGHUP handler: kill the registered groups, then
// die of the same signal (default action). A signal the process ignores stays
// ignored. Called once by the prism CLI's main().
void install_child_cleanup() noexcept;

// RAII registration of a child process group.
struct ChildGroup {
    int pgid;
    explicit ChildGroup(int g) noexcept : pgid(g) { track_child_group(g); }
    ~ChildGroup() { untrack_child_group(pgid); }
    ChildGroup(const ChildGroup&) = delete;
    ChildGroup& operator=(const ChildGroup&) = delete;
};

}  // namespace prism::detail
