#include "prism/gui_model.hpp"

#include "prism/laws.hpp"
#include "prism/taxonomy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace prism::gui {

namespace {

bool is_noise_stage(std::string_view name) {
    return name == "inventory" || name == "classify" || name == "unify";
}

// First `n` characters of UTF-8 text (continuation bytes do not count).
std::string utf8_left(const std::string& s, std::size_t n) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            if (chars == n) return s.substr(0, i);
            ++chars;
        }
    }
    return s;
}

std::string trim_lower(std::string_view t) {
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.remove_prefix(1);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.remove_suffix(1);
    std::string s(t);
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

double json_number(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return 0;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number()) return confidence_number(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    if (it->is_string()) return confidence_number(it->get<std::string>());
    return 0;
}

// Launch flags that are not a scan (they print and exit, or are
// maintainer tooling). The window lists them as ignored.
bool non_scan_flag(std::string_view a) {
    return a == "--version" || a == "-V" || a == "-h" || a == "--help" ||
           a == "--list-stages" || a == "--z3-only";
}
bool non_scan_value_flag(std::string_view a) {
    return a == "--pir-vcs" || a == "--solve-smt2";
}

bool file_is_exe(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec) return false;
#ifndef _WIN32
    return ::access(p.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

}  // namespace

std::vector<std::string> skip_from_checks(bool fuzz, bool repair, bool optional) {
    std::vector<std::string> skip;
    if (fuzz) skip.emplace_back("fuzz");
    if (repair) skip.emplace_back("repair");
    if (optional) skip.emplace_back("optional");
    return skip;
}

double confidence_number(double value) {
    return std::isfinite(value) ? value : 0.0;
}

double confidence_number(std::string_view text) {
    const std::string s = trim_lower(text);
    if (s.empty() || s == "n/a" || s == "na" || s == "none" || s == "null") return 0.0;
    double v = 0;
    const char* b = s.data();
    const char* e = s.data() + s.size();
    if (*b == '+') ++b;
    auto [p, ec] = std::from_chars(b, e, v);
    if (ec != std::errc() || p != e) return 0.0;
    return confidence_number(v);
}

Confidence confidence_product(const RunReport& report) {
    return {confidence_number(report.visibility), confidence_number(report.answer),
            confidence_number(report.resolution), confidence_number(report.confidence)};
}

Confidence confidence_from_json(std::string_view report_json) {
    auto j = nlohmann::json::parse(report_json, nullptr, false);
    if (j.is_discarded()) return {};
    return {json_number(j, "visibility"), json_number(j, "answer"),
            json_number(j, "resolution"), json_number(j, "confidence")};
}

std::string format_number(double value) {
    std::ostringstream o;
    o << confidence_number(value);
    return o.str();
}

std::string confidence_label(const Confidence& c) {
    return "confidence " + format_number(c.confidence) + "  (vis " + format_number(c.visibility) +
           " x ans " + format_number(c.answer) + " x res " + format_number(c.resolution) + ")";
}

std::string confidence_label(const RunReport& report) {
    return confidence_label(confidence_product(report));
}

const std::string& FindingRow::column(std::string_view name) const {
    if (name == "status") return status;
    if (name == "stage") return stage;
    if (name == "cls") return cls;
    if (name == "file") return file;
    if (name == "line") return line;
    if (name == "function") return function;
    if (name == "message") return message;
    return id;
}

std::vector<FindingRow> finding_rows(const RunReport& report) {
    std::vector<FindingRow> rows;
    for (const auto& s : report.stages) {
        int idx = -1;  // numbering counts every finding, as prism ask does
        for (const auto& f : s.findings) {
            ++idx;
            if ((f.status == laws::NOTRUN || f.status == laws::CLEAN) && is_noise_stage(s.name))
                continue;
            FindingRow r;
            r.status = f.status;
            r.stage = f.stage.empty() ? s.name : f.stage;
            r.cls = f.cls;
            r.file = f.file;
            r.line = f.line ? std::to_string(*f.line) : std::string();
            r.function = f.function.value_or("");
            r.message = utf8_left(f.message, 200);
            r.id = s.name + "#" + std::to_string(idx);
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

std::vector<TaxonomyRow> taxonomy_rows(const RunReport& report) {
    std::vector<TaxonomyRow> rows;
    for (const auto& t : coverage_from_report(report)) {
        std::string verdict = t.verdict;
        if (verdict != "COVERED" && verdict != "PARTIAL" && verdict != "GAP") verdict = "GAP";
        verdict = refuse_llm_cover(report, t.id, verdict, t.best);
        rows.push_back({t.id, verdict, t.best});
    }
    return rows;
}

StageRow stage_row(const StageResult& s) {
    char secs[64];
    std::snprintf(secs, sizeof secs, "%.2f", confidence_number(s.elapsed));
    return {s.name, s.status, std::to_string(s.records), secs,
            s.detail.empty() ? s.install : s.detail};
}

std::vector<StageRow> stage_rows(const std::vector<StageResult>& stages) {
    std::vector<StageRow> rows;
    rows.reserve(stages.size());
    for (const auto& s : stages) rows.push_back(stage_row(s));
    return rows;
}

std::string status_background(std::string_view status) {
    if (status == "PROVED-CERTIFIED") return "#1f6f3a";
    if (status == "PROVED-UNBOUNDED") return "#1f6f3a";
    if (status == "PROVED") return "#2a8148";
    if (status == "PROVED-ASSUMING") return "#3a7a4a";
    if (status == "BOUNDED") return "#6b6b2a";
    if (status == "FAILED") return "#8b2e2e";
    if (status == "CRASH" || status == "SANFAIL") return "#aa1111";
    if (status == "CLEAN") return "#2a4a6b";
    if (status == "HYPOTHESIS" || status == "READS") return "#5a3a7a";
    if (status == "NOTRUN") return "#6b5a2a";
    if (status == "ERROR") return "#5a5a5a";
    if (status == "NEEDS-HARNESS") return "#5a4a2a";
    if (status == "TIMEOUT" || status == "UNKNOWN") return "#4a4a4a";
    return "";
}

std::string taxonomy_background(std::string_view verdict) {
    if (verdict == "COVERED") return status_background("PROVED");
    if (verdict == "PARTIAL") return status_background("BOUNDED");
    if (verdict == "GAP") return status_background("NOTRUN");
    if (verdict == "CLEAN") return status_background("CLEAN");
    return "";
}

bool is_proof_green(std::string_view hex) {
    return !hex.empty() && (hex == status_background("PROVED-UNBOUNDED") ||
                            hex == status_background("PROVED") ||
                            hex == status_background("PROVED-ASSUMING"));
}

std::vector<StageResult> live_stages(const std::vector<StageResult>& journal, double run_started,
                                     bool resume) {
    if (resume) return journal;
    std::vector<StageResult> out;
    for (const auto& s : journal)
        if (s.started >= run_started) out.push_back(s);
    return out;
}

std::string progress_line(const StageResult& s) {
    return s.name + " " + s.status + " (" + std::to_string(s.records) + ")";
}

std::optional<std::string> ProgressTracker::update(const std::vector<StageResult>& rows) {
    if (rows.empty()) return std::nullopt;
    const auto& last = rows.back();
    std::string key = last.name + ":" + last.status + ":" + std::to_string(last.records);
    if (key == last_) return std::nullopt;
    last_ = std::move(key);
    return progress_line(last);
}

std::string done_summary(const Confidence& c, const std::vector<StageResult>& stages) {
    std::string notrun;
    for (const auto& s : stages) {
        if (s.status != laws::NOTRUN) continue;
        if (!notrun.empty()) notrun += ',';
        notrun += s.name;
    }
    return "done. " + confidence_label(c) + ". NOTRUN=" + (notrun.empty() ? "none" : notrun);
}

std::optional<std::string> run_failure(int exit_code, bool crashed) {
    if (crashed) return "prism crashed (exit " + std::to_string(exit_code) + "): not a clean run";
    if (exit_code == 0 || exit_code == 1) return std::nullopt;
    if (exit_code == 2)
        return "prism exited 2 (a stage crashed or the arguments were rejected): not a clean run";
    return "prism exited " + std::to_string(exit_code) + ": not a clean run";
}

Finding missing_display_finding(const std::string& reason) {
    Finding f;
    f.stage = "gui";
    f.status = std::string(laws::NOTRUN);
    f.message = "NOTRUN: no display; Qt window is NOTRUN";
    if (!reason.empty()) f.message += " (" + reason + ")";
    f.strength = std::string(laws::STRENGTH_READS);
    f.extra["install"] = "QT_QPA_PLATFORM=offscreen or a real display";
    return f;
}

std::string notrun_display_text(const std::string& reason) {
    const auto f = missing_display_finding(reason);
    std::string t = "NOTRUN gui: no display — not a clean window\n";
    t += "  " + f.message + "\n";
    t += "  install: " + f.extra.at("install") + "\n";
    return t;
}

bool display_available(const char* display, const char* wayland, const char* platform) {
#ifdef _WIN32
    (void)display, (void)wayland, (void)platform;
    return true;
#else
    if (platform && std::string_view(platform) == "offscreen") return true;
    return (display && *display) || (wayland && *wayland);
#endif
}

bool flag_takes_value(std::string_view a) {
    static constexpr std::string_view kValue[] = {
        "--out", "--stage", "--skip", "--unwind", "--fuzz-budget", "--fuzz-iters",
        "--repair-rounds", "--jobs", "-j", "--tool", "--pbsd", "--timeout", "--solver-cache",
        "--requirements", "--contracts-approved", "--fail-on", "--pir-vcs", "--solve-smt2"};
    return std::find(std::begin(kValue), std::end(kValue), a) != std::end(kValue);
}

std::vector<std::string> gui_run_args(const RunOptions& o) {
    std::vector<std::string> args{o.path, "--out", o.out};
    if (!o.llm) args.emplace_back("--no-llm");
    if (o.resume) args.emplace_back("--resume");
    if (o.allow_exec) args.emplace_back("--allow-exec");
    auto skip = skip_from_checks(o.skip_fuzz, o.skip_repair, o.skip_optional);
    for (const auto& s : o.extra_skip)
        if (std::find(skip.begin(), skip.end(), s) == skip.end()) skip.push_back(s);
    if (!skip.empty()) {
        std::string joined;
        for (const auto& s : skip) joined += (joined.empty() ? "" : ",") + s;
        args.emplace_back("--skip");
        args.push_back(joined);
    }
    auto given = [&](std::string_view flag) {
        for (const auto& e : o.extra)
            if (e == flag || (e.size() > flag.size() && e.starts_with(flag) && e[flag.size()] == '='))
                return true;
        return false;
    };
    if (!given("--fuzz-budget")) args.insert(args.end(), {"--fuzz-budget", std::string(GUI_FUZZ_BUDGET)});
    if (!given("--fuzz-iters")) args.insert(args.end(), {"--fuzz-iters", std::string(GUI_FUZZ_ITERS)});
    if (!given("--repair-rounds"))
        args.insert(args.end(), {"--repair-rounds", std::string(GUI_REPAIR_ROUNDS)});
    args.insert(args.end(), o.extra.begin(), o.extra.end());
    return args;
}

Launch parse_launch(const std::vector<std::string>& args) {
    Launch c;
    auto add_skip = [&](const std::string& csv) {
        std::stringstream ss(csv);
        std::string t;
        while (std::getline(ss, t, ',')) {
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (t == "fuzz") c.skip_fuzz = true;
            else if (t == "repair") c.skip_repair = true;
            else if (t == "optional") c.skip_optional = true;
            else if (!t.empty()) c.extra_skip.push_back(t);
        }
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        // --flag=value is the same as --flag value.
        std::optional<std::string> inline_value;
        if (a.starts_with("--")) {
            if (auto eq = a.find('='); eq != std::string::npos && flag_takes_value(a.substr(0, eq))) {
                inline_value = a.substr(eq + 1);
                a = a.substr(0, eq);
            }
        }
        auto value = [&]() -> std::optional<std::string> {
            if (inline_value) return inline_value;
            if (i + 1 < args.size()) return args[++i];
            return std::nullopt;
        };
        if (a == "--gui") {
            c.from_cli = true;
            continue;
        }
        c.from_cli = true;
        if (a == "--no-llm") c.no_llm = true;
        else if (a == "--resume") c.resume = true;
        else if (a == "--allow-exec") c.allow_exec = true;
        else if (a == "--out") {
            if (auto v = value()) c.out = *v;
            else c.ignored.push_back(a + " (no value)");
        } else if (a == "--skip") {
            if (auto v = value()) add_skip(*v);
            else c.ignored.push_back(a + " (no value)");
        } else if (non_scan_flag(a)) {
            c.ignored.push_back(a);
        } else if (non_scan_value_flag(a)) {
            auto v = value();
            c.ignored.push_back(v ? a + " " + *v : a);
        } else if (flag_takes_value(a)) {
            if (auto v = value()) {
                c.extra.push_back(a);
                c.extra.push_back(*v);
            } else {
                c.ignored.push_back(a + " (no value)");
            }
        } else if (a.starts_with("-") && a.size() > 1) {
            // A boolean scan flag (--strict-aliasing, --pir-drafts,
            // --fp-checks, --certified, ...) or one the CLI will judge.
            c.extra.push_back(args[i]);
        } else if (c.path.empty()) {
            c.path = a;
        } else {
            c.ignored.push_back(a + " (second path)");
        }
    }
    return c;
}

std::filesystem::path gui_binary_name() {
#ifdef _WIN32
    return "prism_gui.exe";
#else
    return "prism_gui";
#endif
}

std::vector<std::string> gui_forward_args(const std::filesystem::path& gui,
                                          const std::vector<std::string>& args) {
    std::vector<std::string> out{gui.string()};
    for (const auto& a : args)
        if (a != "--gui") out.push_back(a);
    return out;
}

std::filesystem::path find_prism_gui(const std::vector<std::filesystem::path>& self_dirs,
                                     std::string_view path_env) {
    const auto name = gui_binary_name();
    for (const auto& dir : self_dirs) {
        if (dir.empty()) continue;
        auto cand = dir / name;
        if (file_is_exe(cand)) return cand;
    }
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::size_t start = 0;
    while (start <= path_env.size()) {
        auto end = path_env.find(sep, start);
        if (end == std::string_view::npos) end = path_env.size();
        auto dir = path_env.substr(start, end - start);
        if (!dir.empty()) {
            auto cand = std::filesystem::path(std::string(dir)) / name;
            if (file_is_exe(cand)) return cand;
        }
        start = end + 1;
    }
    return {};
}

std::string notrun_missing_gui_text() {
    return "NOTRUN gui: prism_gui not found — not a clean window\n"
           "  install: build prism_gui with WSL clang++ Qt6 Widgets (never MinGW)\n";
}

std::string error_spawn_gui_text(const std::string& detail) {
    std::string t = "ERROR gui: failed to spawn prism_gui — not a clean window\n";
    if (!detail.empty()) t += "  " + detail + "\n";
    return t;
}

}  // namespace prism::gui
