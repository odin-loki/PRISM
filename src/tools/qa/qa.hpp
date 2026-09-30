#pragma once

// prism-qa: developer QA tools (not shipped). Each command is a function here
// so that the doctests (tests/cpp/test_qa.cpp) can drive it; main.cpp only
// dispatches.
//
//   triage-selfscan OUT/      bucket a self-scan report.sarif
//   pir-vs-bmc [TREE]         bmc encoder vs pir stage agreement matrix
//   pir-lean-check [TREE...]  C++ LLVM->PIR translator vs the proved Lean one
//   llvm-sem-vs-lli [FILE.c]  formal LLVM semantics (Lean) vs lli
//   libc-bounds               size-bound sweep of the libc model harnesses
//   solver-bench              portfolio vs Z3 alone on the pir VCs
//   assurance-check           cited artefacts of docs/assurance exist
//   docs-check                --help flags documented, anchors resolve, stage order
//
// PRISM runs as a child process in a session of its own
// (detail::run_session): a timeout or Ctrl-C kills it and every solver,
// checker and compiler it started.

#include "docscan.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa {

using Args = std::vector<std::string>;

// ---- support
// The prism binary: `explicit_bin`, else $PRISM_BIN, else `prism` next to
// this executable, else <repo>/build/prism. nullopt when none is executable.
std::optional<std::filesystem::path> find_prism(const std::string& explicit_bin, const std::filesystem::path& repo);
std::optional<nlohmann::json> load_json(const std::filesystem::path& p);
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& prefix);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---- triage-selfscan
struct TriageSummary {
    int failed = 0, third_party = 0, corpus = 0, src = 0, github = 0;
    int real = 0, false_alarm = 0, out_of_scope = 0;
};
// real | false_alarm | out_of_scope for one defect at `path` (repo-relative URI)
std::string triage_classify(const std::string& path, const std::string& status, const std::string& stage);
TriageSummary triage_selfscan(const nlohmann::json& sarif);
std::string triage_text(const TriageSummary& s);
int triage_selfscan_main(const Args& args);

// ---- pir-vs-bmc
// PROVED* (every proof class) | BOUNDED | FAILED | NEEDS-HARNESS | ERROR |
// UNKNOWN (UNKNOWN / TIMEOUT / NOFUNC / NOTRUN / anything else) | absent (null)
std::string pvb_bucket(const std::string* status);
struct PvbResult {
    std::map<std::pair<std::string, std::string>, int> matrix;  // (bmc bucket, pir bucket) -> functions
    nlohmann::json conflicts = nlohmann::json::array();         // PROVED* vs FAILED
    std::size_t functions = 0, bmc = 0, pir = 0;
};
PvbResult pir_vs_bmc(const nlohmann::json& report);
std::string pir_vs_bmc_text(const PvbResult& r);
nlohmann::json pir_vs_bmc_json(const PvbResult& r);
int pir_vs_bmc_main(const Args& args);

// ---- pir-lean-check
struct CheckerRows {
    std::map<std::string, int> counts;           // agree, agree-ext, agree-reject, outside, MISMATCH
    std::vector<std::vector<std::string>> rows;  // tab-separated columns of those lines
};
CheckerRows parse_checker_output(const std::string& out);
// "total: agree=.. agree-ext=.. agree-reject=.. outside=.. mismatch=.. (functions in the proved fragment: ..)"
std::string lean_total_line(const std::map<std::string, int>& total);
// The pir_lean_check executable (proofs/refinement, built with `lake build`
// when lake is found); nullopt: not built and not buildable.
std::optional<std::filesystem::path> find_pir_lean_checker(const std::string& explicit_path,
                                                          const std::filesystem::path& repo);
int pir_lean_check_main(const Args& args);

// ---- llvm-sem-vs-lli
struct PirlRecord {
    std::string name;
    std::vector<int> params;  // parameter widths in bits
    int ret = 0;              // return width; 0 = void
};
// Functions of a .pirl pair file that are in the modelled fragment, in file order.
std::vector<PirlRecord> pirl_records(const std::string& text);
// Up to n distinct argument vectors from per-width edge values (0, 1, 2, 7,
// 100, max, max-1, the sign bit, the largest positive) plus random ones.
std::vector<std::vector<std::uint64_t>> input_vectors(const std::vector<int>& widths, int n, std::mt19937_64& rng);
// `ir` plus a __prism_lli_main that calls fn(args) and prints the result.
std::string lli_harness(const std::string& ir, const std::string& fn, const std::vector<int>& widths, int ret,
                        const std::vector<std::uint64_t>& args);
int llvm_sem_vs_lli_main(const Args& args);

// ---- libc-bounds
struct HarnessSplit {
    std::string prelude;
    std::vector<std::pair<std::string, std::string>> funcs;  // name -> its comment and body, file order
};
HarnessSplit split_harness(const std::string& text);
// #include "x.h" -> #include "<suite>/x.h" (absolute)
std::string absolutize_includes(const std::string& prelude, const std::filesystem::path& suite);
int libc_bounds_main(const Args& args);

// ---- solver-bench
struct BenchTask {
    std::string ident;  // sidecar path relative to the suite root
    std::filesystem::path source;
};
// Conformance tasks under `roots` (sidecar *.yml with input_files and either
// `expected:` or an SV-COMP property the suite scores), sorted by path.
std::vector<BenchTask> discover_tasks(const std::vector<std::filesystem::path>& roots,
                                      const std::filesystem::path& suite);
int solver_bench_main(const Args& args);

// ---- docs-check
// Flags of `help` missing from the guide (sorted).
std::vector<std::string> undocumented_flags(const std::string& help, const std::string& guide);
int docs_check_main(const Args& args);

}  // namespace prism::qa
