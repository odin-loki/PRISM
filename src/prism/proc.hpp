#pragma once

// Internal to prism_core: the adapter process runner (src/prism/adapters.cpp)
// for stages that live in other translation units.

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
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
// SIGKILL every registered group and session (what the signal handler does first).
void kill_child_groups() noexcept;
// Installs the SIGINT/SIGTERM/SIGHUP handler: kill the registered groups, then
// die of the same signal (default action). A signal the process ignores stays
// ignored. Called once by the prism CLI's main().
void install_child_cleanup() noexcept;

// ---- sessions (src/prism/proc_session.cpp)
//
// run_process kills the child's process group on a timeout. A child that
// starts its own children in process groups of their own (PRISM does that for
// every solver, proof checker and compiler) leaves them running when only its
// group is killed. run_session starts the child as the leader of a new
// session (setsid): every descendant stays in that session unless it calls
// setsid itself (setpgid changes only the group), so on a timeout, and on a
// SIGINT/SIGTERM/SIGHUP to this process (install_child_cleanup), the session's
// group is killed and then, on Linux, every process whose session id is the
// child's (/proc/<pid>/stat), in up to three rounds. Processes that leave the
// session (bwrap --new-session) are expected to use --die-with-parent. The QA
// tools (prism-qa) run PRISM itself through it.
struct SessionOptions {
    std::vector<std::pair<std::string, std::string>> env;  // added to / replacing the environment
    std::filesystem::path cwd;                             // empty: this process's
    const std::string* input = nullptr;                    // stdin data; null: /dev/null
    double timeout_s = 0;                                  // <= 0: no limit
    std::size_t rlimit_as_mb = 0;                          // RLIMIT_AS in the child; 0: none
};
struct SessionResult {
    std::string out, err;  // stdout and stderr, separately
    int rc = -1;           // exit status; -N when killed by signal N
    bool timed_out = false;
    bool failed = false;   // could not start (fork/pipe; an exec failure is rc 127)
    double seconds = 0;
};
SessionResult run_session(const std::vector<std::string>& argv, const SessionOptions& opt = {});
// PIDs whose session id is `sid` (Linux /proc; empty elsewhere).
std::vector<int> session_members(int sid);
// SIGKILL the session `sid` leads: its process group, then every other member
// (three rounds). Async-signal-safe (raw syscalls, no allocation).
void kill_session(int sid) noexcept;
// Sessions killed by the SIGINT/SIGTERM/SIGHUP handler, like the groups above.
void track_child_session(int sid) noexcept;
void untrack_child_session(int sid) noexcept;
void kill_child_sessions() noexcept;

// RAII registration of a child process group.
struct ChildGroup {
    int pgid;
    explicit ChildGroup(int g) noexcept : pgid(g) { track_child_group(g); }
    ~ChildGroup() { untrack_child_group(pgid); }
    ChildGroup(const ChildGroup&) = delete;
    ChildGroup& operator=(const ChildGroup&) = delete;
};

}  // namespace prism::detail
