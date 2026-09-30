#pragma once

// PRISM as an SV-COMP verifier (roadmap 6.3): `prism svcomp`.
//
//     prism svcomp [--allow-exec] [--data-model ILP32|LP64] [--out DIR]
//                  [--witness FILE] --prop PROPERTY.prp TASK.c
//
// runs the pipeline on one task with exactly the stages
// inventory,classify,bmc,pir (no LLM, no execution of task code), maps
// report.json to an SV-COMP answer, replays the counterexample and writes a
// witness (a violation witness for `false`, a correctness witness for
// `true`). It prints `PRISM-SVCOMP-RESULT: <answer>` where answer is `true`,
// `false(no-overflow)`, `false(unreach-call)`, `false(valid-deref)`,
// `false(valid-free)`, `unknown` or `error`, then `PRISM-SVCOMP-REASON: ...`.
//
// The mapping keeps PRISM's laws (docs/VERDICTS.md, docs/SVCOMP.md):
//
// - `true` only from PROVED, PROVED-UNBOUNDED or PROVED-CERTIFIED of main by
//   a verdict stage that covers the property (bmc/pir for no-overflow and
//   unreach-call; nothing covers the whole of valid-memsafety, so it is
//   never `true`), and only when no verdict stage reported a FAILED of the
//   property's class. PROVED-ASSUMING and BOUNDED are never `true` (Law 2).
// - `false(...)` only from a FAILED of the property's class whose
//   counterexample replays: the task is compiled with the matching
//   sanitizer and run in the sandbox on the counterexample's nondet values,
//   and the sanitizer (or reach_error) must fire. Replay executes task code,
//   so it needs --allow-exec (Law 9); without it every refutation is
//   `unknown`.
// - Everything else is `unknown`.
//
// `prism svcomp score` runs the pinned subset (tests/conformance/sv-comp)
// with SV-COMP points; `prism svcomp pack` builds the tool archive. The
// BenchExec tool-info module tools/svcomp/prism.py runs this subcommand.

#include "prism/export.hpp"
#include "prism/svcomp_witness.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::svcomp {

using json = nlohmann::json;

inline constexpr const char* STAGES = "inventory,classify,bmc,pir";
inline constexpr const char* RESULT_PREFIX = "PRISM-SVCOMP-RESULT: ";
inline constexpr const char* REASON_PREFIX = "PRISM-SVCOMP-REASON: ";
inline constexpr const char* WITNESS_PREFIX = "PRISM-SVCOMP-WITNESS: ";

// ---------------------------------------------------------------- property / task
// "no-overflow", "unreach-call", "valid-memsafety" or "unsupported".
PRISM_API std::string parse_property(const std::string& prp_text);
PRISM_API bool supported_property(const std::string& prop);
// Does a function body use a type whose width differs between ILP32 and
// LP64 (long, size_t, sizeof, ... or a typedef / struct built from one)?
// Unused declarations from preprocessed headers do not count.
PRISM_API bool width_dependent_code(const std::string& source);

// ---------------------------------------------------------------- decision
struct Decision {
    std::string answer;  // true | false(<prop>) | unknown | error
    std::string reason;
    std::optional<json> finding;  // the deciding finding, with "stage" added
    json replay = json::object();
};

// Findings about main per verdict stage, in report order (plus a
// STAGE-FAILED row for a crashed stage).
PRISM_API std::vector<std::pair<std::string, std::vector<json>>> main_findings(const json& report);
// extra["prop"] (pir), else the message prefix (bmc: "shift31: INT-SHIFT-UB").
PRISM_API std::string finding_prop(const json& finding);
PRISM_API bool refutes(const json& finding, const std::string& prop);
// replay(finding) replays one refutation; its "replay" key must be
// "replayed" for the refutation to count.
PRISM_API Decision decide(const json& report, const std::string& prop,
                          const std::function<json(const json&)>& replay);

// ---------------------------------------------------------------- replay
// The nondet values a refutation of main reads, in call order, from
// extra["nondet"] ("fn=value, ..."); nullopt when not reported, not a
// finding of main, or unreadable.
PRISM_API std::optional<std::vector<std::pair<std::string, Num>>> nondet_trace(const json& finding);
// Definitions of the __VERIFIER_* functions the task uses but does not
// define; nondet functions return the values in order. Throws
// std::invalid_argument for an unsupported nondet type.
PRISM_API std::string stub_source(const std::string& source, const std::vector<Num>& nondet_values);
// Compile and run the task on the counterexample; the violation must show.
// {"replay": "replayed"|"not-replayed"|"notrun"|"unsupported", ...}
PRISM_API json replay(const std::filesystem::path& src, const std::string& prop, bool allow_exec,
                      const std::optional<std::vector<Num>>& nondet_values, const std::filesystem::path& work,
                      double timeout_s = 10.0);

// ---------------------------------------------------------------- waypoints
using LineCol = std::pair<long, long>;
// (line, col) of each call in the nondet trace from extra["nondet_loc"]
// ("line:col, ..."; 0:0 is unknown, given as nullopt); nullopt when absent
// or when it does not match the trace entry for entry.
PRISM_API std::optional<std::vector<std::optional<LineCol>>> nondet_locations(const json& finding,
                                                                              std::size_t count);
// (line, column) of the `)` of a call of name that starts exactly at
// line:col (1-based), or nullopt.
PRISM_API std::optional<LineCol> call_at(const std::string& text, const std::string& name, long line, long col);
// function_return waypoints for the longest prefix of trace whose calls can
// each be placed exactly (see waypoints.cpp).
PRISM_API std::vector<NondetValue> nondet_waypoints(
    const std::filesystem::path& task, const std::vector<std::pair<std::string, Num>>& trace,
    std::optional<std::vector<std::optional<LineCol>>> locs = std::nullopt, bool physical = false);
// (line, column) of the reach_error() call the witness targets: the only
// call site, or the one on the finding's line; nullopt when ambiguous.
PRISM_API std::optional<LineCol> reach_error_call_site(const std::filesystem::path& src, std::optional<long> hint);
PRISM_API long first_code_column(const std::filesystem::path& src, long line);
// Column of a violation target waypoint for a sanitizer report at
// line:col, or nullopt to give no column.
PRISM_API std::optional<long> target_column(const std::filesystem::path& src, long line, long col);
PRISM_API bool decl_head(const std::string& text);

// ---------------------------------------------------------------- invariants
// Every subterm of the arithmetic expr stays within int for all values in
// bounds (variable -> [lo, hi], either side may be unknown).
using Bounds = std::vector<std::pair<std::string, std::pair<std::optional<long long>, std::optional<long long>>>>;
PRISM_API bool int_safe(const std::string& expr, const Bounds& bounds);
// The proved conjuncts a witness may carry (see invariants.cpp).
PRISM_API std::vector<std::string> exportable_conjuncts(const std::vector<std::string>& conjuncts);
// Loop invariants for a correctness witness of a `true` answer, and a note
// on where they came from.
PRISM_API std::pair<std::vector<Invariant>, std::string> correctness_invariants(const std::filesystem::path& task,
                                                                                const json& finding);

// ---------------------------------------------------------------- pir invariants -> C
// A C variable, its signedness and width in bits.
struct CVar {
    std::string name;
    bool is_signed = false;
    int bits = 0;
};
// C text of one proved pir conjunct, or nullopt when C would mean
// something else (signedness, promotion, wrap-around).
PRISM_API std::optional<std::string> render_conjunct(const json& conj,
                                                     const std::function<std::optional<CVar>(const json&)>& term_of);
// The task's IR as the pir stage builds it, with full debug information.
PRISM_API std::optional<std::string> debug_ir(const std::filesystem::path& task);
// C conjuncts per exported loop of a pir PROVED-UNBOUNDED finding (aligned
// with extra["invariant_loops"]); nullopt when unavailable (no clang/opt,
// no debug information, a task with line markers).
PRISM_API std::optional<std::vector<std::vector<std::string>>> pir_invariant_texts(
    const std::filesystem::path& task, const json& finding);

// ---------------------------------------------------------------- one task
struct SolveOptions {
    bool allow_exec = false;
    std::string data_model = "LP64";
    std::filesystem::path out = "prism-svcomp-out";
    std::optional<std::filesystem::path> witness = std::filesystem::path("witness.yml");
};
struct Outcome {
    Decision decision;
    std::optional<std::filesystem::path> witness_path;
};
PRISM_API Outcome solve(const std::filesystem::path& task, const std::filesystem::path& prop_file,
                        const SolveOptions& opt);
PRISM_API std::string version_string();

// ---------------------------------------------------------------- scoring the subset
struct SubsetTask {
    std::string id;  // path of the .yml below the suite
    std::filesystem::path yml, input, property_file;
    bool expected = false;
    std::string data_model;
};
// Tasks of the suite with an expected verdict for prop_name ("no-overflow.prp").
PRISM_API std::vector<SubsetTask> subset_tasks(const std::filesystem::path& suite, const std::string& prop_name);
// SV-COMP points: correct true +2, correct false +1, incorrect true -32,
// incorrect false -16, anything else 0. ("correct"|"wrong"|"unknown", points)
PRISM_API std::pair<std::string, int> score(bool expected, const std::string& answer);
PRISM_API std::string score_markdown(const ojson& rows, const ojson& meta);

// ---------------------------------------------------------------- archive
struct PackResult {
    bool ok = false;
    std::string error;
    ojson manifest;
};
// Build the tool archive: a tarball that unpacks to prism/ (the binary,
// the BenchExec tool-info module, README.md, fm-tools.yml, LICENSE,
// MANIFEST.json, dependencies.json).
PRISM_API PackResult pack(const std::filesystem::path& prism_bin, const std::filesystem::path& repo,
                          const std::filesystem::path& out);

// `prism svcomp ...` (argv after "svcomp").
PRISM_API int svcomp_main(int argc, char** argv);

}  // namespace prism::svcomp
