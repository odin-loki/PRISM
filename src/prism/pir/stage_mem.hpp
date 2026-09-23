#pragma once

// Memory-model hooks of the pir stage (stage.cpp calls these; the policy
// lives in stage_mem.cpp so the stage driver stays small).

#include "prism/config.hpp"
#include "prism/models.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prism::pir::pirmem {

struct UnitInfo {
    std::filesystem::path path;
    std::string rel;
    bool cxx = false;
    const std::vector<std::string>& src_lines;
    std::optional<std::vector<FunctionInfo>> functions;  // cparse view, for harness drafts (lazy)
};

// `// prism: asm ensures <cond>` contracts of a unit: source line of the asm -> cond.
std::map<int, std::string> asm_contracts(const std::vector<std::string>& lines);

// Options for translating one function: pointer-parameter contracts from the
// source (`requires:`) or, failing that, from a template harness draft
// (ai::draft_harness), --strict-aliasing, and the global-state policy
// (initial values only for `main`).
TranslateOptions function_options(const TranslateOptions& base, const ir::Function& irf, UnitInfo& unit, int line,
                                  const Config& cfg, const std::string& source_name);

// Harness-draft assumptions ("p points to at least 4 int element(s)", "p
// points to exactly n int elements ...", "1 <= n <= 8 ...") as contracts.
std::vector<PtrContract> contracts_from_draft(const std::vector<std::string>& assumptions, const ir::Function& irf);

// After check_function:
//  * a proof under listed assumptions (pointer contracts) becomes PROVED-ASSUMING;
//  * a violation that disappears when mutable globals hold their initial
//    values is a missing precondition on global state: NEEDS-HARNESS;
//  * memory-model notes (encoding, strict aliasing off, library models).
void apply_memory_policy(Finding& f, Verdict& v, Function& fn, const ir::Module& mod, const ir::Function& irf,
                         const TranslateOptions& topt, const Config& cfg);

// Why translation validation cannot replay this function (empty: it can).
std::string tv_exclusion(const Function& fn);

}  // namespace prism::pir::pirmem
