#pragma once

// PRISM solver library (roadmap Part 3.1 portfolio + query cache, 3.2 certified
// mode, 3.3 stochastic local search counterexample finder, CPU reference).
//
// The query contract: solve() is given a formula that is SATISFIABLE exactly
// when the property is violated. So
//   Sat     -> a counterexample; its model has been evaluated against the
//              ORIGINAL formula in Z3 before it is returned (never trusted raw);
//   Unsat   -> the property holds (plain: the solver is trusted, PROVED;
//              certified: an LRAT proof of the exact CNF was accepted by
//              cake_lpr, PROVED-CERTIFIED — see docs/TRUSTED_BASE.md);
//   Unknown / Timeout / Error -> no answer; never a clean result (Law 1, 7).
//
// Nothing here edits the verdict lattice; `kProvedCertified` is a local
// spelling of the status the verdict module owns (the integrator switches it
// to laws::PROVED_CERTIFIED).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef PRISM_HAS_Z3
#include <z3++.h>
#endif

namespace prism::solver {

inline constexpr std::string_view kProvedCertified = "PROVED-CERTIFIED";

// A portfolio member supplied by the caller (user configuration or tests).
// "{input}" in argv is replaced by the query file path.
struct ExternalSolver {
    enum class Input { Smt2, Dimacs };
    std::string name;
    std::vector<std::string> argv;
    Input input = Input::Smt2;
};

struct SolveOptions {
    double timeout_s = 30.0;
    bool certified = false;     // roadmap 3.2: try to produce PROVED-CERTIFIED
    bool portfolio = true;      // false: Z3 only (plus the certificate chain when certified)
    std::string cache_dir;      // empty: $XDG_CACHE_HOME/prism/solver or ~/.cache/prism/solver
    bool use_cache = true;      // the query cache; solve times are always recorded in cache_dir
    bool cache_certificates = true;  // keep CNF + LRAT so a certified hit is re-checked
    bool sls = true;            // ProbSAT walker (counterexamples only)
    double sls_budget_s = 0.0;  // 0: max(1 s, 10% of timeout_s); it then frees its core
    bool z3_in_process = true;  // tests switch Z3 off to observe other members alone
    unsigned max_parallel = 0;  // 0: std::thread::hardware_concurrency()
    std::vector<std::string> tool_dirs;  // searched first: <dir>/<name>/<sha>/{bin/,}<exe>
    bool search_default_tools = true;    // then ~/.prism/tools and PATH
    std::vector<ExternalSolver> extra_solvers;
    std::string work_dir;       // CNF / LRAT / SMT2 files; empty: fresh temp dir
    bool keep_artifacts = false;
    std::uint64_t seed = 1;
};

struct SolveResult {
    enum Kind { Sat, Unsat, Unknown, Timeout, Error } kind = Unknown;
    std::string winner;                        // solver whose answer was accepted
    std::map<std::string, std::string> model;  // name -> SMT-LIB literal (#x.., #b.., true/false)
    bool certified = false;                    // Unsat AND cake_lpr accepted the LRAT proof
    std::string certificate_info;
    std::string query_hash;                    // sha256 of the normalised query
    bool cache_hit = false;
    std::string note;                          // everything that ran, was missing, or failed
    std::vector<std::string> ran;              // members that actually started
    std::vector<std::string> missing;          // members whose binary was not found
    std::map<std::string, double> times;       // seconds, members that finished
    std::string bucket;                        // scheduler feature bucket
    double wall_s = 0.0;
};

std::string_view kind_name(SolveResult::Kind k);
// Status words for a verdict: PROVED-CERTIFIED only for a checked certificate.
std::string_view verdict_status(const SolveResult& r);

// ---- hashing, tools, cache ----
std::string sha256_hex(std::string_view data);
std::string sha256_file(const std::filesystem::path& p);  // empty if unreadable
std::string default_cache_dir();

struct ToolInfo {
    std::string name;
    std::filesystem::path path;
    std::string version;  // the <sha> directory under ~/.prism/tools, or "PATH"
};
std::optional<ToolInfo> find_tool(std::string_view name, const SolveOptions& opt);

// ---- CNF ----
struct Cnf {
    struct Symbol {
        std::string name;      // original constant name
        unsigned width = 1;    // bits
        bool is_bool = false;
        std::vector<int> vars;  // DIMACS var of bit i (LSB first); 0 = not in the CNF
    };
    int num_vars = 0;
    std::vector<std::vector<int>> clauses;
    std::vector<Symbol> symbols;  // the variable map back to the formula
};
std::string to_dimacs(const Cnf& cnf);
std::optional<Cnf> parse_dimacs(std::string_view text, std::string* why = nullptr);
// assignment[v] for v in 1..num_vars (index 0 unused): 1 true, 0 false.
std::map<std::string, std::string> model_from_assignment(const Cnf& cnf,
                                                         const std::vector<std::int8_t>& assignment);
bool assignment_satisfies(const Cnf& cnf, const std::vector<std::int8_t>& assignment);
// "v 1 -2 3 0" lines of a SAT solver's output -> assignment (size num_vars+1).
std::optional<std::vector<std::int8_t>> parse_sat_values(std::string_view out, int num_vars);

// ---- stochastic local search (roadmap 3.3, CPU reference) ----
struct SlsOptions {
    std::uint64_t seed = 1;
    std::uint64_t max_flips = 0;  // 0: until stop / timeout
    double timeout_s = 0.0;       // 0: none
    double cb = 0.0;              // 0: chosen from max clause length (ProbSAT defaults)
    double eps = 1.0;
    const std::atomic<bool>* stop = nullptr;
};
struct SlsResult {
    bool found = false;  // found a satisfying assignment; "not found" means NOTHING
    std::vector<std::int8_t> assignment;
    std::uint64_t flips = 0;
};
SlsResult probsat(const Cnf& cnf, const SlsOptions& opt);

// ---- LRAT certificate checking ----
struct CheckOutcome {
    bool ran = false;
    bool verified = false;
    std::string checker;
    std::string version;
    std::string detail;
};
CheckOutcome check_lrat(const ToolInfo& checker, const std::filesystem::path& cnf,
                        const std::filesystem::path& lrat, double timeout_s);
std::size_t lrat_steps(const std::filesystem::path& lrat);  // addition lines

#ifdef PRISM_HAS_Z3
struct Features {
    unsigned max_bv_width = 0;
    std::size_t nodes = 0;
    std::size_t consts = 0;
    bool arrays = false, fp = false, uf = false, arith = false, quant = false, other_sort = false;
    bool bv_mul_div = false;
    std::string logic() const;   // QF_BV, QF_ABV, QF_FP, QF_UFBV, ALL
    std::string bucket() const;  // scheduler bucket
};
Features features(const z3::expr& f);
// Empty when the formula is quantifier-free bitvector/Boolean only (certifiable).
std::string not_certifiable_reason(const z3::expr& f);
// Normalised SMT-LIB2 text (simplified, constants renamed v0..vn by first
// occurrence) and its sha256. canonical[i] is the original name of "v<i>".
std::string normalized_query(z3::context& c, const z3::expr& f,
                             std::vector<std::string>* canonical = nullptr);
std::string query_hash(z3::context& c, const z3::expr& f);
// Bit-blast a certifiable formula to CNF with Z3's simplify, bit-blast and
// tseitin-cnf tactics; nullopt (with why) when that is not possible.
std::optional<Cnf> bitblast(z3::context& c, const z3::expr& f, std::string* why = nullptr);
// Evaluate the formula under the model (completion on) in Z3; true only if it
// evaluates to true. A malformed value fails; names that are not constants of
// the formula are ignored (they cannot change its value).
bool validate_model(z3::context& c, const z3::expr& f,
                    const std::map<std::string, std::string>& model, std::string* why = nullptr);
SolveResult solve(z3::context& c, const z3::expr& formula, const SolveOptions& opt);
#endif

#ifdef PRISM_HAS_CUDA
// src/cuda/probsat.cu: many independent ProbSAT walkers on the GPU.
// Returns true and fills assignment (size num_vars+1) when one satisfies all
// clauses. Mirrors probsat() above. Never a proof.
bool cuda_probsat(const Cnf& cnf, std::uint64_t seed, std::uint64_t max_flips_per_walker,
                  unsigned walkers, std::vector<std::int8_t>& assignment);
#endif

}  // namespace prism::solver
