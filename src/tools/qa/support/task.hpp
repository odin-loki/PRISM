#pragma once

// Conformance tasks (tests/conformance/**.yml, one sidecar per source file)
// and the scorer's fixed tables. See src/tools/qa/conformance.cpp for the
// task format and the scoring rules.

#include "../docscan.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa {

namespace fs = std::filesystem;
using i128 = __int128;

// ---- fixed tables
extern const std::set<std::string> PROOF;             // PROVED, PROVED-UNBOUNDED, PROVED-ASSUMING, PROVED-CERTIFIED
extern const std::vector<std::string> BASE_STAGES;    // inventory, classify, bmc, harness
extern const std::vector<std::string> VERDICT_STAGES; // bmc, harness, pir, conc
// Stages that only speak about part of the functions (harness: POINTER
// functions under `// requires:`). A function they do not mention is out of
// scope, not silently skipped; bmc and pir must report every function.
extern const std::set<std::string> SCOPED_STAGES;
// conc is scored only on the thread programs of tests/conformance/concurrency,
// and those only by conc.
extern const std::map<std::string, std::set<std::string>> STAGE_TASK_ORIGINS;
extern const std::map<std::string, std::set<std::string>> ORIGIN_STAGES;
extern const std::vector<std::string> DEFAULT_ROOTS;  // prism, sv-comp, concurrency, esbmc-cpp, libc-models
// Task origins whose labels speak for one property only (another class of
// FAILED is reported separately, not as a false alarm).
extern const std::set<std::string> PROPERTY_SCOPED;
extern const std::string CERT_STAGE;                  // pir-certified
extern const std::vector<std::string> KEEP_EXTRA;     // finding extras kept in results.json
// SV-COMP property file -> suite property (in rank order)
extern const std::vector<std::pair<std::string, std::string>> SV_PROPERTIES;
// Which PRISM classes witness which property (for property-scoped tasks).
extern const std::map<std::string, std::set<std::string>> PROPERTY_CLASSES;

// ---- the repository (default suite, cwd of the PRISM runs)
// repo_root() lives in docscan.hpp (walks up for tests/conformance/SOURCES.md
// or include/prism/laws.hpp, else the built-from source tree).
inline fs::path suite_root() { return repo_root() / "tests" / "conformance"; }

struct Task {
    std::string ident;  // path of the sidecar relative to the suite root
    fs::path yml;
    fs::path source;
    std::string origin;  // prism | sv-comp | concurrency | esbmc-cpp | libc-models | juliet | ...
    std::string category;
    std::string lang = "C";
    std::string prop;
    std::vector<std::pair<std::string, bool>> expected;  // function -> no violation; file order
    std::map<std::string, std::vector<i128>> witness;
    std::map<std::string, std::string> expect_status;
    std::set<std::string> sanitizer_blind;
    std::string data_model;
    std::string std;  // whole-program tasks (esbmc-cpp): language standard of the native self-check
    bool deterministic = false;  // whole program whose one run is its only behaviour
    int unwind = 0;              // per-task --unwind (0: the run's default)
    // per-task time budget in seconds for one PRISM run (0: the run's
    // --timeout); never lowers the run's budget
    double timeout = 0.0;
    std::map<std::string, std::set<std::string>> expect_class;  // false fn -> classes that refute it

    bool has(const std::string& fn) const;
    bool expected_of(const std::string& fn) const;
};

// A task, or nothing when the file is not a task (no input_files, or an
// SV-COMP definition with no verdict for a suite property). Throws
// yaml::Error on YAML outside the supported subset.
std::optional<Task> load_task(const fs::path& yml, const fs::path& root);
// every *.yml under the roots (sorted), whose source exists
std::vector<Task> discover(const std::vector<fs::path>& roots);
// sorted recursive glob of *.yml (Path.rglob order after sorted())
std::vector<fs::path> rglob_yml(const fs::path& root);

// Python-style text helpers (code points, not bytes)
std::string py_head(const std::string& s, std::size_t n);  // s[:n]
std::string py_tail(const std::string& s, std::size_t n);  // s[-n:]
std::string i128_str(i128 v);

}  // namespace prism::qa
