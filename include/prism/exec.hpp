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
};

struct Result {
    int exit_code = -1;  // 128+signal when killed by a signal (FORGE/POSIX convention)
    int term_signal = 0;
    bool timed_out = false;
    bool truncated = false;
    bool failed = false;  // could not start; err says why
    std::string out, err;
    std::uint64_t wall_ms = 0;
};

PRISM_API Result run(const Spec& spec);

}  // namespace prism::exec
