#pragma once

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"
#include "prism/verdict.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

inline constexpr const char* STAGE_ORDER[] = {
    "inventory", "classify", "lints", "taint", "thread", "interval",
    "warnings", "cppcheck", "pbsd", "sanitize", "optional", "polyglot", "esbmc",
    "dafny", "contracts", "wp", "pir", "conc", "harness", "review", "concolic", "fuzz", "diff",
    "rapid", "muttest", "ltl", "llm", "execute", "repair", "unify", nullptr};

PRISM_API RunReport run_pipeline(const Config& cfg);

// --resume replays a journal only for the inputs that wrote it. RUN_KEY_JSON
// under --out holds that run's key: a content digest of the scanned tree and
// the config fields that change results. A mismatch re-runs the stages the
// change affects and says so in report.notes.
inline constexpr const char* RUN_KEY_JSON = "run_key.json";
struct RunKey {
    // sha256 over (relative path, size, sha256 of the bytes) of every file
    // under the root (a file root: that file), the skipped vendor/build dirs
    // and --out excluded; plus --requirements files and an approvals file
    // outside the root.
    std::string tree;
    std::map<std::string, std::string> fields;  // option -> value as text
};
// cfg is the run's effective config (allow_exec after sandbox::exec_gate).
PRISM_API RunKey run_key(const Config& cfg);
PRISM_API std::optional<RunKey> load_run_key(const std::filesystem::path& out);
PRISM_API void save_run_key(const std::filesystem::path& out, const RunKey& key);
struct RunKeyDiff {
    std::set<std::string> rerun;  // stage names whose journal rows are stale
    std::vector<std::string> why;
};
// Changed tree or a general field: every stage. Only allow_exec /
// allow_unsandboxed / sandbox: the exec_stages(). Equal keys: empty.
PRISM_API RunKeyDiff diff_run_key(const RunKey& before, const RunKey& now);
PRISM_API void write_report_md(const RunReport& report, const std::filesystem::path& path);
// Law 5: (visibility, answer, resolution, confidence) of a report, without
// changing it. No parsed functions scores 0 on every factor.
PRISM_API verdict::Score confidence_score(const RunReport& report);
// Writes confidence_score rounded to 4 decimals into the report, and the
// "no functions parsed" note once when the scope is empty.
PRISM_API void apply_confidence(RunReport& report);
// x rounded to 4 decimals, ties to even (the report's number format).
PRISM_API double round4(double x);
// llm stage is READS. A lying backend cannot COVER or prove.
PRISM_API std::vector<Finding> llm_forced_reads(std::vector<Finding> findings);
// Law 9: stage -> "whole" | "part" (prism/pipeline.py EXEC_STAGES). Stages
// not listed execute nothing from the scanned tree.
PRISM_API const std::map<std::string, std::string>& exec_stages();
// Appends the stage-level --allow-exec NOTRUN row when a finding carries
// extra.exec = NOTRUN and no such row exists yet (prism/pipeline.py exec_gate_note).
PRISM_API std::vector<Finding> exec_gate_note(const std::string& stage, std::vector<Finding> findings,
                                              std::string_view what);

// From CMake project(prism VERSION ...) (target prism_core, PUBLIC).
#ifndef PRISM_VERSION_STRING
#  error "PRISM_VERSION_STRING is set by CMakeLists.txt (link prism_core)"
#endif
inline constexpr const char* PRISM_VERSION = PRISM_VERSION_STRING;

// SARIF 2.1.0 (src/prism/sarif.cpp, prism/sarif.py). Only FAILED/CRASH/SANFAIL
// become results; HYPOTHESIS is a note; NOTRUN/failed stages are
// tool execution notifications.
PRISM_API std::string to_sarif(const RunReport& report);
PRISM_API void write_sarif(const RunReport& report, const std::filesystem::path& path);
// --fail-on never|defect|gap. 2 = a stage crashed, 1 = policy tripped, else 0.
PRISM_API int exit_code(const RunReport& report, std::string_view fail_on);

}  // namespace prism
