#include <cstdlib>
// pir-vs-bmc: differential oracle between the bmc encoder and the pir stage
// (roadmap 2.8). Runs PRISM once over a tree with --stage
// inventory,classify,bmc,pir (or reads an existing report.json) and prints the
// per-function agreement matrix plus every hard conflict: one engine says
// PROVED* and the other FAILED. A wrong PROVED in either engine is a
// soundness bug (docs/PIR.md). --gate exits 1 when hard conflicts are nonzero
// or bmc PROVED* meets pir absent / NEEDS-HARNESS. It does not retire bmc.

#include "qa.hpp"

#include "../../prism/proc.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
const std::vector<std::string> ROWS = {"PROVED*", "BOUNDED", "FAILED", "NEEDS-HARNESS", "ERROR", "UNKNOWN", "absent"};

std::string text_of(const nlohmann::json& f, const char* key) {
    auto it = f.find(key);
    if (it == f.end() || it->is_null()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

using PerFunction = std::map<std::pair<std::string, std::string>, nlohmann::json>;

PerFunction per_function(const nlohmann::json& report, const std::string& stage) {
    PerFunction out;
    auto st = report.find("stages");
    if (st == report.end() || !st->is_array()) return out;
    for (const auto& s : *st) {
        if (text_of(s, "name") != stage) continue;
        auto fs_ = s.find("findings");
        if (fs_ == s.end() || !fs_->is_array()) continue;
        for (const auto& f : *fs_) {
            const auto fn = text_of(f, "function");
            if (fn.empty()) continue;
            // the first finding per function is the verdict (both stages emit one)
            out.emplace(std::make_pair(text_of(f, "file"), fn), f);
        }
    }
    return out;
}

// pir's unit-level findings (no function) by file, and every file pir reported on
struct PirUnits {
    std::map<std::string, std::string> unit_msg;  // first unit-level ERROR/NOTRUN message
    std::set<std::string> files;
};
PirUnits pir_units(const nlohmann::json& report) {
    PirUnits u;
    auto st = report.find("stages");
    if (st == report.end() || !st->is_array()) return u;
    for (const auto& s : *st) {
        if (text_of(s, "name") != "pir") continue;
        auto fs_ = s.find("findings");
        if (fs_ == s.end() || !fs_->is_array()) continue;
        for (const auto& f : *fs_) {
            const auto file = text_of(f, "file");
            u.files.insert(file);
            const auto st_ = text_of(f, "status");
            if (text_of(f, "function").empty() && (st_ == "ERROR" || st_ == "NOTRUN"))
                u.unit_msg.emplace(file, text_of(f, "message"));
        }
    }
    return u;
}

// Why a function pir does not report is absent (Law 7: written down, not
// excluded): the gate still counts every one of these.
std::string absent_reason(const PirUnits& u, const std::string& file) {
    auto m = u.unit_msg.find(file);
    if (m != u.unit_msg.end() && m->second.starts_with("clang front end failed"))
        return "absent: clang rejects the unit";
    if (!u.files.count(file)) return "absent: no pir result for the unit (timeout, memory or crash)";
    return "absent";
}
}  // namespace

std::string pvb_bucket(const std::string* status) {
    if (!status) return "absent";
    const auto& s = *status;
    // Every proof class, PROVED-CERTIFIED included: it is a PROVED with a
    // checked certificate, and a certified proof that the other engine
    // refutes is exactly the hard conflict this tool exists to show.
    if (s == "PROVED" || s == "PROVED-UNBOUNDED" || s == "PROVED-ASSUMING" || s == "PROVED-CERTIFIED")
        return "PROVED*";
    if (s == "BOUNDED" || s == "FAILED" || s == "NEEDS-HARNESS" || s == "ERROR") return s;
    return "UNKNOWN";  // UNKNOWN / TIMEOUT / NOFUNC / NOTRUN
}

std::string pvb_gap_reason(const std::string* pir_status, const std::string& pir_msg) {
    if (!pir_status) return "absent";
    std::string m = pir_msg;
    auto cut = m.find_first_of(":;(\n");
    if (cut != std::string::npos) m = m.substr(0, cut);
    std::string o;
    for (char c : m) {
        if (c >= '0' && c <= '9') {
            if (o.empty() || o.back() != 'N') o += 'N';
        } else o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > 80) o.resize(80);
    return *pir_status + ": " + (o.empty() ? "(no message)" : o);
}

PvbGate pir_vs_bmc_gate(const PvbResult& r) {
    PvbGate g;
    g.hard_conflicts_ok = r.conflicts.empty();
    g.superset_ok = r.superset_gaps == 0;
    g.pass = g.hard_conflicts_ok && g.superset_ok;
    return g;
}

PvbResult pir_vs_bmc(const nlohmann::json& report) {
    PvbResult r;
    const auto bmc = per_function(report, "bmc");
    const auto pir = per_function(report, "pir");
    const auto units = pir_units(report);
    std::set<std::pair<std::string, std::string>> keys;
    for (auto& [k, _] : bmc) keys.insert(k);
    for (auto& [k, _] : pir) keys.insert(k);
    r.functions = keys.size();
    r.bmc = bmc.size();
    r.pir = pir.size();
    for (const auto& k : keys) {
        auto b = bmc.find(k);
        auto p = pir.find(k);
        std::optional<std::string> sb, sp;
        if (b != bmc.end()) sb = text_of(b->second, "status");
        if (p != pir.end()) sp = text_of(p->second, "status");
        const auto rb = pvb_bucket(sb ? &*sb : nullptr);
        const auto rp = pvb_bucket(sp ? &*sp : nullptr);
        ++r.matrix[{rb, rp}];
        if (rb == "PROVED*" && (rp == "absent" || rp == "NEEDS-HARNESS")) {
            const auto msg = p != pir.end() ? text_of(p->second, "message") : std::string();
            const auto why = sp ? pvb_gap_reason(&*sp, msg) : absent_reason(units, k.first);
            ++r.gap_reasons[why];
            r.gaps.push_back({{"file", k.first},
                              {"function", k.second},
                              {"pir", sp ? *sp : std::string("absent")},
                              {"pir_msg", msg},
                              {"reason", why}});
        }
        if ((rb == "PROVED*" && rp == "FAILED") || (rb == "FAILED" && rp == "PROVED*")) {
            r.conflicts.push_back({{"file", k.first},
                                   {"function", k.second},
                                   {"bmc", *sb},
                                   {"bmc_msg", text_of(b->second, "message")},
                                   {"bmc_cex", text_of(b->second, "counterexample")},
                                   {"pir", *sp},
                                   {"pir_msg", text_of(p->second, "message")},
                                   {"pir_cex", text_of(p->second, "counterexample")}});
        }
    }
    // Superset gaps are matrix[PROVED*, absent] + matrix[PROVED*, NEEDS-HARNESS],
    // taken from the matrix so the counter cannot drift. BOUNDED is not a
    // PROVED* (Law 2). ERROR and UNKNOWN are not those cells.
    for (auto& [rc, n] : r.matrix) {
        if (rc.first == rc.second && rc.first != "absent") r.agree_both += n;
        if (rc.first != "absent" && rc.second != "absent") r.both_report += n;
        if (rc.first == "PROVED*" && (rc.second == "absent" || rc.second == "NEEDS-HARNESS")) r.superset_gaps += n;
    }
    return r;
}

std::string pir_vs_bmc_text(const PvbResult& r) {
    std::size_t width = 0;
    for (auto& row : ROWS) width = std::max(width, row.size());
    width += 2;
    const int w = static_cast<int>(width);
    std::ostringstream o;
    o << "functions: " << r.functions << " (bmc " << r.bmc << ", pir " << r.pir << ")\n";
    o << "rows = bmc, columns = pir\n";
    o << std::string(width, ' ');
    for (auto& c : ROWS) o << std::setw(w) << std::right << c;
    o << "\n";
    for (auto& row : ROWS) {
        o << std::setw(w) << std::left << row;
        for (auto& c : ROWS) {
            auto it = r.matrix.find({row, c});
            o << std::setw(w) << std::right << (it == r.matrix.end() ? 0 : it->second);
        }
        o << "\n";
    }
    o << "same bucket where both report: " << r.agree_both << "/" << r.both_report << "\n";
    o << "superset gaps (bmc PROVED*, pir absent or NEEDS-HARNESS): " << r.superset_gaps << "\n";
    const auto gate = pir_vs_bmc_gate(r);
    o << "retirement gate (hard conflicts): " << (gate.hard_conflicts_ok ? "PASS" : "FAIL") << " ("
      << r.conflicts.size() << ")\n";
    o << "retirement gate (pir superset): " << (gate.superset_ok ? "PASS" : "FAIL") << " (" << r.superset_gaps
      << ")\n";
    o << "retirement gate (overall): " << (gate.pass ? "PASS" : "FAIL") << "\n";
    if (!r.gap_reasons.empty()) {
        std::vector<std::pair<int, std::string>> by;
        for (auto& [why, n] : r.gap_reasons) by.emplace_back(-n, why);
        std::sort(by.begin(), by.end());
        o << "superset gaps by reason:\n";
        for (auto& [n, why] : by) o << "  " << -n << "  " << why << "\n";
    }
    o << "hard conflicts (PROVED* vs FAILED): " << r.conflicts.size() << "\n";
    for (const auto& c : r.conflicts)
        o << "  " << c["file"].get<std::string>() << "::" << c["function"].get<std::string>()
          << ": bmc=" << c["bmc"].get<std::string>() << " [" << c["bmc_msg"].get<std::string>() << "] "
          << c["bmc_cex"].get<std::string>() << " | pir=" << c["pir"].get<std::string>() << " ["
          << c["pir_msg"].get<std::string>() << "] " << c["pir_cex"].get<std::string>() << "\n";
    return o.str();
}

nlohmann::json pir_vs_bmc_json(const PvbResult& r) {
    nlohmann::json m = nlohmann::json::object();
    for (auto& [rc, n] : r.matrix) m[rc.first + "|" + rc.second] = n;
    const auto gate = pir_vs_bmc_gate(r);
    return {{"matrix", m},
            {"conflicts", r.conflicts},
            {"agree_both", r.agree_both},
            {"both_report", r.both_report},
            {"superset_gaps", r.superset_gaps},
            {"gaps", r.gaps},
            {"gap_reasons", r.gap_reasons},
            {"gate", {{"hard_conflicts", gate.hard_conflicts_ok ? "PASS" : "FAIL"},
                      {"superset", gate.superset_ok ? "PASS" : "FAIL"},
                      {"overall", gate.pass ? "PASS" : "FAIL"}}}};
}

int pir_vs_bmc_main(const Args& args) {
    const auto repo = repo_root();
    std::string tree = (repo / "testdata").string(), bin, report_path, json_out, keep_report;
    int jobs = 2;
    double timeout = 0;
    // Per-function pir solver budget for the gated run (PRISM_FUNCTION_BUDGET):
    // one huge function (SV-COMP s3_srvr's state machine) must not use up
    // the run so that every other function of its unit goes unreported.
    // A spent budget is a TIMEOUT verdict with the budget named (Law 7),
    // never a proof; the environment's own value wins.
    std::string function_budget = "60";
    bool gate = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--bin") bin = next();
        else if (a == "--report") report_path = next();
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--json") json_out = next();
        else if (a == "--timeout") timeout = std::atof(next().c_str());
        else if (a == "--gate") gate = true;
        else if (a == "--function-budget") function_budget = next();
        else if (a == "--keep-report") keep_report = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa pir-vs-bmc [TREE] [--bin PRISM] [--report report.json] [--jobs N]\n"
                         "                    [--json OUT.json] [--timeout S] [--gate]\n"
                         "                    [--keep-report report.json] [--function-budget S]\n"
                         "bmc encoder vs pir stage: per-function agreement matrix and every hard\n"
                         "conflict (PROVED* vs FAILED). --gate exits 1 when hard conflicts are\n"
                         "nonzero or a bmc PROVED* function is pir-absent or NEEDS-HARNESS.\n"
                         "Agreement is printed and is not an exit condition. Default TREE: testdata.\n"
                         "--function-budget: pir seconds per function (PRISM_FUNCTION_BUDGET; default 60,\n"
                         "0: none; the environment's value wins). A spent budget is TIMEOUT, not PROVED.\n";
            return 0;
        } else if (!a.starts_with("-")) tree = a;
        else {
            std::cerr << "pir-vs-bmc: unknown option " << a << "\n";
            return 2;
        }
    }
    std::optional<nlohmann::json> report;
    if (!report_path.empty()) {
        report = load_json(report_path);
        if (!report) {
            std::cerr << "pir-vs-bmc: cannot read " << report_path << "\n";
            return 2;
        }
    } else {
        auto exe = find_prism(bin, repo);
        if (!exe) {
            std::cout << "pir-vs-bmc: NOTRUN: no PRISM binary (build it, or pass --bin / PRISM_BIN)\n";
            return 2;
        }
        TempDir td("prism_pir_vs_bmc_");
        const auto out = td.path / "out";
        if (const char* e = std::getenv("PRISM_FUNCTION_BUDGET"); e && *e) function_budget = e;
        else {
#ifdef _WIN32
            ::_putenv_s("PRISM_FUNCTION_BUDGET", function_budget.c_str());
#else
            ::setenv("PRISM_FUNCTION_BUDGET", function_budget.c_str(), 1);
#endif
        }
        std::cout << "pir function budget: " << function_budget << " s (PRISM_FUNCTION_BUDGET)\n";
        detail::SessionOptions so;
        so.cwd = repo;
        so.timeout_s = timeout;
        auto r = detail::run_session({exe->string(), fs::absolute(tree).string(), "--no-llm", "--stage",
                                      "inventory,classify,bmc,pir", "--out", out.string(), "--jobs",
                                      std::to_string(jobs)},
                                     so);
        report = load_json(out / "report.json");
        if (report && !keep_report.empty()) {
            std::error_code ec;
            fs::copy_file(out / "report.json", keep_report, fs::copy_options::overwrite_existing, ec);
        }
        if (!report) {
            std::cerr << "pir-vs-bmc: PRISM wrote no report.json (rc " << r.rc
                      << (r.timed_out ? ", timed out" : "") << ")\n"
                      << r.err.substr(r.err.size() > 2000 ? r.err.size() - 2000 : 0) << "\n";
            return 2;
        }
    }
    const auto res = pir_vs_bmc(*report);
    std::cout << pir_vs_bmc_text(res);
    if (!json_out.empty()) std::ofstream(json_out, std::ios::binary) << pir_vs_bmc_json(res).dump(2);
    if (gate) {
        const auto g = pir_vs_bmc_gate(res);
        if (!g.pass) {
            if (!g.hard_conflicts_ok)
                std::cerr << "PIR-vs-BMC GATE FAILED: " << res.conflicts.size() << " hard conflict(s)\n";
            if (!g.superset_ok)
                std::cerr << "PIR-vs-BMC GATE FAILED: " << res.superset_gaps
                          << " bmc PROVED* function(s) with pir absent or NEEDS-HARNESS\n";
            return 1;
        }
    }
    return 0;
}

}  // namespace prism::qa
