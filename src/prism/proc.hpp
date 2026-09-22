#pragma once

// Internal to prism_core: the adapter process runner (src/prism/adapters.cpp)
// for stages that live in other translation units.

#include <filesystem>
#include <string>
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

}  // namespace prism::detail
