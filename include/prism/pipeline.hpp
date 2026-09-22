#pragma once

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

inline constexpr const char* STAGE_ORDER[] = {
    "inventory", "classify", "lints", "taint", "thread", "interval",
    "warnings", "cppcheck", "pbsd", "sanitize", "optional", "polyglot", "esbmc",
    "dafny", "contracts", "wp", "bmc", "harness", "concolic", "fuzz", "diff",
    "rapid", "muttest", "ltl", "llm", "execute", "repair", "unify", nullptr};

PRISM_API RunReport run_pipeline(const Config& cfg);
PRISM_API void write_report_md(const RunReport& report, const std::filesystem::path& path);
PRISM_API void apply_confidence(RunReport& report);
// llm stage is READS. A lying backend cannot COVER or prove.
PRISM_API std::vector<Finding> llm_forced_reads(std::vector<Finding> findings);

// Same version string as the Python engine (prism/__init__.py __version__).
inline constexpr const char* PRISM_VERSION = "0.1.0";

// SARIF 2.1.0 (src/prism/sarif.cpp, prism/sarif.py). Only FAILED/CRASH/SANFAIL
// become results; HYPOTHESIS is a note; NOTRUN/failed stages are
// tool execution notifications.
PRISM_API std::string to_sarif(const RunReport& report);
PRISM_API void write_sarif(const RunReport& report, const std::filesystem::path& path);
// --fail-on never|defect|gap. 2 = a stage crashed, 1 = policy tripped, else 0.
PRISM_API int exit_code(const RunReport& report, std::string_view fail_on);

}  // namespace prism
