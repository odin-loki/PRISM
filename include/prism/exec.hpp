#pragma once

// PRISM's process runner: a thin interface over FORGE's
// forge::os::spawn_wait (os/process.hpp). Spec and Result map field for field
// onto spawn_spec / proc_limits / proc_result; failed (could not start) is
// FORGE's result<> error branch, with the reason in err.

#include "prism/export.hpp"

#include <cstdint>
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
    std::uint64_t file_size_bytes = 0;           // RLIMIT_FSIZE; 0: inherit
    bool no_core = false;                        // RLIMIT_CORE = 0
    bool merge_stderr = false;                   // stderr goes into out (order kept)
};

struct Result {
    int exit_code = -1;  // 128+signal when killed by a signal (FORGE/POSIX convention)
    int term_signal = 0;
    bool timed_out = false;
    bool truncated = false;
    bool failed = false;  // could not start; err says why
    bool crashed() const { return term_signal != 0 && !timed_out; }
    std::string out, err;
    std::uint64_t wall_ms = 0;
};

// Law 8: throws std::runtime_error on a --no-*-check flag, before starting anything.
PRISM_API void refuse_disabled_checks(const std::vector<std::string>& argv);

// Throws via refuse_disabled_checks; otherwise never throws.
PRISM_API Result run(const Spec& spec);

// The Law 9 sandbox caps (sandbox::Limits) as Spec fields: CPU = timeout + 1 s,
// open files, file size, no core, address space unless !limit_as. No-op when
// !enabled. Timeout is not touched.
PRISM_API void apply_limits(Spec& spec, bool enabled, double cpu_seconds, bool limit_as);

}  // namespace prism::exec
