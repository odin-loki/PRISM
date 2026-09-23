#pragma once

// PRISM AI layer, second part (roadmap 9.2 / 9.3 / 4.2), C++ engine only (D8):
//
//   * Lean proof search (9.2): a prover model proposes tactic steps or whole
//     proofs for a `sorry` in a Lean theorem; the Lean kernel checks every
//     candidate (`lake env lean`), `#print axioms` must stay within
//     {propext, Classical.choice, Quot.sound}, and a proof is accepted only
//     after `lake build` of the module succeeds (in a scratch copy of the
//     project; written back to the source only with --write).
//   * Proof repair (9.2): a proof store records, per function, a fingerprint
//     of its body and the artefacts that proved it (invariants, contracts,
//     harness assumptions). A changed function whose artefacts no longer
//     re-check is a PROOF-REGRESSION (UNKNOWN, extra.regression = true); the
//     model may propose repairs, which are re-checked before acceptance.
//   * Contract drafting (4.2) and contracts from requirements (9.3): drafted
//     requires/ensures carry a trace link to the sentence they came from and
//     stay HYPOTHESIS until a human approves them (contracts.approved.json);
//     only approved contracts can yield PROVED-ASSUMING.
//   * Assumption auditing (9.3): a deterministic Z3 vacuity check
//     (VACUOUS-ASSUMPTION, works without a model) and an independent model
//     pass whose flags are READS findings, never verdict changes.
//
// The rule from ai.hpp holds: the model proposes, a checker decides, every
// model call is in <out>/ai_audit.jsonl.

#include "prism/ai.hpp"
#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::ai {

// =================================================================== Lean proof search
inline constexpr const char* LEAN_ALLOWED_AXIOMS[] = {"propext", "Classical.choice", "Quot.sound"};

// A `sorry` inside one theorem of a Lean file.
struct LeanTarget {
    std::filesystem::path project;   // directory holding lakefile.toml / lakefile.lean
    std::filesystem::path file;      // the .lean file (absolute)
    std::string module;              // Lean module name (Prism.Verdict), "" when outside the lib root
    std::string theorem;             // declaration name as written (t, Foo.bar)
    std::string statement;           // "theorem t (a b : Nat) : a + b = b + a"
    std::size_t decl_begin = 0;      // byte offsets in the file text
    std::size_t proof_begin = 0;     // start of the replaced span (just after ":=")
    std::size_t proof_end = 0;       // end of the replaced span (just after "sorry")
    std::string indent = "  ";
};
// nullopt + why when the theorem is missing, has no sorry, or more than one.
PRISM_API std::optional<LeanTarget> find_lean_target(const std::filesystem::path& file,
                                                     const std::string& theorem, std::string* why = nullptr);
// Theorems with a `sorry` in their proof (comments ignored), for the driver.
PRISM_API std::vector<std::string> lean_sorry_theorems(const std::filesystem::path& file);

// Post-decoding validation of a model proof: {"tactics": [str, ...]} with
// 1..40 lines of at most 400 characters; no command or escape hatch
// (sorry, admit, native_decide, #eval, set_option, axiom, run_tac, ...).
PRISM_API Validated validate_lean_tactics(const std::string& raw);

// The file text with the target's sorry replaced by `by` + tactics, plus
// `#print axioms <theorem>` directly after the declaration.
PRISM_API std::string splice_proof(const std::string& text, const LeanTarget& t,
                                   const std::vector<std::string>& tactics, bool print_axioms);

struct LeanCheck {
    bool ran = false;               // lean could be started
    bool complete = false;          // no error at all: proof closes
    bool only_unsolved = false;     // the only errors are "unsolved goals" (valid prefix)
    std::string errors;             // error text (trimmed), fed back to the model
    std::string goals;              // goal state from "unsolved goals" (or the first error)
    int goal_count = 0;
    std::vector<std::string> axioms;  // from #print axioms; empty = none
    bool axioms_seen = false;
    bool axioms_ok = false;
    std::string log;
};
// Parses `lean` output for the spliced candidate.
PRISM_API LeanCheck parse_lean_output(const std::string& out, const std::string& theorem, int rc);

struct LemmaEntry {
    std::string theorem, file, statement, proof, model, audit_id;
};
PRISM_API std::vector<LemmaEntry> read_lemmas(const std::filesystem::path& path);
PRISM_API void append_lemma(const std::filesystem::path& path, const LemmaEntry& e);

struct ProveOptions {
    std::filesystem::path file;
    std::string theorem;
    std::filesystem::path project;          // "" = nearest parent with a lakefile
    bool write = false;                     // write the accepted proof back into `file`
    std::filesystem::path lemmas;           // "" = <project>/lemmas.jsonl if in proofs/, else proofs/lemmas.jsonl
    int budget = 24;                        // model calls
    int beam = 4;                           // partial proofs kept
    double lean_timeout = 120.0;
    std::filesystem::path out{"prism-out"};
    std::string prover_server;              // llama-server URL ($PRISM_PROVER_SERVER)
    std::filesystem::path prover_gguf;      // GGUF to serve ($PRISM_PROVER_GGUF)
    std::string prover_model;               // name recorded in the audit ($PRISM_PROVER_MODEL)
    std::shared_ptr<ModelBackend> backend;  // tests: injected prover (else resolved from the above)
};

struct ProveNode {
    std::vector<std::string> tactics;
    std::string goals;
    int goal_count = 0;
    std::string last_error;
    int depth = 0;
};

struct ProveResult {
    std::string status;                     // PROVED (kernel + axioms + lake build) | FAILED | NOTRUN | ERROR
    std::string reason;
    std::vector<std::string> proof;         // accepted tactics
    std::vector<std::string> axioms;
    bool written = false;
    int model_calls = 0;
    int kernel_checks = 0;
    int rejected_invalid = 0;               // grammar/validator rejections
    int rejected_kernel = 0;                // Lean errors
    int rejected_axioms = 0;
    std::vector<std::string> attempts;      // one line per candidate: verdict + first error line
    std::string audit_id;                   // record of the accepted candidate
    std::string model;
};
PRISM_API ProveResult prove_theorem(const ProveOptions& opt);
// `prism prove FILE THEOREM [...]` (argv[0] == "prove"). Exit 0 proved, 1 not
// proved, 3 NOTRUN (no prover / no Lean), 2 usage or I/O error.
PRISM_API int prove_main(int argc, char** argv);

// The lake executable ($PRISM_LAKE, PATH, ~/.elan/bin/lake); nullopt = none.
PRISM_API std::optional<std::filesystem::path> find_lake();

// =================================================================== proof store / repair
struct ProofArtefact {
    std::string stage;         // bmc | contracts | harness | wp | pir | regress
    std::string status;        // the proof-class status it earned
    std::string invariants;    // JSON list per loop (extra.invariants)
    std::string requires_;     // contract requires
    std::string ensures;       // contract ensures
    std::string assumptions;   // JSON list (harness)
    std::string source;        // template | llm:<model> | user
    int unwind = 0;
};
struct ProofEntry {
    std::string file, function, fingerprint;
    std::vector<ProofArtefact> artefacts;
};
// sha256 of the signature and the body with comments and whitespace removed.
PRISM_API std::string function_fingerprint(const FunctionInfo& fn);
PRISM_API std::vector<ProofEntry> load_proof_store(const std::filesystem::path& path);
PRISM_API void save_proof_store(const std::filesystem::path& path, const std::vector<ProofEntry>& entries);
// $PRISM_PROOF_CACHE, else $XDG_CACHE_HOME/prism/proofs, else ~/.cache/prism/proofs;
// file <dir>/<sha(root)[:16]>.json.
PRISM_API std::filesystem::path persistent_store_path(const std::filesystem::path& root);
// Re-checks one stored artefact against the current function (BMC/Houdini).
// Returns the checked finding (proof status on success).
PRISM_API Finding recheck_artefact(const FunctionInfo& fn, const ProofArtefact& a, int unwind);
// The `regress` stage: compare the current run with the stored proofs,
// re-check stored artefacts of functions the current run does not prove,
// report PROOF-REGRESSION, ask the model for repairs, save the new store.
PRISM_API std::vector<Finding> run_proof_regression(const std::vector<FunctionInfo>& functions,
                                                    const std::vector<StageResult>& stages, const Config& cfg);

// =================================================================== contracts (4.2, 9.3)
struct Requirement {
    std::string id;     // R1, R2, ...
    std::string file;   // path as given
    int line = 0;
    std::string text;   // one sentence
};
// Markdown / text files (or directories of them) split into sentences.
PRISM_API std::vector<Requirement> load_requirements(const std::vector<std::filesystem::path>& paths);
// Clause hash used by contracts.approved.json: sha256(function "\n" clause)[:16],
// clause normalised to "requires <expr>;" / "ensures <expr>;".
PRISM_API std::string clause_hash(const std::string& function, const std::string& clause);
// Approved clause hashes from the approval file (entries whose recorded hash
// does not match their function + clause are ignored: stale approvals).
PRISM_API std::vector<std::string> load_approvals(const std::filesystem::path& path);
// "a ==> b" to C, "\result" to "result" (the contracts engine's spelling).
PRISM_API std::string contract_to_c(const std::string& expr);
// Caller check: every call of `callee` in `caller` satisfies `requires_expr`
// (parameters substituted by the call's arguments). "satisfies" | "violates: <model>" | "unknown: <why>".
PRISM_API std::string check_callers_requires(const FunctionInfo& caller, const FunctionInfo& callee,
                                             const std::string& requires_expr, int unwind);
// Contracts drafted by the model for spec-less functions, proved by the
// contracts engine; HYPOTHESIS unless every clause is approved. One NOTRUN
// row without a model.
PRISM_API std::vector<Finding> draft_contracts(const std::vector<FunctionInfo>& functions, const Config& cfg);

// =================================================================== assumption audit (9.3)
// Deterministic: is the conjunction of `clauses` (C expressions over fn's
// integer parameters) satisfiable? "sat" | "unsat" | "unknown: <why>".
PRISM_API std::string assumptions_satisfiable(const FunctionInfo& fn, const std::vector<std::string>& clauses);
// VACUOUS-ASSUMPTION findings for user `// requires:` of every function and
// for the requires/assumptions recorded on `findings` (contracts, harness).
PRISM_API std::vector<Finding> vacuity_audit(const std::vector<FunctionInfo>& functions,
                                             const std::vector<Finding>& findings, const std::string& stage);
// Independent model pass over the assumptions on `findings`: READS flags.
// One NOTRUN row without a model; never changes a verdict.
PRISM_API std::vector<Finding> model_assumption_audit(const std::vector<FunctionInfo>& functions,
                                                      const std::vector<Finding>& findings,
                                                      const std::string& stage);

// Helpers shared with the tests.
PRISM_API std::vector<std::string> split_conjuncts(const std::string& expr);
PRISM_API Validated validate_assumption_audit(const std::string& raw, std::size_t n);

// =================================================================== the review stage
// Runs after harness: vacuity audit (Z3, no model), approved and drafted
// contracts, the model assumption audit, and the proof store / regression
// check. One NOTRUN row names the model features that could not run.
PRISM_API std::vector<Finding> run_review(const std::vector<FunctionInfo>& functions,
                                          const std::vector<StageResult>& stages, const Config& cfg);

}  // namespace prism::ai
