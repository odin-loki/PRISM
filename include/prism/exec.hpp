#pragma once

// PRISM's process runner: a thin interface over FORGE's
// forge::os::spawn_wait (os/process.hpp). Spec and Result map field for field
// onto spawn_spec / proc_limits / proc_result; failed (could not start) is
// FORGE's result<> error branch, with the reason in err.

#include "prism/export.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prism::exec {

struct Spec {
    std::vector<std::string> argv;
    std::string cwd;                             // empty: inherit
    std::map<std::string, std::string> env_set;  // added/overridden variables
    std::vector<std::string> env_unset;
    std::optional<std::string> stdin_data;       // nullopt: empty stdin
    std::uint64_t timeout_ms = 0;                // 0: wait forever; expiry kills the tree
    std::uint64_t max_output = 64ull << 20;      // per stream; the rest is dropped, truncated = true
    std::uint64_t memory_bytes = 0;              // address-space cap; 0: none
    std::uint64_t cpu_seconds = 0;               // 0: none
    std::uint64_t open_files = 0;                // 0: inherit
    // Planned FORGE proc_limits fields (prism-forge session is adding them to
    // forge::os::proc_limits, clamped to the hard limit). Until they reach FORGE
    // main, forge_services.cpp cannot pass them on and they are ignored.
    std::uint64_t file_size_bytes = 0;           // RLIMIT_FSIZE (POSIX); 0: inherit
    bool no_core = false;                        // RLIMIT_CORE = 0 (POSIX)
    bool merge_stderr = false;                   // stderr goes into out (order kept)
    // File redirects (empty: unused). stdin_file wins over stdin_data; stdout_file /
    // stderr_file win over capture (merge_stderr sends stderr to stdout_file).
    std::string stdin_file, stdout_file, stderr_file;
    bool append_output = false;                  // append to the redirect files instead of truncating
    const std::atomic<bool>* stop = nullptr;     // true: kill the tree, Result::cancelled
    bool new_session = false;                    // POSIX setsid / Windows new process group
    std::uint64_t watch_memory_bytes = 0;        // kill the tree past this much memory (Result::memory_killed)
};

struct Result {
    int exit_code = -1;  // 128+signal when killed by a signal (FORGE/POSIX convention)
    int term_signal = 0;
    bool timed_out = false;
    bool truncated = false;
    bool failed = false;  // could not start; err says why
    bool cancelled = false;      // Spec::stop or an interrupt of PRISM killed it
    bool memory_killed = false;  // Spec::watch_memory_bytes passed
    bool unreaped = false;       // killed but not reaped within 2 s; FORGE left it to a detached reaper
    std::uint64_t peak_rss_bytes = 0;  // child peak resident set (rusage / job accounting); 0: unknown
    bool crashed() const { return term_signal != 0 && !timed_out && !cancelled && !memory_killed; }
    // PRISM's rc convention: 127 when it could not start, -signal when killed, else the exit code.
    int rc() const { return failed ? 127 : term_signal ? -term_signal : exit_code; }
    std::string out, err;
    std::uint64_t wall_ms = 0;
};

// Law 8: throws std::runtime_error on a --no-*-check flag, before starting anything.
PRISM_API void refuse_disabled_checks(const std::vector<std::string>& argv);

// Throws via refuse_disabled_checks; otherwise never throws.
PRISM_API Result run(const Spec& spec);

// stdout and stderr merged, empty stdin; rc as Result::rc().
struct ProcOut {
    std::string text;
    int rc = -1;
    bool timed_out = false;
    bool failed = false;  // could not start
};
PRISM_API ProcOut run_process(const std::vector<std::string>& args, double timeout_s,
                              const std::filesystem::path& cwd = {});

// A command in a session of its own (POSIX setsid), stdout and stderr captured
// separately; the whole tree is killed on the timeout (seconds; <= 0: none).
struct SessionOptions {
    double timeout_s = 0;
    std::filesystem::path cwd;
    std::vector<std::pair<std::string, std::string>> env;  // overlay on the child only
    std::string input;                                    // written to stdin, then closed
    long rlimit_as_mb = 0;                                // address-space cap; 0: none
};
struct SessionOut {
    std::string out, err;
    int rc = -1;  // exit status, -signal when killed by a signal, 127 when it could not start,
                  // -1 when a timeout ended it without a signal (Windows)
    bool timed_out = false;
    bool failed = false;
    double seconds = 0;
};
PRISM_API SessionOut run_session(const std::vector<std::string>& args, const SessionOptions& opt = {});
PRISM_API SessionOut run_session(const std::vector<std::string>& args, double timeout_s,
                                 const std::filesystem::path& cwd = {});

// SIGINT / SIGTERM (and SIGHUP on POSIX) to PRISM: every running exec::run is
// cancelled (its process tree killed), then PRISM dies of the same signal.
// A signal the process ignores stays ignored. Call once from main().
PRISM_API void install_interrupt_cleanup() noexcept;

// The Law 9 sandbox caps (sandbox::Limits) as Spec fields: CPU = timeout + 1 s,
// open files, file size, no core, address space unless !limit_as. No-op when
// !enabled. Timeout is not touched.
PRISM_API void apply_limits(Spec& spec, bool enabled, double cpu_seconds, bool limit_as);

}  // namespace prism::exec
