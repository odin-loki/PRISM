// pir-vs-bmc: differential oracle between the bmc encoder and the pir stage
// (roadmap 2.8). Runs PRISM once over a tree with --stage
// inventory,classify,bmc,pir (or reads an existing report.json) and prints the
// per-function agreement matrix plus every hard conflict: one engine says
// PROVED* and the other FAILED. A wrong PROVED in either engine is a
// soundness bug (docs/PIR.md).

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

PvbResult pir_vs_bmc(const nlohmann::json& report) {
    PvbResult r;
    const auto bmc = per_function(report, "bmc");
    const auto pir = per_function(report, "pir");
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
    int agree = 0, both = 0;
    for (auto& [rc, n] : r.matrix) {
        if (rc.first == rc.second && rc.first != "absent") agree += n;
        if (rc.first != "absent" && rc.second != "absent") both += n;
    }
    o << "same bucket where both report: " << agree << "/" << both << "\n";
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
    return {{"matrix", m}, {"conflicts", r.conflicts}};
}

int pir_vs_bmc_main(const Args& args) {
    const auto repo = repo_root();
    std::string tree = (repo / "testdata").string(), bin, report_path, json_out;
    int jobs = 2;
    double timeout = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--bin") bin = next();
        else if (a == "--report") report_path = next();
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--json") json_out = next();
        else if (a == "--timeout") timeout = std::atof(next().c_str());
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa pir-vs-bmc [TREE] [--bin PRISM] [--report report.json] [--jobs N]\n"
                         "                    [--json OUT.json] [--timeout S]\n"
                         "bmc encoder vs pir stage: per-function agreement matrix and every hard\n"
                         "conflict (PROVED* vs FAILED). Default TREE: testdata.\n";
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
        detail::SessionOptions so;
        so.cwd = repo;
        so.timeout_s = timeout;
        auto r = detail::run_session({exe->string(), fs::absolute(tree).string(), "--no-llm", "--stage",
                                      "inventory,classify,bmc,pir", "--out", out.string(), "--jobs",
                                      std::to_string(jobs)},
                                     so);
        report = load_json(out / "report.json");
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
    return 0;
}

}  // namespace prism::qa
