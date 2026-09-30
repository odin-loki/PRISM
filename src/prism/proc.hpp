#pragma once

// Internal to prism_core: the one process runner (src/prism/proc.cpp). The
// adapters (adapters.cpp run_argv) and the stages (stages/platform.cpp
// run_argv) are thin wrappers over detail::run, so stdin, an environment
// overlay, a working directory, rlimits and merged or split output are
// available to every caller the same way (the Python engine's run_binary).

#include "prism/sandbox.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace prism::detail {

struct RunSpec {
    std::vector<std::string> argv;
    std::string input;  // written to the child's stdin, which is then closed
    // Set on the child only, never on PRISM's own environment (other --jobs
    // threads and later children must not see it). Replaces a variable of
    // the same name.
    std::vector<std::pair<std::string, std::string>> env;
    std::filesystem::path cwd;  // empty: PRISM's cwd
    double timeout_s = 60.0;
    // rlimits applied in the forked child (Law 9 sandbox for built binaries;
    // the caller wraps argv with sandbox::wrap_argv). Default: none.
    sandbox::Limits limits;
    bool merge_stderr = false;  // one pipe for stdout and stderr (order kept)
};

struct RunOut {
    std::string out;  // stdout (and stderr when merge_stderr)
    std::string err;  // stderr, or why the child could not start
    int rc = -1;      // exit code; -signal on POSIX
    bool timed_out = false;
    bool failed = false;   // could not start (pipe/fork/exec/CreateProcess)
    bool crashed = false;  // killed by a signal / NTSTATUS exception, not by the timeout
};

// Law 8: throws std::runtime_error on a --no-*-check flag. detail::run calls
// it first, so no caller (adapters, stages, the polyglot table, pir, the AI
// tools) can start a child with a check silently disabled.
void refuse_disabled_checks(const std::vector<std::string>& argv);

// Throws on a --no-*-check flag (refuse_disabled_checks). An argv[0] that
// cannot be executed (missing, no execute bit) or a cwd that cannot be entered
// is failed = true, rc = 127, err = why (the same as CreateProcess failing on
// Windows), not an exit code of the tool.
RunOut run(const RunSpec& spec);

struct ProcOut {
    std::string text;  // stdout and stderr, merged
    int rc = -1;
    bool timed_out = false;
    bool failed = false;  // could not start
};

ProcOut run_process(const std::vector<std::string>& args, double timeout_s,
                    const std::filesystem::path& cwd = {});

// Child process groups (POSIX). Every runner that starts a child in a process
// group of its own (detail::run, solver::detail::run) registers the group
// while the child runs, so that a SIGINT / SIGTERM /
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

// Run argv in a session of its own (setsid; POSIX), stdin /dev/null,
// stdout captured, stderr discarded. On the timeout (seconds; <= 0: none)
// the whole session is killed: its process group, then every process whose
// session id is the child's (Linux /proc), so solver and compiler children
// that PRISM put in process groups of their own die too. A descendant that
// leaves the session (bwrap --new-session) must use --die-with-parent.
// The scorers (`prism svcomp score`) run one task per session.
struct SessionOut {
    std::string out;   // stdout
    int rc = -1;       // exit status, -signal when killed by a signal
    bool timed_out = false;
    bool failed = false;  // could not start
};
SessionOut run_session(const std::vector<std::string>& args, double timeout_s,
                       const std::filesystem::path& cwd = {});
// PIDs whose session id is sid (Linux /proc; empty elsewhere).
std::vector<int> session_members(int sid);
// SIGKILL the session sid leads: its process group, then every other member.
void kill_session(int sid) noexcept;

// RAII registration of a child process group.
struct ChildGroup {
    int pgid;
    explicit ChildGroup(int g) noexcept : pgid(g) { track_child_group(g); }
    ~ChildGroup() { untrack_child_group(pgid); }
    ChildGroup(const ChildGroup&) = delete;
    ChildGroup& operator=(const ChildGroup&) = delete;
};

}  // namespace prism::detail
