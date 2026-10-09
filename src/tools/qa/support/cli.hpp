#pragma once

// The prism-qa subcommands (src/tools/qa/main.cpp dispatches; the library
// entry points let prism_tests drive them in process).

#include "replay.hpp"
#include "task.hpp"

#include <map>
#include <set>

#include <string>
#include <vector>

namespace prism::qa {

// Minimal argparse: `--name VALUE`, `--name=VALUE`, `-x VALUE`, `-xVALUE`, flags.
struct ArgSpec {
    std::string long_name;   // "--timeout"
    std::string short_name;  // "-j" or ""
    bool takes_value = true;
    bool repeat = false;     // action="append"
    std::string help;
};

struct CliArgs {
    std::map<std::string, std::vector<std::string>> values;  // long name -> values
    std::set<std::string> flags;
    bool has(const std::string& n) const { return values.count(n) || flags.count(n); }
    std::string get(const std::string& n, const std::string& dflt = "") const;
};

// Parses argv against specs; on --help prints usage and returns false with
// rc 0; on an error prints it and returns false with rc 2.
bool parse_args(const std::string& prog, const std::string& desc, const std::vector<ArgSpec>& specs,
                const std::vector<std::string>& argv, CliArgs& out, int& rc);

int conformance_main(const std::vector<std::string>& argv);
int soundness_main(const std::vector<std::string>& argv);

// Label self-check: every witness trips a sanitizer, every `true` function
// survives the input grid; returns (records, number of FAIL records).
std::pair<std::vector<ojson>, int> self_check(const std::vector<Task>& tasks, const fs::path& work, int jobs);

}  // namespace prism::qa
