#pragma once

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

std::vector<FunctionInfo> inline_static(const std::vector<FunctionInfo>& functions);

std::vector<Finding> run_lints(const std::vector<std::filesystem::path>& paths,
                               const std::filesystem::path& root, int jobs = 0);
std::vector<Finding> run_taint(const std::vector<FunctionInfo>& functions);
std::vector<Finding> run_thread(const std::vector<FunctionInfo>& functions);
std::vector<Finding> run_interval(const std::vector<FunctionInfo>& functions);
std::vector<Finding> run_compiler(const std::vector<std::filesystem::path>& paths,
                                  const Config& cfg);
std::vector<Finding> run_cppcheck(const std::vector<std::filesystem::path>& paths,
                                  const Config& cfg);
std::vector<Finding> run_pbsd_lints(const std::vector<std::filesystem::path>& paths,
                                    const Config& cfg);
std::vector<Finding> run_sanitize(const std::vector<std::filesystem::path>& paths,
                                  const Config& cfg);
// Law 9 (sanitize calls only what the author opted in): `// prism: run` on
// the definition or in the comment block directly above it (1-based line).
// Same rule as the Python engine prism/sanitize.py marked_run / _opted_in_callable.
bool marked_run(const std::vector<std::string>& lines, int line);
std::optional<std::string> opted_in_callable(const std::filesystem::path& path);
std::vector<Finding> run_optional_tools(const std::vector<std::filesystem::path>& paths,
                                        const Config& cfg);
// Every language in the tree (prism/polyglot.py): built-in conflict-marker /
// credential scan plus per-language syntax, lint and type tools.
std::vector<std::filesystem::path> iter_polyglot_sources(const std::filesystem::path& root);
// Every text file in scope (no NUL in the first 8 KiB, <= 2 MB), whatever its name.
std::vector<std::filesystem::path> iter_text_files(const std::filesystem::path& root);
// Known source/config extension (prism/polyglot.py is_known_source).
bool is_known_source(const std::filesystem::path& p);
bool is_text_file(const std::filesystem::path& p);
std::vector<Finding> run_polyglot(const std::filesystem::path& root, const Config& cfg);
std::vector<Finding> run_esbmc(const std::vector<std::filesystem::path>& paths,
                               const Config& cfg);
std::vector<Finding> run_dafny(const std::vector<std::filesystem::path>& paths,
                               const Config& cfg);
std::vector<Finding> prove_contracts(const std::vector<FunctionInfo>& functions, int unwind);
// The contracts engine (BMC with requires assumed, ensures asserted at every
// return) for a contract given here instead of in fn's comments. SCALAR only;
// anything else is NEEDS-HARNESS. Stage "contracts" on the result.
PRISM_API Finding prove_with_contract(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                                      const std::optional<std::string>& ensures);
std::vector<Finding> run_wp(const std::vector<FunctionInfo>& functions, int unwind);
std::vector<Finding> run_bmc(const std::vector<FunctionInfo>& functions, int unwind,
                             bool allow_local_pointers = false);
std::vector<Finding> run_harness_bmc(const std::vector<FunctionInfo>& functions, int unwind);
std::vector<Finding> run_concolic(const std::vector<FunctionInfo>& functions, int budget = 32);

// Python engine prism/bmc.py harness_for_parsefail. Unmapped parsefail is ERROR (nullopt),
// not a generic NEEDS-HARNESS. Plain goto stays ERROR.
std::optional<std::string> harness_for_parsefail(std::string_view err, const std::string& engine);

// KLEE Executor::fork: SAT model of the flipped branch, or Unsat to drop that side.
enum class ForkFlipKind { Model, Unsat, Unknown };
struct ForkFlipResult {
    ForkFlipKind kind = ForkFlipKind::Unknown;
    std::map<std::string, int> args;
};
PRISM_API ForkFlipResult solve_fork_flip(const FunctionInfo& fn,
                                         const std::map<std::string, int>& seed,
                                         const std::string& cond, bool want);
std::vector<Finding> run_fuse(const std::vector<FunctionInfo>& functions,
                              const std::vector<Finding>& bmc_findings,
                              const std::filesystem::path& src_root,
                              double budget, int iters, bool llm);
std::vector<Finding> run_diff(const std::vector<FunctionInfo>& functions,
                              const std::filesystem::path& root);
std::vector<Finding> run_rapid(const std::vector<FunctionInfo>& functions, int trials = 64);
std::vector<Finding> run_muttest(const std::vector<FunctionInfo>& functions, int trials = 32);
std::vector<Finding> run_ltl(const std::vector<FunctionInfo>& functions,
                             const std::vector<std::filesystem::path>& specs);
std::vector<Finding> hypothesize(const std::vector<FunctionInfo>& functions, int budget,
                                 const Config& cfg);
std::vector<Finding> execute_cex(const std::vector<Finding>& fails,
                                 const std::vector<FunctionInfo>& functions,
                                 const Config& cfg);
std::vector<Finding> rlef_repair(const Finding& fail, const Config& cfg);

struct ConcreteRec {
    std::string ub;
    int rc = 0;
};
ConcreteRec concrete_execute(const FunctionInfo& fn,
                             const std::map<std::string, int>& args);

}  // namespace prism
