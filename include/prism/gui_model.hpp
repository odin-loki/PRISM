#pragma once

// Qt-free model of the PRISM window (src/gui). Everything the window shows
// is computed here, so it is tested headless in prism_tests; src/gui only
// copies these rows into widgets.
//
// The window never invents a verdict: statuses are copied verbatim from
// report.json, CLEAN is never proof-green, a missing report is confidence 0
// (never n/a, never a proof) and an LLM hypothesis never covers a class.

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::gui {

inline constexpr std::string_view WINDOW_TITLE = "PRISM — hybrid code testing";
// CLAUDE.md: the GUI writes to prism-out-gui/, the CLI to prism-out/.
inline constexpr std::string_view DEFAULT_OUT = "prism-out-gui";
inline constexpr std::array<std::string_view, 7> FINDING_COLUMNS{
    "status", "stage", "cls", "file", "line", "function", "message"};
inline constexpr std::array<std::string_view, 5> STAGE_COLUMNS{
    "stage", "status", "records", "seconds", "note"};
// Empty scope / missing report.json: 0, never n/a, never a proof.
inline constexpr std::string_view MISSING_REPORT_LABEL =
    "confidence 0  (report.json missing; not a proof)";
// Journal poll period while a run is in flight (milliseconds).
inline constexpr int POLL_MS = 400;
// Interactive budgets: a window run is a quick look, not a release gate.
// They only lower fuzz/repair effort (nothing is disabled) and are passed
// on the logged command line; a budget flag given at launch wins.
inline constexpr std::string_view GUI_FUZZ_BUDGET = "4";
inline constexpr std::string_view GUI_FUZZ_ITERS = "256";
inline constexpr std::string_view GUI_REPAIR_ROUNDS = "1";

// Skip list from the three "Skip:" checkboxes, in this order.
PRISM_API std::vector<std::string> skip_from_checks(bool fuzz, bool repair, bool optional);

struct Confidence {
    double visibility = 0;
    double answer = 0;
    double resolution = 0;
    double confidence = 0;
};

// A confidence field as text ("0.5", "n/a", "", "null", "NaN"): a number
// when it parses, else 0. Empty scope is 0, never a string placeholder.
PRISM_API double confidence_number(std::string_view text);
PRISM_API double confidence_number(double value);  // NaN / inf -> 0
PRISM_API Confidence confidence_product(const RunReport& report);
// Same fields read from report.json text: numbers, numeric strings and
// n/a / null / missing (0). Unparsable JSON is all 0.
PRISM_API Confidence confidence_from_json(std::string_view report_json);
// A number the way `prism PATH` prints it (std::ostream default format).
PRISM_API std::string format_number(double value);
// The confidence line `prism PATH` prints:
// "confidence C  (vis V x ans A x res R)".
PRISM_API std::string confidence_label(const Confidence& c);
PRISM_API std::string confidence_label(const RunReport& report);

struct FindingRow {
    std::string status, stage, cls, file, line, function, message;
    std::string id;  // "<stage>#<index>" as prism ask / explain number them
    // Cell text by column name (FINDING_COLUMNS).
    PRISM_API const std::string& column(std::string_view name) const;
};
// Rows with the CLI finding vocabulary. Statuses are copied verbatim.
// CLEAN/NOTRUN rows of inventory/classify/unify are noise (as in report.md).
// Messages are cut to 200 characters (never inside a UTF-8 sequence).
PRISM_API std::vector<FindingRow> finding_rows(const RunReport& report);

struct TaxonomyRow {
    std::string id, verdict, best;
};
// COVERED / PARTIAL / GAP recomputed from the report's findings (never from
// a stale taxonomy.json). CLEAN or any other verdict is GAP; the LLM cannot
// cover (refuse_llm_cover).
PRISM_API std::vector<TaxonomyRow> taxonomy_rows(const RunReport& report);

struct StageRow {
    std::string stage, status, records, seconds, note;
};
PRISM_API StageRow stage_row(const StageResult& s);
PRISM_API std::vector<StageRow> stage_rows(const std::vector<StageResult>& stages);

// Palette (hex "#rrggbb", "" = no colour). CLEAN is blue, never proof-green;
// COVERED reuses PROVED green, PARTIAL BOUNDED olive, GAP NOTRUN brown.
PRISM_API std::string status_background(std::string_view status);
PRISM_API std::string taxonomy_background(std::string_view verdict);
PRISM_API bool is_proof_green(std::string_view hex);

// Live progress from <out>/stages.jsonl. Without --resume the CLI resets
// the journal, so rows older than the run's start belong to the previous
// run and are not shown; with --resume reused rows are part of this run.
PRISM_API std::vector<StageResult> live_stages(const std::vector<StageResult>& journal,
                                               double run_started, bool resume);
// "<stage> <status> (<records>)"
PRISM_API std::string progress_line(const StageResult& s);
// Logs one progress line each time the last journal row changes.
class ProgressTracker {
public:
    PRISM_API std::optional<std::string> update(const std::vector<StageResult>& rows);
    void reset() { last_.clear(); }
private:
    std::string last_;
};

// "done. <confidence line>. NOTRUN=a,b" ("NOTRUN=none" when every stage ran).
PRISM_API std::string done_summary(const Confidence& c, const std::vector<StageResult>& stages);

// Why a finished CLI run is a failure the window must raise (a dialog), or
// nullopt. Exit 0 is a run; exit 1 is --fail-on reporting defects/gaps
// (a verdict, not a failure). A crash or any other code is a failure.
PRISM_API std::optional<std::string> run_failure(int exit_code, bool crashed);

// A missing display: NOTRUN finding (never CLEAN, never a fake window).
PRISM_API Finding missing_display_finding(const std::string& reason = {});
// What prism_gui prints (stdout, exit 0) when there is no display: the
// finding above as text, "NOTRUN gui: no display — not a clean window".
PRISM_API std::string notrun_display_text(const std::string& reason = {});
// DISPLAY / WAYLAND_DISPLAY / QT_QPA_PLATFORM (null = unset). Offscreen
// always has a display. Windows always has one.
PRISM_API bool display_available(const char* display, const char* wayland,
                                 const char* platform);

// Options of one window run; gui_run_args builds the `prism` argv (no argv[0]).
struct RunOptions {
    std::string path;
    std::string out;
    bool llm = true;
    bool resume = false;
    bool allow_exec = false;  // Law 9: off unless the user opts in
    bool skip_fuzz = false;
    bool skip_repair = false;
    bool skip_optional = false;
    std::vector<std::string> extra_skip;  // other --skip entries from the launch
    std::vector<std::string> extra;       // other scan flags from the launch, verbatim
};
PRISM_API std::vector<std::string> gui_run_args(const RunOptions& o);

// `prism_gui [PATH] [flags]` (also reached through `prism PATH --gui ...`).
// Every scan flag is kept: checkbox flags set their box, --out sets the
// output directory, the rest go to the CLI verbatim (a value-taking flag
// keeps its value, so a value is never taken as the scan path). Flags that
// do not describe a scan are listed in `ignored` and logged.
struct Launch {
    std::string path;
    std::string out;
    bool no_llm = false;
    bool resume = false;
    bool allow_exec = false;
    bool skip_fuzz = false;
    bool skip_repair = false;
    bool skip_optional = false;
    std::vector<std::string> extra_skip;
    std::vector<std::string> extra;
    std::vector<std::string> ignored;
    bool from_cli = false;
};
PRISM_API Launch parse_launch(const std::vector<std::string>& args);
// True for a CLI flag that consumes the next argument.
PRISM_API bool flag_takes_value(std::string_view flag);

// `prism ... --gui`: the argv forwarded to prism_gui (argv[0] = gui path,
// every user argument kept in order, --gui removed).
PRISM_API std::vector<std::string> gui_forward_args(const std::filesystem::path& gui,
                                                    const std::vector<std::string>& args);
// prism_gui beside the running binary (self_dirs, in order) or on PATH
// (`path_env`, ':'-separated; ';' on Windows). Empty = not found (NOTRUN).
PRISM_API std::filesystem::path find_prism_gui(
    const std::vector<std::filesystem::path>& self_dirs, std::string_view path_env);
PRISM_API std::filesystem::path gui_binary_name();
// Text `prism --gui` prints when prism_gui is missing (NOTRUN, exit 0) or
// cannot be started (ERROR, exit 2).
PRISM_API std::string notrun_missing_gui_text();
PRISM_API std::string error_spawn_gui_text(const std::string& detail);

}  // namespace prism::gui
