#pragma once

#include "prism/checkers.hpp"
#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

std::vector<FunctionInfo> inline_static(const std::vector<FunctionInfo>& functions);

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
// The wp stage's predicate encoder: the scalar C form of an ACSL/comment
// predicate, or nullopt when it is not encodable (\valid, \old, quantifiers,
// ->, calls, ...), which makes the wp record ERROR.
std::optional<std::string> encode_wp_predicate(const std::string& raw);
std::vector<Finding> run_bmc(const std::vector<FunctionInfo>& functions, int unwind,
                             bool allow_local_pointers = false);
// The year of a `-std=` flag's C++ standard (c++98/03 -> 3, c++0x/11 -> 11,
// c++2a/20 -> 20, gnu++ alike); 0 for anything else. Same in prism/bmc.py.
int cxx_std_year(std::string_view std_flag);
// functions with FunctionInfo::cxx_std set from the -std= of their unit in
// compile_commands.json (root or root/build), as the lints read it.
std::vector<FunctionInfo> with_cxx_std(std::vector<FunctionInfo> functions, const std::filesystem::path& root);
std::vector<Finding> run_harness_bmc(const std::vector<FunctionInfo>& functions, int unwind);
// The harness stage's materialization of a POINTER function: each pointer
// parameter becomes a local buffer of its pointee type, sized by an honest
// `requires`, and the requires guard the body. nullopt when the function is
// not POINTER or has no requires that sizes every buffer (NEEDS-HARNESS).
std::optional<FunctionInfo> materialize_harness(const FunctionInfo& fn);
std::vector<Finding> run_concolic(const std::vector<FunctionInfo>& functions, int budget = 32);

// Python engine prism/bmc.py harness_for_parsefail. Unmapped parsefail is ERROR (nullopt),
// not a generic NEEDS-HARNESS. An unstructured goto is NEEDS-HARNESS.
std::optional<std::string> harness_for_parsefail(std::string_view err, const std::string& engine);

// Syntax the encoders and the concrete interpreter do not model, as a
// NEEDS-HARNESS reason naming `engine` (nullopt: none). Memoized: every
// stage asks about the same functions.
std::optional<std::string> unencoded_syntax_reason_cached(const FunctionInfo& fn, std::string_view engine);

// KLEE Executor::fork: SAT model of the flipped branch, or Unsat to drop that side.
enum class ForkFlipKind { Model, Unsat, Unknown };
struct ForkFlipResult {
    ForkFlipKind kind = ForkFlipKind::Unknown;
    std::map<std::string, int> args;
};
PRISM_API ForkFlipResult solve_fork_flip(const FunctionInfo& fn,
                                         const std::map<std::string, int>& seed,
                                         const std::string& cond, bool want);
// FuSeBMC goals of a function: each if/while/for condition and its negation
// (then, implicit else, loop exit) and each switch case, whitespace
// normalised, first occurrence kept.
std::vector<std::string> branch_goals(const FunctionInfo& fn);
// The same goals labelled GOAL_1, GOAL_2, ... in order (FuSeBMC numbering).
std::vector<std::pair<std::string, std::string>> numbered_goals(const FunctionInfo& fn);

// Fuzz4All prompt scoring: unique non-empty seeds padded/cut to nbytes.
int score_prompt_seeds(const std::vector<std::vector<uint8_t>>& seeds, int nbytes);
struct PromptPick {
    std::string prompt;
    std::vector<std::vector<uint8_t>> seeds;
    int score = 0;
};
// The candidate with the highest score (ties: the first); empty for none.
PromptPick pick_best_prompt(const std::vector<std::pair<std::string, std::vector<std::vector<uint8_t>>>>& candidates,
                            int nbytes);
// Fuzz4All update_strategy: 0 generate, 1 mutate, 2 semantic, 3 combine
// (with prev_hex; mutate without it).
std::string fuzz4all_update_strategy(const std::string& new_hex, const std::string& prev_hex, int strategy);
// Documentation comments of a translation unit (no ACSL blocks, no contract
// lines), at most 8, one per line.
std::string documentation_from_comments(const std::string& source);
struct FuzzPrompt {
    std::string docstring, example_code, hw_prompt, target_api;
};
// Fuzz4All prompt ingredients of one function (a distilled prompt is a
// HYPOTHESIS, never a check).
FuzzPrompt create_prompt_from_source(const std::string& name, const std::string& body, const std::string& source);

std::vector<Finding> run_fuse(const std::vector<FunctionInfo>& functions,
                              const std::vector<Finding>& bmc_findings,
                              const std::filesystem::path& src_root,
                              double budget, int iters, bool llm);
// cfg: where afl-fuzz is looked up (--tool, the pinned build, PATH).
std::vector<Finding> run_fuse(const std::vector<FunctionInfo>& functions,
                              const std::vector<Finding>& bmc_findings,
                              const std::filesystem::path& src_root,
                              double budget, int iters, bool llm, const Config& cfg);
std::vector<Finding> run_diff(const std::vector<FunctionInfo>& functions,
                              const std::filesystem::path& root);
std::vector<Finding> run_rapid(const std::vector<FunctionInfo>& functions, int trials = 64);
std::vector<Finding> run_muttest(const std::vector<FunctionInfo>& functions, int trials = 32);
// Status + extra for a rapid/muttest evaluation error (twin of prism/rapid.py _eval_status).
std::pair<std::string, std::map<std::string, std::string>> rapid_plan_error_status(const std::string& err);
// cfg only locates a strix binary (tools[], pinned build, PATH) to record it;
// strix output is never a verdict.
std::vector<Finding> run_ltl(const std::vector<FunctionInfo>& functions,
                             const std::vector<std::filesystem::path>& specs,
                             const Config& cfg = Config{});

// The finite machine of a `switch (state)` body (ltl stage).
struct LtlFsm {
    std::vector<std::string> states, cases, assigns;
    std::vector<std::pair<std::string, std::string>> transitions;
};
// nullopt unless the body has a `switch (state)` / `switch (p->state)` /
// `switch (obj.state)` with at least one case and two states.
std::optional<LtlFsm> extract_ltl_fsm(const std::string& body);
// G p, G (p -> X q), G (req -> F_k ack) and the GF / FG / U / F safety
// approximations on one machine; nullopt when the formula is outside that
// fragment or a predicate cannot be evaluated.
std::optional<Finding> check_ltl_safety(const std::string& formula, const LtlFsm& fsm);
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
