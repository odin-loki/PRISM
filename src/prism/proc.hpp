#pragma once

// Internal to prism_core: the one process runner (src/prism/proc.cpp). The
// adapters (adapters.cpp run_argv) and the stages (stages/platform.cpp
// run_argv) are thin wrappers over detail::run, so stdin, an environment
// overlay, a working directory, rlimits and merged or split output are
// available to every caller the same way (the Python engine's run_binary).

#include "prism/sandbox.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace prism::detail {

struct RunSpec {
    std::vector<std::string> argv;
    std::string input;  // written to the child's stdin, which is then closed
    // When input is empty, Windows detail::run opens this for the child's stdin.
    std::filesystem::path stdin_path;
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
    // Child stdout goes to this file instead of RunOut::out (stderr is still captured).
    std::filesystem::path stdout_path;
    const std::atomic<bool>* stop = nullptr;  // when true: kill the child (RunOut::cancelled)
    std::size_t max_output_bytes = 0;       // 0: unlimited; caps captured stdout/stderr only
    // Windows only (the child and every descendant run in a Job Object that a
    // timeout or stop request terminates whole):
    // - kill_descendants: also terminate what the child left running when it
    //   exits (POSIX run_session does the same to its session);
    // - mem_limit_mb: committed-memory cap per process of the tree (the
    //   counterpart of RLIMIT_AS), 0: none. POSIX callers use `limits`.
    bool kill_descendants = false;
    long mem_limit_mb = 0;
    // - rss_cap_bytes: resident-memory watchdog, 0: none. The child's working
    //   set is sampled (~every 100 ms, the Windows counterpart of the Linux
    //   /proc/<pid>/statm watchdog in solver/util.cpp) and the Job Object is
    //   terminated once it passes the cap (RunOut::rss_exceeded). Unlike
    //   mem_limit_mb (committed memory; the child sees failed allocations)
    //   this kills a checker that is merely large. POSIX callers sample
    //   /proc themselves (solver::detail::run); this field is ignored there.
    std::uint64_t rss_cap_bytes = 0;
};

struct RunOut {
    std::string out;  // stdout (and stderr when merge_stderr)
    std::string err;  // stderr, or why the child could not start
    int rc = -1;      // exit code; -signal on POSIX
    bool timed_out = false;
    bool cancelled = false;  // stop poll requested termination
    bool failed = false;   // could not start (pipe/fork/exec/CreateProcess)
    bool crashed = false;  // killed by a signal / NTSTATUS exception, not by the timeout
    bool rss_exceeded = false;       // Windows: killed, working set passed RunSpec::rss_cap_bytes
    std::uint64_t peak_rss = 0;      // Windows: highest sampled working set (bytes; 0: not sampled)
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
// Sessions started by run_session, killed whole (every member) by the handler.
void track_child_session(int sid) noexcept;
void untrack_child_session(int sid) noexcept;
// SIGKILL the session sid leads, members in other groups included.
// Async-signal-safe (raw /proc scan on Linux).
void kill_session_now(int sid) noexcept;
// SIGKILL every registered group and session (what the signal handler does first).
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
struct SessionOptions {
    double timeout_s = 0;  // 0: no timeout
    std::filesystem::path cwd;
    std::vector<std::pair<std::string, std::string>> env;  // overlay on the child only
    std::string input;                                    // written to stdin, then closed
    long rlimit_as_mb = 0;  // RLIMIT_AS for the child (POSIX); 0: none
};

struct SessionOut {
    std::string out;   // stdout
    std::string err;   // stderr (when captured) or start failure text
    int rc = -1;       // exit status, -signal when killed by a signal
    bool timed_out = false;
    bool failed = false;  // could not start
    double seconds = 0;   // wall time until the child exited or timed out
};

SessionOut run_session(const std::vector<std::string>& args, const SessionOptions& opt = {});
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

// RAII registration of a child session.
struct ChildSession {
    int sid;
    explicit ChildSession(int s) noexcept : sid(s) { track_child_session(s); }
    ~ChildSession() { untrack_child_session(sid); }
    ChildSession(const ChildSession&) = delete;
    ChildSession& operator=(const ChildSession&) = delete;
};

}  // namespace prism::detail
