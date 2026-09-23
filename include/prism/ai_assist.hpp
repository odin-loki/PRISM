#pragma once

// PRISM AI layer, roadmap 9.1 / 9.3 / 9.4 "useful" and "convenience" features
// (C++ engine only, decision D8). Every feature here has a deterministic core
// that works without a model; a model can only add proposals on top, and
// nothing in this header can change a verdict:
//
//   * regression tests  (9.3) FAILED/CRASH counterexamples -> unit tests in
//                              the project's framework (src/prism/ai/regress.cpp)
//   * triage + dedup    (9.3) root-cause clusters that order findings only
//                              (src/prism/ai/triage.cpp)
//   * questions         (9.4) natural language -> a structured, printed query
//                              (src/prism/ai/ask.cpp)
//   * report drafting   (9.4) prose where every claim links to a finding,
//                              verdict, theorem or certificate; unlinked
//                              claims are rejected (src/prism/ai/draft.cpp)
//
// docs/AI.md documents each feature and its measured metric (roadmap 9.7).

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::ai {

// ---------------------------------------------------------------- finding ids
// A finding id is "<stage>#<index within the stage>" (report.json order).
// Triage, ask and drafting all speak in these ids.
struct FindingRef {
    std::string id;
    const Finding* f = nullptr;
};
PRISM_API std::vector<FindingRef> enumerate_findings(const RunReport& report);
PRISM_API const Finding* find_by_id(const RunReport& report, const std::string& id);

// ---------------------------------------------------------------- regression tests (9.3)
struct RegressOptions {
    std::filesystem::path out_dir;      // <out>/regression_tests or --write-tests DIR
    std::string framework = "auto";     // auto | ctest | gtest | catch2 | plain | pytest
    bool relative_includes = false;     // #include the source relative to out_dir (--write-tests)
    bool run = false;                   // compile + run each test now (needs allow_exec, Law 9)
    bool allow_exec = false;
    std::string cc;                     // "" = clang, else gcc (probed)
    std::string cxx;
    double timeout_s = 30.0;
};
struct RegressTest {
    std::string name;          // test name (identifier)
    std::string finding_id;    // stage#n
    std::string file;          // source file of the function (as in report.json)
    std::string function;
    std::string cls;
    std::string args;          // "x=2147483647, y=1" (C literals, in parameter order)
    std::string harness;       // path of the generated harness (relative to out_dir)
    std::string flags;         // sanitizer build flags (UBSan+ASan; MSan for uninitialised reads)
    // written | unsupported | reproduces | does-not-reproduce | compile-error | NOTRUN
    std::string status;
    std::string detail;
};
struct RegressResult {
    std::string framework;               // the detected / requested framework
    std::string framework_evidence;      // why (file that decided it)
    std::vector<RegressTest> tests;
    std::filesystem::path manifest;      // <out_dir>/manifest.json
};
// Framework of the project under root: gtest | catch2 | ctest | pytest | plain,
// with the file that decided it in *evidence.
PRISM_API std::string detect_framework(const std::filesystem::path& root, std::string* evidence = nullptr);
// Parses "a=1, b=#x0000000f, c=-2" (bmc/pir/concolic spelling) into name -> raw integer
// bit pattern / value. Unparseable entries are ignored.
PRISM_API std::vector<std::pair<std::string, std::string>> parse_counterexample(const std::string& cex);
// C literal of value `raw` (a cex value) in C type `type`; nullopt when the type
// is not a supported scalar.
PRISM_API std::optional<std::string> c_literal_for(const std::string& type, const std::string& raw);
PRISM_API RegressResult generate_regression_tests(const RunReport& report, const RegressOptions& opt);
// `prism regress --report out/report.json [--write-tests DIR] [--framework F] [--run --allow-exec]`
PRISM_API int regress_main(int argc, char** argv);

// ---------------------------------------------------------------- triage + dedup (9.3)
// Optional embedding model (roadmap 9.1: a small code embedding model through
// llama.cpp's /embedding endpoint, PRISM_EMBED_SERVER). Without it the
// deterministic TF-IDF embedding is used, and triage.json says so.
class PRISM_API Embedder {
public:
    virtual ~Embedder();
    virtual std::string name() const = 0;
    virtual std::optional<std::vector<double>> embed(const std::string& text) = 0;
};
PRISM_API std::shared_ptr<Embedder> connect_embedder(std::string* why);
PRISM_API void set_embedder_for_testing(std::shared_ptr<Embedder> e);  // nullptr: back to env

struct TriageOptions {
    double threshold = 0.7;       // cosine similarity for a union-find edge (docs/AI.md "Triage")
    bool use_embedder = true;     // try the embedding model when one is reachable
    std::filesystem::path root;   // source root for snippets ("" = report.root)
};
struct TriageCluster {
    std::string id;                        // C1, C2, ... in rank order
    int rank = 0;
    std::vector<std::string> members;      // finding ids, representative first
    std::string representative;
    std::string top_status;                // most severe member status
    std::vector<std::string> stages;       // distinct stages that report it
    std::string label;                     // cls / function summary
    double cohesion = 1.0;                 // mean similarity to the representative
};
struct TriageResult {
    std::vector<TriageCluster> clusters;
    std::string embedder;                  // "tfidf-char3+token" or "tfidf+<model>"
    std::string embedder_note;             // why no model (NOTRUN reason) when none
    double threshold = 0.0;
    std::size_t considered = 0;            // findings that were clustered
};
// Findings clustered: FAILED, CRASH, SANFAIL, ERROR, UNKNOWN, TIMEOUT,
// NEEDS-HARNESS, BOUNDED, HYPOTHESIS. Proofs, CLEAN and NOTRUN are not
// defects to triage. Never modifies the report.
PRISM_API TriageResult triage(const RunReport& report, const TriageOptions& opt = {});
// Deterministic feature text and TF-IDF vectors (exposed for tests / tools).
PRISM_API std::string triage_text(const Finding& f, const std::string& snippet);
PRISM_API double cosine_tfidf(const std::string& a, const std::string& b,
                              const std::vector<std::string>& corpus);
PRISM_API std::string triage_json(const TriageResult& t);
PRISM_API std::string triage_markdown(const TriageResult& t, const RunReport& report);
// Writes <out>/triage.json and appends the "## Clusters" section to <out>/report.md.
PRISM_API void write_triage(const RunReport& report, const std::filesystem::path& out,
                            const TriageOptions& opt = {});

// ---------------------------------------------------------------- questions over findings (9.4)
struct Query {
    std::vector<std::string> stages;       // any of
    std::vector<std::string> statuses;     // any of (expanded groups: "proof", "unproved", ...)
    std::vector<std::string> cls;          // substring match, any of
    std::vector<std::string> strengths;    // any of
    std::string file_glob;                 // fnmatch-style, "" = any
    std::string function_glob;
    std::string text;                      // substring of message, "" = any
    std::string group_by;                  // "" | stage | status | cls | file | function
    bool count_only = false;
    int limit = 50;
};
PRISM_API std::string query_to_json(const Query& q);
PRISM_API std::optional<Query> query_from_json(const std::string& text, std::string* why = nullptr);
// Deterministic keyword grammar (docs/AI.md "Questions over findings").
// *unused receives the words the grammar did not use (shown to the user).
PRISM_API Query parse_question(const std::string& question, std::string* unused = nullptr);
PRISM_API bool glob_match(const std::string& pattern, const std::string& text);
PRISM_API std::vector<FindingRef> run_query(const RunReport& report, const Query& q);
struct AskResult {
    Query query;
    std::string translator;   // "grammar" or "llm:<backend>" (model translation, validated)
    std::string model_note;   // why the model was not used / was rejected
    std::string unused;       // words the grammar ignored
    std::vector<FindingRef> matches;
    std::string answer;       // human text (query shown first)
};
// use_model: try the open ai::Session backend (grammar-constrained JSON);
// its output is validated with query_from_json and falls back to the grammar.
PRISM_API AskResult ask_question(const RunReport& report, const std::string& question, bool use_model);
PRISM_API std::string ask_json(const AskResult& r);
// `prism ask "<question>" --report out/report.json [--json] [--no-llm]`
PRISM_API int ask_main(int argc, char** argv);

// ---------------------------------------------------------------- explain (GUI assistant)
// Plain explanation of a finding's verdict from the verdict vocabulary and the
// finding's recorded evidence (no model). Unknown id -> "".
PRISM_API std::string explain_finding(const RunReport& report, const std::string& id);
// Short description of the trusted base (docs/TRUSTED_BASE.md summary).
PRISM_API std::string explain_trusted_base();
// One assistant turn for the GUI chat panels: "explain <id>", "trusted base",
// or a question (ask_question). Returns the text to show.
PRISM_API std::string assistant_reply(const RunReport& report, const std::string& input, bool use_model);

// ---------------------------------------------------------------- report / assurance drafting (9.4)
// Link kinds: finding:<stage#n>  verdict:<stage#n> (a proof/FAILED status)
//             theorem:<Lean name> (declared under proofs/)  certificate:<stage#n>
//             (extra.certificate == checked)  stage:<name>
struct Claim {
    std::string text;
    std::vector<std::string> links;
};
struct DraftSection {
    std::string title;
    std::vector<Claim> claims;
};
struct Draft {
    std::string kind;       // report | assurance
    std::string author;     // "template" or "llm:<backend>"
    std::vector<DraftSection> sections;
};
struct DraftCheck {
    bool ok = true;
    std::vector<std::string> rejected;   // "<section>: <claim text>: <why>"
    std::size_t claims = 0;
    std::size_t links = 0;
};
// Theorem names declared in proofs/**/*.lean (`theorem X` / `lemma X`, with
// the enclosing namespaces), for theorem: links.
PRISM_API std::vector<std::string> lean_theorems(const std::filesystem::path& proofs_dir);
PRISM_API Draft draft_template(const RunReport& report, const std::string& kind,
                               const std::vector<std::string>& theorems);
// Every claim needs >= 1 link, and every link must resolve against the
// report / theorem list. Rejected claims are listed and dropped by
// render_draft (they never reach the rendered text).
PRISM_API DraftCheck validate_draft(const Draft& d, const RunReport& report,
                                    const std::vector<std::string>& theorems);
PRISM_API std::string render_draft(const Draft& d, const DraftCheck& check);
// Model-assisted: the model may reword claims (grammar draft.gbnf: JSON list
// of {"text","links"}); the result is validated like any draft; with no model
// the template draft is returned and *note says why.
PRISM_API Draft draft_with_model(const RunReport& report, const std::string& kind,
                                 const std::vector<std::string>& theorems, std::string* note);
// `prism draft --report out/report.json [--kind report|assurance] [--proofs DIR] [--out FILE]`
PRISM_API int draft_main(int argc, char** argv);

}  // namespace prism::ai
