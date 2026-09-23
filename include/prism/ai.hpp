#pragma once

// PRISM AI layer (roadmap Part 4 / 9.2 / 9.3 / 9.6), C++ engine only (D8).
//
// The rule: the model proposes, the prover decides. Nothing in this module
// can raise a verdict on model output alone:
//   * invariants (template or model) are filtered by Houdini in Z3 and only
//     count when the strengthened induction step AND the base case close;
//   * drafted harness assumptions give PROVED-ASSUMING with every assumption
//     listed, never PROVED;
//   * explanations and fixes are HYPOTHESIS / READS; a fix is called
//     "verified fix" only when BMC proves the patched function.
// Every model interaction is appended to <out>/ai_audit.jsonl.

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::ai {

// ---------------------------------------------------------------- hashing
PRISM_API std::string sha256_hex(const std::string& data);
PRISM_API std::string sha256_file(const std::filesystem::path& path);  // "" when unreadable

// ---------------------------------------------------------------- grammars
// GBNF text shipped in grammars/<name>.gbnf (embedded verbatim; tests/test_ai.py
// locks the embedded copy to the file). Names: invariants, harness, contract, explain.
PRISM_API const std::string& grammar_text(const std::string& name);
// JSON schema used for Ollama's "format" parameter (same shape as the grammar).
PRISM_API std::string grammar_json_schema(const std::string& name);

// Post-decoding validation. A grammar constrains the sampler, but the decoded
// text is re-validated here; anything that fails is rejected (and logged).
struct Validated {
    bool ok = false;
    std::string reason;                 // why it was rejected
    std::vector<std::string> items;     // invariants: expressions
    std::string explanation;            // explain
    std::string fix_body;               // explain
    std::vector<std::pair<std::string, std::string>> pairs;  // harness/contract: (kind, text)
};
// A C boolean expression over `vars` only: identifiers, integer literals,
// arithmetic / comparison / logical operators, parentheses. No assignment,
// no calls, no increments, no casts, bounded length.
PRISM_API bool valid_c_bool_expr(const std::string& expr, const std::vector<std::string>& vars,
                                 std::string* why = nullptr);
PRISM_API Validated validate_invariants(const std::string& raw, const std::vector<std::string>& vars);
PRISM_API Validated validate_harness(const std::string& raw, const std::vector<std::string>& params);
PRISM_API Validated validate_contract(const std::string& raw, const std::vector<std::string>& vars);
PRISM_API Validated validate_explain(const std::string& raw);

// ---------------------------------------------------------------- prompts
// Source text in prompts is fenced and labelled untrusted: a comment in the
// analysed code is data, never an instruction.
PRISM_API std::string fence_untrusted(const std::string& source, const std::string& label);
PRISM_API std::string system_prompt(const std::string& task);

// ---------------------------------------------------------------- backends
struct ModelRequest {
    std::string feature;   // invariants | harness | contract | explain
    std::string system;
    std::string user;
    std::string grammar;       // grammar name (audit log, Ollama schema)
    std::string grammar_text;  // GBNF sent to llama-server (ident rule narrowed)
};
struct ModelReply {
    std::string text;
    std::string error;     // non-empty: no usable output
};
class PRISM_API ModelBackend {
public:
    virtual ~ModelBackend();
    virtual std::string name() const = 0;          // e.g. "llama-server:qwen3.5:9b"
    virtual std::string model_sha256() const = 0;  // "unknown" for HTTP backends
    virtual ModelReply complete(const ModelRequest& req) = 0;
};

// llama-server /completion with "grammar" (GBNF) and Ollama /api/generate
// with "format" (JSON schema). Returns nullptr (and a reason) when neither is
// reachable, or when --no-llm. The native in-process GGUF path has no grammar
// sampler wired, so it is not used for AI features (reason says so).
PRISM_API std::shared_ptr<ModelBackend> connect_backend(const Config& cfg, std::string* why);

// ---------------------------------------------------------------- session
// The pipeline opens one Session per run. Library callers without a session
// still get template invariants / template harnesses; model features are
// NOTRUN ("no AI session").
struct AuditRecord {
    std::string feature;
    std::string function;
    std::string file;
    std::string model;
    std::string model_sha256;
    std::string prompt_sha256;
    std::string grammar;
    std::string raw_output_sha256;
    bool output_valid = false;
    std::string rejected_reason;
    std::string checker;          // e.g. "z3-houdini+k-induction", "bmc(patched)"
    std::string checker_result;   // the checker's own status; "" = no checker ran
    std::string verdict_effect;   // "none" or the checked verdict
    std::string id;               // stable id referenced by findings (extra.ai_audit_id)
};

class PRISM_API Session {
public:
    explicit Session(const Config& cfg);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

// Current session state (nullptr config when none is open).
PRISM_API const Config* session_config();
// Backend of the open session (connects lazily, once). nullptr + why when unavailable.
PRISM_API std::shared_ptr<ModelBackend> session_backend(std::string* why);
// Tests only: inject a deterministic backend into the open session. The
// production binary never constructs a fake backend.
PRISM_API void set_session_backend_for_testing(std::shared_ptr<ModelBackend> backend);

// Calls the model and returns the reply; fills the model/prompt/output
// hashes of `rec`. Callers finish `rec` (validity, checker result) and append it.
PRISM_API ModelReply ask(ModelBackend& backend, const ModelRequest& req, AuditRecord& rec);
PRISM_API void audit_append(AuditRecord& rec);
// For model calls made outside ask() (the llm/fuzz/execute/repair stages'
// chat engine): fills id and hashes from the prompt and raw output, appends.
PRISM_API void audit_model_call(AuditRecord& rec, const std::string& prompt, const std::string& output);
PRISM_API std::filesystem::path audit_path();  // "" when no session

// ---------------------------------------------------------------- BMC primitive
// Implemented in src/prism/bmc.cpp (needs the encoder). Encodes `body` for
// `fn`'s parameters with the AI hook statements enabled:
//   __prism_assume(e);  __prism_assert(tag, e);  __prism_havoc(x);
//   __prism_step (c) { ... }   (runs the block under c, then continues under !c
//                               with the pre-block state: a loop cut)
// and checks every property separately.
struct ProgramProp {
    std::string name;    // "ai-inv#<tag>" for __prism_assert, else the UB property name
    std::string cls;
    std::string result;  // sat | unsat | unknown
    std::string model;   // "x=1, n=3" on sat
};
struct ProgramCheck {
    bool encoded = false;
    bool unwind_ok = true;
    std::string error;
    std::vector<ProgramProp> props;
};
// only_invariants: check just the ai-inv properties, model-guided (one solver
// call can refute many candidates); UB properties are reported "skipped".
PRISM_API ProgramCheck check_program(const FunctionInfo& fn, const std::string& body, int unwind,
                                     unsigned timeout_ms = 4000, bool only_invariants = false);

// ---------------------------------------------------------------- invariants
struct LoopCut {
    std::string kind;       // for | while | do
    std::string init, cond, incr, body;
    std::size_t begin = 0, end = 0;   // span of the loop statement in fn.body
    std::vector<std::string> havoc;   // scalars and arrays written in the loop
    std::vector<std::string> scalars; // candidate vocabulary at the loop head
    std::vector<std::string> modified;  // scalars in the vocabulary written in the loop
    std::vector<std::pair<std::string, int>> arrays;  // fixed-size arrays in scope
    std::vector<long long> constants;   // integer literals of the function
};
// Splits fn.body into loops that can be cut soundly (no nesting, break,
// continue, goto, switch, calls or pointer declarations). nullopt = refuse.
PRISM_API std::optional<std::vector<LoopCut>> loop_cuts(const FunctionInfo& fn, std::string* why = nullptr);
// The loop-cut program: every loop j replaced by
//   init; assert(I_j) [base]; havoc(written); assume(I_j);
//   step(cond) { body; incr; assert(I_j) [consecution] }
// with inv[j] = (tag, expression) pairs.
PRISM_API std::string cut_program(const FunctionInfo& fn, const std::vector<LoopCut>& loops,
                                  const std::vector<std::vector<std::pair<int, std::string>>>& inv);
// Deterministic template candidates for one loop.
PRISM_API std::vector<std::string> template_candidates(const FunctionInfo& fn, const LoopCut& loop);

struct HoudiniResult {
    bool proved = false;              // strengthened step AND base case closed, no UB property sat
    bool encoded = false;
    std::string why;
    std::vector<std::vector<std::string>> invariants;  // survivors per loop
    std::vector<std::vector<std::string>> sources;     // "template" | "llm:<model>" per survivor
    std::string cti;                  // counterexample to induction (UB property model) when open
    int rounds = 0;
};
// Houdini over candidates[j] for loop j, then the cut program's UB check.
PRISM_API HoudiniResult houdini(const FunctionInfo& fn, const std::vector<LoopCut>& loops,
                                std::vector<std::vector<std::string>> candidates,
                                std::vector<std::vector<std::string>> sources, int unwind);

// The bmc k-induction hook: called by bmc.cpp when k-induction leaves a
// function BOUNDED. Returns the strengthened PROVED-UNBOUNDED finding, or the
// input finding annotated with why strengthening did not close.
PRISM_API Finding strengthen_bounded(const FunctionInfo& fn, const Finding& bounded, int unwind);

// ---------------------------------------------------------------- harness drafting
struct HarnessDraft {
    FunctionInfo harnessed;                 // scalar-only function with drafted buffers
    std::vector<std::string> assumptions;   // human-readable, every one listed
    std::string source;                     // template | llm:<model>
};
// One draft per concrete size case (size range); empty = could not draft.
PRISM_API std::vector<HarnessDraft> draft_harness(const FunctionInfo& fn, std::string* why = nullptr);
// Runs the drafted harness through BMC. nullopt when no draft was possible
// (`refused` then says why: template reason, and the model note).
PRISM_API std::optional<Finding> drafted_harness_bmc(const FunctionInfo& fn, int unwind,
                                                     std::string* refused = nullptr);

// ---------------------------------------------------------------- explanation / repair
// For a FAILED finding: explanation (HYPOTHESIS/READS) and a proposed fix
// re-verified by BMC. NOTRUN rows without a model. Never changes the verdict.
PRISM_API std::vector<Finding> explain_failed(const Finding& fail, const Config& cfg);

}  // namespace prism::ai
