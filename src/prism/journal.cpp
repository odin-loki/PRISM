#include "prism/journal.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace prism {

void journal_reset(const std::filesystem::path& out) {
    std::filesystem::create_directories(out);
    std::error_code ec;
    std::filesystem::remove(out / "stages.jsonl", ec);
    std::filesystem::remove(out / "progress.json", ec);
    std::filesystem::remove(out / "functions.json", ec);
}

void journal_append_stage(const std::filesystem::path& out, const StageResult& rec) {
    std::filesystem::create_directories(out);
    auto reportish = RunReport{};
    reportish.stages.push_back(rec);
    // serialize just this stage via dumps of a one-stage report is wasteful;
    // reconstruct from rec through JSON by saving a tiny object.
    nlohmann::json extra_findings = nlohmann::json::array();
    // Use the public JSON of a dummy report containing this stage.
    RunReport tmp;
    tmp.stages.push_back(rec);
    auto full = nlohmann::json::parse(tmp.dumps());
    auto line = full["stages"][0].dump();
    {
        std::ofstream f(out / "stages.jsonl", std::ios::app | std::ios::binary);
        f << line << "\n";
    }
    nlohmann::json progress = {
        {"last", rec.name},
        {"status", rec.status},
        {"records", rec.records},
        {"elapsed", rec.elapsed},
    };
    std::ofstream p(out / "progress.json", std::ios::binary);
    p << progress.dump(2);
}

std::vector<StageResult> journal_read_stages(const std::filesystem::path& out) {
    std::vector<StageResult> recs;
    std::ifstream f(out / "stages.jsonl", std::ios::binary);
    if (!f) return recs;
    std::string ln;
    while (std::getline(f, ln)) {
        if (ln.empty()) continue;
        try {
            recs.push_back(stage_from_json_object(ln));
        } catch (...) {
        }
    }
    return recs;
}

bool journal_stages_present(const std::filesystem::path& out) {
    // An empty/failed log is not a missing log. report.json must not revive
    // ok/NOTRUN rows when stages.jsonl exists.
    return std::filesystem::is_regular_file(out / "stages.jsonl");
}

std::map<std::string, StageResult> journal_completed_ok(const std::filesystem::path& out) {
    std::map<std::string, StageResult> recs;
    for (auto& s : journal_read_stages(out)) {
        if (s.name.empty()) continue;
        if (s.status == "ok" || s.status == "NOTRUN") recs[s.name] = s;
    }
    return recs;
}

void journal_write_functions(const std::filesystem::path& out,
                             const std::vector<FunctionInfo>& functions) {
    std::filesystem::create_directories(out);
    RunReport tmp;
    tmp.functions = functions;
    auto j = nlohmann::json::parse(tmp.dumps());
    std::ofstream f(out / "functions.json", std::ios::binary);
    f << j.value("functions", nlohmann::json::array()).dump();
}

std::vector<FunctionInfo> journal_read_functions(const std::filesystem::path& out) {
    std::ifstream in(out / "functions.json", std::ios::binary);
    if (!in) return {};
    try {
        auto j = nlohmann::json::parse(in);
        if (!j.is_array()) return {};
        std::vector<FunctionInfo> fns;
        for (auto& fj : j) fns.push_back(function_from_json_object(fj.dump()));
        return fns;
    } catch (...) {
        return {};
    }
}

}  // namespace prism
