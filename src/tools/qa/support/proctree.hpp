#pragma once

// Run a command so that a timeout or Ctrl-C kills its whole process tree.
//
// PRISM starts solvers, proof checkers and compilers (CaDiCaL, cake_lpr,
// clang, the LRAT checkers), each in a process group of its own so that
// PRISM can kill it; killing PRISM alone leaves them running, still using CPU
// and memory, after the scorer has moved on.
//
// run_tree starts the command in a new session (setsid). Every descendant
// stays in that session unless it calls setsid itself (setpgid changes only
// the process group), so on a timeout, or on SIGINT/SIGTERM/SIGHUP to the
// scorer (install_interrupt_cleanup), kill_session sends SIGKILL to the
// session's process group and to every process whose session id is the
// command's (/proc/<pid>/stat). Processes that leave the session
// (bwrap --new-session) are expected to use --die-with-parent.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa {

struct RunOpts {
    double timeout_s = 0;  // 0: no timeout
    std::filesystem::path cwd;
    std::vector<std::pair<std::string, std::string>> env;  // set on top of the scorer's environment
    long mem_limit_mb = 0;  // RLIMIT_AS for the command (inherited by its children); 0: none
};

struct RunResult {
    int rc = -1;              // exit status, or -signal when killed by a signal
    std::string out;
    std::string err;
    bool timed_out = false;
    bool start_failed = false;  // fork/exec failed (errno text in err)
};

RunResult run_tree(const std::vector<std::string>& argv, const RunOpts& opts = {});

// PIDs whose session id is sid (Linux /proc; empty elsewhere).
std::vector<int> session_members(int sid);
// SIGKILL the session `sid` leads: its process group, then every other member.
void kill_session(int sid) noexcept;
// SIGINT/SIGTERM/SIGHUP: kill every running session, then die of the signal.
void install_interrupt_cleanup() noexcept;

// PATH lookup (shutil.which); "" when absent.
std::string which(const std::string& name);

}  // namespace prism::qa
