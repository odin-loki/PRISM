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

// --allow-exec without an isolating sandbox (no working bwrap) is held back:
// the exec steps are NOTRUN with this hint, unless the user opted in to the
// weaker mode with --allow-exec=unsandboxed (Config::allow_unsandboxed).
inline constexpr const char* EXEC_UNSANDBOXED_FLAG = "--allow-exec=unsandboxed";
inline constexpr const char* NO_SANDBOX_INSTALL =
    "no isolating sandbox here (bubblewrap missing or cannot start): install bubblewrap, or re-run "
    "with --allow-exec=unsandboxed to run it with resource limits only (only on code you trust)";

// Process-wide policy for stages that get no Config (fuzz, diff, afl,
// libFuzzer, sandbox_run). run_pipeline holds a Policy for the run.
// held_no_sandbox: --allow-exec was given but held back for want of a
// sandbox; exec_notrun then gives NO_SANDBOX_INSTALL as the hint.
PRISM_API bool allowed();
PRISM_API bool set_allowed(bool value);  // returns the previous value
PRISM_API bool held_no_sandbox();
PRISM_API bool set_held_no_sandbox(bool value);  // returns the previous value
struct Policy {
    bool prev, prev_held;
    explicit Policy(bool allow, bool held = false)
        : prev(set_allowed(allow)), prev_held(set_held_no_sandbox(held)) {}
    ~Policy() {
        set_allowed(prev);
        set_held_no_sandbox(prev_held);
    }
    Policy(const Policy&) = delete;
    Policy& operator=(const Policy&) = delete;
};

// "bwrap" (Linux, bwrap on PATH and a probe jail starts), "job-object"
// (Windows: every child runs in a kill-on-close Job Object with the memory
// and CPU caps of Limits), or "rlimits-only" (other POSIX).
PRISM_API std::string kind();
// True only for a kind that hides the file system and the network from the
// child (bwrap). job-object and rlimits-only cap resources, nothing more.
PRISM_API bool isolates(std::string_view kind);

// Law 9 gate for one run: may scanned code execute, and what the report says.
struct Gate {
    bool run = false;              // the effective --allow-exec
    bool held_no_sandbox = false;  // asked for, held back: no isolating sandbox
    std::string note;              // report note; empty when nothing to say
};
PRISM_API Gate exec_gate(bool allow_exec, bool allow_unsandboxed, std::string_view kind);
// exec_gate with this host's kind() (probed only when allow_exec).
PRISM_API Gate exec_gate_here(bool allow_exec, bool allow_unsandboxed);
// Subcommand flag: "--allow-exec" sets allow; "--allow-exec=unsandboxed"
// sets both. False (nothing set) for any other argument.
PRISM_API bool parse_allow_exec(std::string_view arg, bool& allow, bool& allow_unsandboxed);

// The jail's environment: every inherited variable in `names` is unset
// except PATH, LANG/LC_*, TERM, TZ, LD_LIBRARY_PATH, *SAN_OPTIONS, AFL_*, PRISM_FUZZ_SEED
// and a few build ones (no tokens, no SSH agent, no cloud keys), and HOME
// and TMPDIR point at the scratch dir. Variables a run sets itself
// (exec::Spec::env_set) still reach the child.
PRISM_API bool jail_env_kept(std::string_view name);
PRISM_API std::vector<std::string> jail_env(const std::vector<std::string>& names,
                                            const std::filesystem::path& scratch);

// The jail argv for one run (pure; bwrap is the resolved bwrap path).
// ro_mounts are the bwrap mount arguments that make up the read-only view
// (jail_mounts() for a real run). The three-argument form is the whole root
// read-only ("--ro-bind / /"), which wrap_argv no longer uses.
PRISM_API std::vector<std::string> bwrap_argv(const std::string& bwrap,
                                              const std::vector<std::string>& argv,
                                              const std::filesystem::path& scratch,
                                              const std::vector<std::string>& ro_mounts);
PRISM_API std::vector<std::string> bwrap_argv(const std::string& bwrap,
                                              const std::vector<std::string>& argv,
                                              const std::filesystem::path& scratch);
// The read-only view of a real run: /usr and the /bin, /sbin, /lib* entries
// (symlinks kept as symlinks), a minimal /etc (loader cache and config,
// alternatives, localtime, passwd, group, nsswitch.conf), the LLVM toolchain
// ($PRISM_LLVM_BIN/.., the pinned bin/..), the `prism-deps tool` home, and
// the program's own install prefix. The home directory itself (so ~/.ssh,
// tokens, shell history) is never bound.
PRISM_API std::vector<std::string> jail_mounts(const std::vector<std::string>& argv);
// A new directory under the temp dir that no one else made or can enter
// (mkdtemp on POSIX: random name, mode 0700; Windows: random name created
// exclusively). Empty path when none could be made. The caller removes it.
PRISM_API std::filesystem::path private_temp_dir(std::string_view prefix);

// True when argv is a wrap_argv jail (it starts with the probed bwrap).
PRISM_API bool jailed(const std::vector<std::string>& argv);
// bwrap reports a child killed by signal N as its own exit code 128+N; the
// signal, or 0 when the code is not one of those.
PRISM_API int jail_exit_signal(int exit_code);

// argv inside the jail when bwrap works here, else unchanged.
PRISM_API std::vector<std::string> wrap_argv(const std::vector<std::string>& argv,
                                             const std::filesystem::path& scratch);

struct Limits {
    bool enabled = false;
    double cpu_seconds = 0.0;  // wall-clock timeout of the run; CPU cap is +1s
    bool limit_as = true;      // false for ASan/TSan builds (shadow memory)
};
PRISM_API Limits limits_for(double timeout_s, bool limit_as = true);

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
