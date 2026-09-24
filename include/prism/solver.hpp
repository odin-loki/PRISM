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
// Nothing here edits the verdict lattice: the certified status is the one the
// verdict module owns (laws::PROVED_CERTIFIED). The pir stage routes every
// verification condition through solve() (docs/PIR.md, docs/SOLVERS.md).

#include "prism/laws.hpp"

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

// Alias kept for existing callers; the one spelling is laws::PROVED_CERTIFIED.
inline constexpr std::string_view kProvedCertified = laws::PROVED_CERTIFIED;

// A portfolio member supplied by the caller (user configuration or tests).
// "{input}" in argv is replaced by the query file path.
struct ExternalSolver {
    enum class Input { Smt2, Dimacs };
    std::string name;
    std::vector<std::string> argv;
    Input input = Input::Smt2;
};

// Which bit-blaster makes the CNF of a certified query (roadmap 3.2 step 2).
//   Lean: the Lean-proved bit-blaster (proofs/techniques, `prism-bitblast`,
//         theorem toCNF_equisat / checkDag_sound); its certificate must be
//         accepted by cake_lpr AND by Lean's verified LRAT checker
//         (`prism-lrat-check --dag`).
//   Z3:   Z3's simplify + bit-blast + tseitin-cnf tactics (unproved).
//   Auto: Lean for certified requests whose formula is inside the proved
//         fragment and whose tools are present; Z3 otherwise. Every fallback
//         is written into the note and the certificate_info.
// Plain (uncertified) requests use Z3's tactics unless Lean is forced.
enum class Bitblaster { Auto, Lean, Z3 };
std::string_view bitblaster_name(Bitblaster b);

struct SolveOptions {
    double timeout_s = 30.0;
    bool certified = false;     // roadmap 3.2: try to produce PROVED-CERTIFIED
    double check_timeout_s = 0; // LRAT checker budget; 0: max(60 s, 4 x timeout_s)
    // certified: seconds from the start of the query the certificate member
    // (CaDiCaL LRAT) and its bit-blast may run; the answer itself is still
    // due within timeout_s. 0: max(60 s, 4 x timeout_s).
    double cert_timeout_s = 0;
    bool portfolio = true;      // false: Z3 only (plus the certificate chain when certified)
    std::string cache_dir;      // empty: $XDG_CACHE_HOME/prism/solver or ~/.cache/prism/solver
    bool use_cache = true;      // the query cache; solve times are always recorded in cache_dir
    bool cache_certificates = true;  // keep the (packed) LRAT proof so a certified hit is re-checked
    // Size cap of <cache_dir>/certs, pruned least-recently-used
    // (include/prism/solver_certs.hpp). -1: $PRISM_CERT_CACHE_MAX, else 2 GiB.
    std::int64_t cert_cache_max_bytes = -1;
    // certified: the caller already has a plain UNSAT answer to this exact
    // formula from solve() and wants only its certificate. Then only the
    // certificate member (CaDiCaL with LRAT) runs, until the certificate
    // budget; without a certificate the result is that plain Unsat,
    // uncertified (as for a cached plain unsat). A validated model still wins.
    bool known_unsat = false;
    // Memory cap in MB of the certificate tools: cake_lpr's heap
    // (--CML_HEAP_SIZE) and the resident memory of the Lean tools
    // (prism-bitblast, prism-lrat-check; killed past it). 0:
    // $PRISM_CHECKER_MEM (a size such as "6G"), else 4096 (cake_lpr's own
    // default heap). Running out costs the certificate, never the answer,
    // and the note says "ran out of memory".
    unsigned checker_mem_mb = 0;
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
    Bitblaster bitblaster = Bitblaster::Auto;
    // Watchdog: a member job still running this long after it was stopped
    // (answer found, or its deadline) is detached and recorded
    // (SolveResult::watchdog); solve() does not wait for it.
    double watchdog_grace_s = 2.0;
    double debug_stall_s = 0.0;  // fault injection (tests): the Z3 job ignores its stop this long
};

struct SolveResult {
    enum Kind { Sat, Unsat, Unknown, Timeout, Error } kind = Unknown;
    std::string winner;                        // solver whose answer was accepted
    std::map<std::string, std::string> model;  // name -> SMT-LIB literal (#x.., #b.., true/false)
    bool certified = false;                    // Unsat AND cake_lpr accepted the LRAT proof
    std::string certificate_info;
    std::string cnf_sha256;                    // certified: sha256 of the exact CNF cake_lpr checked
    std::string query_hash;                    // sha256 of the normalised query
    bool cache_hit = false;
    std::string note;                          // everything that ran, was missing, or failed
    std::vector<std::string> ran;              // members that actually started
    std::vector<std::string> missing;          // members whose binary was not found
    std::map<std::string, double> times;       // seconds, members that finished
    std::string bucket;                        // scheduler feature bucket
    double wall_s = 0.0;
    std::vector<std::string> watchdog;         // jobs the watchdog detached (docs/SOLVERS.md "Stalls")
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
// heap_mb > 0: cake_lpr's heap cap (--CML_HEAP_SIZE, MB); any other checker
// is killed once its resident memory passes heap_mb MB. A checker that
// runs out of memory (CakeML heap exhausted, bad_alloc, killed by SIGKILL)
// is reported as such in `detail` ("ran out of memory ..."), never as a
// rejection and never as accepted.
CheckOutcome check_lrat(const ToolInfo& checker, const std::filesystem::path& cnf,
                        const std::filesystem::path& lrat, double timeout_s, unsigned heap_mb = 0);
std::size_t lrat_steps(const std::filesystem::path& lrat);  // addition lines

// ---- the Lean-proved bit-blaster (proofs/techniques; docs/PROOFS_TECHNIQUES.md) ----
// A formula in the proved bit-blaster's input format: `(dag (def W BASE e)* e)`
// (grammar in proofs/techniques/PrismTechniques/BitblastSexp.lean). Shared
// Z3 subterms become definitions, so the text is linear in the Z3 DAG.
struct LeanDag {
    struct Input {
        std::string name;
        unsigned width = 1;
        bool is_bool = false;
        unsigned long long base = 0;  // Lean input bits base..base+width-1
    };
    std::string text;
    std::vector<Input> inputs;
    std::size_t defs = 0;
};
// The model as `(rho (BASE WIDTH VALUE)*)`, for `prism-bitblast --eval`;
// nullopt when a value is malformed.
std::optional<std::string> lean_rho(const LeanDag& dag, const std::map<std::string, std::string>& model);
// C++ reference evaluator of the format (the semantics of `Dag.eval`), for
// widths up to 128 bits; nullopt (with why) otherwise. Used to test the
// serializer against Z3 when the Lean executables are not built.
std::optional<bool> eval_lean_dag(const LeanDag& dag, const std::map<std::string, std::string>& model,
                                  std::string* why = nullptr);
// Run `prism-bitblast` on the DAG: writes <work>/query.dag and the exact
// DIMACS text to <work>/query.cnf, returns the CNF with its variable map.
// mem_cap > 0: resident-memory cap in bytes (the process is killed past it).
std::optional<Cnf> lean_bitblast(const LeanDag& dag, const ToolInfo& exe, const std::filesystem::path& work,
                                 double timeout_s, std::string* why = nullptr,
                                 const std::atomic<bool>* stop = nullptr, std::uint64_t mem_cap = 0);
// `prism-lrat-check --dag DAG CNF LRAT`: Lean's verified LRAT checker on the
// CNF rebuilt by the proved bit-blaster (and byte-compared with CNF).
CheckOutcome check_lrat_dag(const ToolInfo& checker, const std::filesystem::path& dag,
                            const std::filesystem::path& cnf, const std::filesystem::path& lrat,
                            double timeout_s, std::uint64_t mem_cap = 0);

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
// Serialize a QF_BV formula into the proved bit-blaster's format; nullopt
// (with why, naming the operator) when it uses anything outside the proved
// fragment. Certified mode then falls back to bitblast() and says why.
std::optional<LeanDag> to_lean_dag(const z3::expr& f, std::string* why = nullptr);
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
