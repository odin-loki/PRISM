#include "prism/journal.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace prism {

void journal_reset(const std::filesystem::path& out) {
    std::filesystem::create_directories(out);
    std::error_code ec;
    for (auto* name : {STAGES_JSONL, PROGRESS_JSON, FUNCTIONS_JSON}) std::filesystem::remove(out / name, ec);
}

void journal_append_stage(const std::filesystem::path& out, const StageResult& rec) {
    std::filesystem::create_directories(out);
    // The stage's JSON is exactly its row in report.json.
    RunReport one;
    one.stages.push_back(rec);
    auto line = nlohmann::json::parse(one.dumps())["stages"][0].dump();
    {
        std::ofstream f(out / STAGES_JSONL, std::ios::app | std::ios::binary);
        f << line << "\n";
    }
    nlohmann::json progress = {
        {"last", rec.name},
        {"status", rec.status},
        {"records", rec.records},
        {"elapsed", rec.elapsed},
    };
    // One line, rewritten after every stage (a reader tails it).
    std::ofstream p(out / PROGRESS_JSON, std::ios::binary);
    p << progress.dump();
}

std::vector<StageResult> journal_read_stages(const std::filesystem::path& out) {
    std::vector<StageResult> recs;
    std::ifstream f(out / STAGES_JSONL, std::ios::binary);
    if (!f) return recs;
    std::string ln;
    while (std::getline(f, ln)) {
        if (ln.empty()) continue;
        try {
            recs.push_back(stage_from_json_object(ln));
        } catch (...) {
            // A torn or corrupt line (a killed run) is skipped; the rest stands.
        }
    }
    return recs;
}

bool journal_stages_present(const std::filesystem::path& out) {
    // An empty/failed log is not a missing log. report.json must not revive
    // ok/NOTRUN rows when stages.jsonl exists.
    return std::filesystem::is_regular_file(out / STAGES_JSONL);
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
    std::ofstream f(out / FUNCTIONS_JSON, std::ios::binary);
    f << j.value("functions", nlohmann::json::array()).dump();
}

std::vector<FunctionInfo> journal_read_functions(const std::filesystem::path& out) {
    std::ifstream in(out / FUNCTIONS_JSON, std::ios::binary);
    if (!in) return {};
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (...) {
        return {};
    }
    if (!j.is_array()) return {};
    std::vector<FunctionInfo> fns;
    // One bad element is skipped on its own; it does not discard the rest.
    for (auto& fj : j) {
        if (!fj.is_object()) continue;
        try {
            fns.push_back(function_from_json_object(fj.dump()));
        } catch (...) {
            continue;
        }
    }
    return fns;
}

}  // namespace prism
