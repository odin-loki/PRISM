#pragma once

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace prism {

inline constexpr const char* STAGE_ORDER[] = {
    "inventory", "classify", "lints", "taint", "thread", "interval",
    "warnings", "cppcheck", "pbsd", "sanitize", "optional", "esbmc",
    "dafny", "contracts", "wp", "bmc", "harness", "concolic", "fuzz", "diff",
    "rapid", "muttest", "ltl", "llm", "execute", "repair", "unify", nullptr};

PRISM_API RunReport run_pipeline(const Config& cfg);
PRISM_API void write_report_md(const RunReport& report, const std::filesystem::path& path);
PRISM_API void apply_confidence(RunReport& report);
// llm stage is READS. A lying backend cannot COVER or prove.
PRISM_API std::vector<Finding> llm_forced_reads(std::vector<Finding> findings);

}  // namespace prism
