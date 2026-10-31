#pragma once

// PRISM's thin process-runner interface, built like log.hpp: one spec, one
// result, one replaceable backend. The default backend is prism::detail::run
// (src/prism/proc.cpp). The fields map one-to-one onto FORGE's
// forge::os::spawn_spec / proc_limits / proc_result (os/process.hpp), so the
// FORGE switch is set_backend(&forge_backend) with a field copy:
//   Spec::argv/cwd/env_set/env_unset/stdin_data/timeout_ms/max_output -> spawn_spec
//   Spec::memory_bytes/cpu_seconds/open_files                         -> proc_limits
//   Result::exit_code/term_signal/timed_out/truncated/out/err/wall_ms  <- proc_result
// failed (could not start) comes from FORGE's result<> error branch.

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

using Backend = Result (*)(const Spec&);

// Replace the backend (nullptr restores the default). Returns the old one.
PRISM_API Backend set_backend(Backend b);

PRISM_API Result run(const Spec& spec);

}  // namespace prism::exec
