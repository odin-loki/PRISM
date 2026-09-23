#pragma once

// Law 9: executing code from the scanned tree requires --allow-exec.
// Twin of the Python engine prism/sandbox.py: the exec policy, the NOTRUN
// row a held-back step writes, the bubblewrap/rlimit sandbox for built
// binaries, and the Windows command-line quoting the process runners use.

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::sandbox {

inline constexpr const char* EXEC_FLAG = "--allow-exec";
inline constexpr const char* EXEC_INSTALL = "re-run with --allow-exec (only on code you trust)";
inline constexpr const char* EXEC_REASON = "executes-scanned-code";

// rlimits for a sandboxed child (same numbers as prism/sandbox.py).
inline constexpr unsigned long long LIMIT_AS_BYTES = 2ULL * 1024 * 1024 * 1024;
inline constexpr unsigned long long LIMIT_NOFILE = 256;
inline constexpr unsigned long long LIMIT_FSIZE_BYTES = 64ULL * 1024 * 1024;

// "<what>: executes code from the scanned tree; re-run with --allow-exec (...)"
PRISM_API std::string exec_message(std::string_view what);
// NOTRUN with extra.install = EXEC_INSTALL and extra.reason = EXEC_REASON.
PRISM_API Finding exec_notrun(std::string stage, std::string_view what,
                              std::map<std::string, std::string> extra = {});

// Process-wide policy for stages that get no Config (fuzz, diff, afl,
// libFuzzer, sandbox_run). run_pipeline holds a Policy for the run.
PRISM_API bool allowed();
PRISM_API bool set_allowed(bool value);  // returns the previous value
struct Policy {
    bool prev;
    explicit Policy(bool allow) : prev(set_allowed(allow)) {}
    ~Policy() { set_allowed(prev); }
    Policy(const Policy&) = delete;
    Policy& operator=(const Policy&) = delete;
};

// "bwrap" (Linux, bwrap on PATH and a probe jail starts), "rlimits-only"
// (other POSIX) or "none" (Windows).
PRISM_API std::string kind();
// The jail argv for one run (pure; bwrap is the resolved bwrap path).
PRISM_API std::vector<std::string> bwrap_argv(const std::string& bwrap,
                                              const std::vector<std::string>& argv,
                                              const std::filesystem::path& scratch);
// argv inside the jail when bwrap works here, else unchanged.
PRISM_API std::vector<std::string> wrap_argv(const std::vector<std::string>& argv,
                                             const std::filesystem::path& scratch);

struct Limits {
    bool enabled = false;
    double cpu_seconds = 0.0;  // wall-clock timeout of the run; CPU cap is +1s
    bool limit_as = true;      // false for ASan/TSan builds (shadow memory)
};
PRISM_API Limits limits_for(double timeout_s, bool limit_as = true);
// setrlimit in the forked child before execvp (POSIX; async-signal-safe).
// No-op on Windows or when !enabled.
PRISM_API void apply_child_limits(const Limits& limits) noexcept;

// --- Windows command lines (pure; compiled and tested on every platform) ---
// One argument quoted for CommandLineToArgvW / the MSVC CRT: quotes are
// \"-escaped, backslashes before a quote or the closing quote are doubled.
// Wrapped in quotes when it holds space/tab/newline/quote, is empty, or
// force is set.
PRISM_API std::string quote_windows_arg(std::string_view arg, bool force = false);
PRISM_API std::string windows_command_line(const std::vector<std::string>& args);
// .bat/.cmd run through cmd.exe even under CreateProcess (metacharacters
// and %var% are live there).
PRISM_API bool is_batch_file(std::string_view path);
// cmd.exe /d /s /c "<every arg quoted>" for a batch target, or nullopt when
// an argument holds a character cmd.exe expands inside quotes (% ! " CR LF):
// refuse rather than guess.
PRISM_API std::optional<std::string> batch_command_line(const std::vector<std::string>& args);

}  // namespace prism::sandbox
