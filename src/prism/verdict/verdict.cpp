// The verdict lattice. Pure: no I/O. Image of proofs/Prism/Verdict.lean,
// checked entry by entry against tests/data/verdict_tables.json.
#include "prism/verdict.hpp"

namespace prism::verdict {

namespace {

using V = Verdict;

constexpr std::array<std::string_view, kVerdictCount> kNames{
    "PROVED-CERTIFIED", "PROVED-UNBOUNDED", "PROVED", "PROVED-ASSUMING", "BOUNDED",
    "FAILED", "UNKNOWN", "TIMEOUT", "ERROR", "NOFUNC", "NOTRUN", "NEEDS-HARNESS",
    "CRASH", "CLEAN", "NOSEED", "SANFAIL", "HYPOTHESIS", "READS"};

constexpr std::array<std::string_view, 31> kStages{
    "inventory", "classify", "lints", "taint", "thread", "interval",
    "warnings", "cppcheck", "pbsd", "sanitize", "optional", "polyglot", "esbmc",
    "dafny", "contracts", "wp", "bmc", "pir", "harness", "review", "concolic", "fuzz", "diff",
    "rapid", "muttest", "ltl", "llm", "execute", "repair", "unify", "other"};

// Same rows as Stage.origin in the Lean model.
constexpr std::array<Origin, 31> kStageOrigin{
    Origin::Pipeline, Origin::Pipeline,                            // inventory classify
    Origin::Lint, Origin::Lint, Origin::Lint, Origin::Lint,        // lints taint thread interval
    Origin::Lint, Origin::Lint, Origin::Lint,                      // warnings cppcheck pbsd
    Origin::Execution,                                             // sanitize
    Origin::ExternalProver,                                        // optional (CBMC)
    Origin::Lint,                                                  // polyglot
    Origin::ExternalProver, Origin::ExternalProver,                // esbmc dafny
    Origin::Solver, Origin::Solver, Origin::Solver,                // contracts wp bmc
    Origin::Solver,                                                // pir (Clang/LLVM front end)
    Origin::Solver,                                                // harness
    Origin::Solver,                                                // review (re-checked proofs)
    Origin::Execution,                                             // concolic
    Origin::Fuzzer,                                                // fuzz
    Origin::Execution,                                             // diff
    Origin::Fuzzer,                                                // rapid
    Origin::Execution,                                             // muttest
    Origin::Solver,                                                // ltl
    Origin::Model,                                                 // llm
    Origin::Execution,                                             // execute
    Origin::Model,                                                 // repair
    Origin::Pipeline,                                              // unify
    Origin::Pipeline};                                             // other

}  // namespace

const std::array<Verdict, kVerdictCount>& all_verdicts() {
    static constexpr std::array<Verdict, kVerdictCount> k{
        V::ProvedCertified, V::ProvedUnbounded, V::Proved, V::ProvedAssuming, V::Bounded,
        V::Failed, V::Unknown, V::Timeout, V::Error, V::NoFunc, V::NotRun, V::NeedsHarness,
        V::Crash, V::Clean, V::NoSeed, V::SanFail, V::Hypothesis, V::Reads};
    return k;
}

std::string_view name(Verdict v) { return kNames[static_cast<std::size_t>(v)]; }

std::optional<Verdict> parse(std::string_view status) {
    for (std::size_t i = 0; i < kVerdictCount; ++i)
        if (kNames[i] == status) return static_cast<Verdict>(i);
    return std::nullopt;
}

bool is_proof(Verdict v) {
    return v == V::ProvedCertified || v == V::ProvedUnbounded || v == V::Proved ||
           v == V::ProvedAssuming;
}

bool is_formal(Verdict v) { return is_proof(v) || v == V::Bounded; }

bool is_answered(Verdict v) { return is_formal(v) || v == V::Failed; }

bool no_answer(Verdict v) {
    return v == V::Unknown || v == V::Timeout || v == V::Error || v == V::NoFunc ||
           v == V::NotRun || v == V::NeedsHarness;
}

bool is_defect(Verdict v) { return v == V::Failed || v == V::Crash || v == V::SanFail; }

bool is_model(Verdict v) { return v == V::Hypothesis || v == V::Reads; }

int rank(Verdict v) {
    switch (v) {
        case V::ProvedCertified: return 5;
        case V::ProvedUnbounded: return 4;
        case V::Proved: return 3;
        case V::ProvedAssuming: return 2;
        case V::Bounded: return 1;
        default: return 0;
    }
}

std::string_view name(Refusal r) {
    switch (r) {
        case Refusal::None: return "none";
        case Refusal::Formal: return "formal";
        case Refusal::PromoteFuzz: return "promote-fuzz";
        case Refusal::PromoteModel: return "promote-model";
        case Refusal::NotRunClean: return "notrun-clean";
    }
    return "none";
}

Refusal merge_refusal(Verdict a, Verdict b) {
    if (a == b) return Refusal::None;
    if (is_formal(a) && is_formal(b)) return Refusal::Formal;
    if ((a == V::Clean && is_formal(b)) || (b == V::Clean && is_formal(a))) return Refusal::PromoteFuzz;
    if ((is_model(a) && is_formal(b)) || (is_model(b) && is_formal(a))) return Refusal::PromoteModel;
    if ((a == V::NotRun && (is_answered(b) || b == V::Clean)) ||
        (b == V::NotRun && (is_answered(a) || a == V::Clean)))
        return Refusal::NotRunClean;
    return Refusal::None;
}

bool may_rewrite(Verdict a, Verdict b) {
    if (a == b) return true;
    if (is_formal(b)) return is_formal(a) && rank(b) < rank(a);
    if (is_formal(a)) return b == V::Unknown || b == V::Error;
    if (no_answer(a)) return no_answer(b);
    return true;
}

const std::array<Origin, kOriginCount>& all_origins() {
    static constexpr std::array<Origin, kOriginCount> k{
        Origin::Solver, Origin::ExternalProver, Origin::Fuzzer, Origin::Model,
        Origin::Lint, Origin::Execution, Origin::Pipeline};
    return k;
}

std::string_view name(Origin o) {
    switch (o) {
        case Origin::Solver: return "solver";
        case Origin::ExternalProver: return "external-prover";
        case Origin::Fuzzer: return "fuzzer";
        case Origin::Model: return "model";
        case Origin::Lint: return "lint";
        case Origin::Execution: return "execution";
        case Origin::Pipeline: return "pipeline";
    }
    return "pipeline";
}

bool may_prove(Origin o) { return o == Origin::Solver || o == Origin::ExternalProver; }

Verdict admit(Origin origin, Verdict requested, bool certificate_checked) {
    if (!is_formal(requested)) return requested;
    if (!may_prove(origin)) return V::Unknown;
    if (requested == V::ProvedCertified)
        return (origin == Origin::Solver && certificate_checked) ? V::ProvedCertified : V::Proved;
    return requested;
}

const std::array<std::string_view, 31>& audit_stages() { return kStages; }

Origin stage_origin(std::string_view stage) {
    for (std::size_t i = 0; i < kStages.size(); ++i)
        if (kStages[i] == stage) return kStageOrigin[i];
    return Origin::Pipeline;
}

AuditResult audit(std::string_view stage, Verdict status, bool certificate_checked) {
    Verdict r = admit(stage_origin(stage), status, certificate_checked);
    return {r, r != status};
}

double confidence(double visibility, double answer, double resolution) {
    if (!(visibility > 0.0)) return 0.0;  // Law 5: no visibility, no confidence (also NaN)
    return visibility * answer * resolution;
}

Score score_counts(long n_fun, long classified, long attempted, long answered, long resolved) {
    auto ratio = [](long n, long d) { return d == 0 ? 0.0 : static_cast<double>(n) / static_cast<double>(d); };
    Score s{};
    s.visibility = ratio(classified, n_fun);
    s.answer = ratio(answered, attempted);
    s.resolution = ratio(resolved, answered);
    s.confidence = confidence(s.visibility, s.answer, s.resolution);
    return s;
}

}  // namespace prism::verdict
