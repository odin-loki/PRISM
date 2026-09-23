#pragma once

// The verdict lattice (docs/VERDICTS.md). A small pure module: no I/O, no
// allocation beyond the returned strings, no dependency on the rest of
// PRISM. It is the C++ image of proofs/Prism/Verdict.lean; the doctest
// "verdict module equals the Lean model" checks every function here against
// tests/data/verdict_tables.json, which `lake exe verdict_tables` prints
// from the proved model, entry by entry over the whole finite domain.
//
// prism::laws (include/prism/laws.hpp) keeps its string API and forwards
// here. prism/laws.py is the Python mirror.

#include "prism/export.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#ifdef ERROR
#  undef ERROR
#endif

namespace prism::verdict {

// Order is the table order of the Lean model (Verdict.all).
enum class Verdict : std::uint8_t {
    ProvedCertified,  // PROVED-CERTIFIED: UNSAT checked by a verified checker
    ProvedUnbounded,  // PROVED-UNBOUNDED: k-induction closed
    Proved,           // PROVED: all properties hold, loops closed in k
    ProvedAssuming,   // PROVED-ASSUMING: under an explicit requires
    Bounded,          // BOUNDED: nothing found within k; that is all
    Failed,
    Unknown,
    Timeout,
    Error,
    NoFunc,
    NotRun,
    NeedsHarness,
    Crash,
    Clean,  // fuzz: not a proof
    NoSeed,
    SanFail,
    Hypothesis,  // LLM
    Reads,       // LLM
};
inline constexpr std::size_t kVerdictCount = 18;

PRISM_API const std::array<Verdict, kVerdictCount>& all_verdicts();
PRISM_API std::string_view name(Verdict v);
PRISM_API std::optional<Verdict> parse(std::string_view status);

PRISM_API bool is_proof(Verdict v);     // the four PROVED-* verdicts
PRISM_API bool is_formal(Verdict v);    // a proof or BOUNDED: never merged (Law 2)
PRISM_API bool is_answered(Verdict v);  // formal or FAILED
PRISM_API bool no_answer(Verdict v);    // UNKNOWN TIMEOUT ERROR NOFUNC NOTRUN NEEDS-HARNESS
PRISM_API bool is_defect(Verdict v);    // FAILED CRASH SANFAIL
PRISM_API bool is_model(Verdict v);     // HYPOTHESIS READS
PRISM_API int rank(Verdict v);          // 5 CERTIFIED .. 1 BOUNDED, 0 otherwise

// Why two claims about one function stay two claims (mergeRefusal).
enum class Refusal : std::uint8_t { None, Formal, PromoteFuzz, PromoteModel, NotRunClean };
PRISM_API std::string_view name(Refusal r);
PRISM_API Refusal merge_refusal(Verdict a, Verdict b);

// May a recorded status later be rewritten (mayRewrite)? Formal claims only
// weaken; nothing becomes a formal claim by rewriting; a status without an
// answer stays without one.
PRISM_API bool may_rewrite(Verdict from, Verdict to);

// Where a result came from. Only Solver / ExternalProver may prove.
enum class Origin : std::uint8_t { Solver, ExternalProver, Fuzzer, Model, Lint, Execution, Pipeline };
inline constexpr std::size_t kOriginCount = 7;
PRISM_API const std::array<Origin, kOriginCount>& all_origins();
PRISM_API std::string_view name(Origin o);
PRISM_API bool may_prove(Origin o);

// The gate every formal claim passes (admit). Non-formal statuses pass
// unchanged; a formal claim from an origin that may not prove becomes
// UNKNOWN; PROVED-CERTIFIED needs the Solver origin and a checked
// certificate, else it falls back to PROVED (never upward).
PRISM_API Verdict admit(Origin origin, Verdict requested, bool certificate_checked);

// Stage -> origin: the audit table. Unknown stage names are Pipeline.
PRISM_API Origin stage_origin(std::string_view stage);
// The stages of the table, in STAGE_ORDER, plus "other".
PRISM_API const std::array<std::string_view, 31>& audit_stages();

struct AuditResult {
    Verdict status;
    bool violation;
};
// Final pipeline pass: status after admit(stage_origin(stage), ...), and
// whether that differs from what the stage wrote.
PRISM_API AuditResult audit(std::string_view stage, Verdict status, bool certificate_checked);

// Law 5. confidence = visibility x answer x resolution; exactly 0 when
// visibility is 0 (no data scores 0).
PRISM_API double confidence(double visibility, double answer, double resolution);

struct Score {
    double visibility, answer, resolution, confidence;
};
// n/d with 0 for d == 0, per factor (Lean `score`).
PRISM_API Score score_counts(long n_fun, long classified, long attempted, long answered,
                             long resolved);

}  // namespace prism::verdict
