#pragma once

// Proof library (docs/PROOF_LIBRARY.md, phase L1): verdict rows of earlier
// runs (report.json), imported with a trust tier, indexed by the engine-
// independent keys pir rows carry (extra.pir_fingerprint, extra.pir_shape)
// and by file::function. A library entry is data, never a verdict: L2 reuses
// an entry only through a re-check.
//
// Layout of a library directory:
//   entries/        FORGE content-addressed store (prism::cache): one JSON entry each
//   index/<kind>    sorted "key\tentry-id" lines; kinds: exact, shape, function
//   summary.json    counts by status, tier, level, stage and NEEDS-HARNESS reason

#include "prism/export.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace prism::library {

struct ImportOptions {
    std::filesystem::path library{"prism-library"};
    std::string tier{"T0"};                 // T0 recorded | T1 replayed (docs/PROOF_LIBRARY.md §2)
    std::filesystem::path vetting;          // JSONL of {"file","function"| "id", "tier", "note"}; "" none
    std::vector<std::string> stages{"pir"}; // which stages' rows are imported
    int jobs = 0;                           // report parsing threads (0: half the cores)
};

struct ImportStats {
    std::size_t reports = 0, rows = 0, imported = 0, duplicates = 0, demoted = 0, skipped = 0;
    std::vector<std::string> errors;        // unreadable or malformed reports
};

// Each path is a run directory (report.json inside) or a report.json.
PRISM_API ImportStats import_runs(const std::vector<std::filesystem::path>& runs, const ImportOptions& opt);
// The entry a pir row becomes (exposed for tests); `tier` as given.
PRISM_API nlohmann::json entry_of(const nlohmann::json& row, const nlohmann::json& report_meta,
                                  const std::string& run, const std::string& tier);
PRISM_API nlohmann::json summary(const std::filesystem::path& library);
// Apply a vetting file to entries already imported: (changed, errors).
PRISM_API std::pair<std::size_t, std::vector<std::string>> vet(const std::filesystem::path& library,
                                                               const std::filesystem::path& vetting);
// Entries whose index `kind` (exact | shape | function) has `key`.
PRISM_API std::vector<nlohmann::json> lookup(const std::filesystem::path& library, const std::string& kind,
                                             const std::string& key);
// `prism library import|stats|lookup ...`
PRISM_API int library_main(int argc, char** argv);

}  // namespace prism::library
