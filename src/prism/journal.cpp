#include "prism/journal.hpp"
#include "prism/models_json.hpp"

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
    // The stage's JSON is exactly its row in report.json (same to_json);
    // dump_json keeps a non-UTF-8 byte in a message from aborting the run.
    auto line = dump_json(nlohmann::json(rec));
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
    p << dump_json(progress);
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
    // Direct to_json, not a RunReport round trip: functions.json keeps
    // body_line/body_col (report.json does not), which --resume needs.
    nlohmann::json fns = nlohmann::json::array();
    for (auto& fn : functions) fns.push_back(fn);
    std::ofstream f(out / FUNCTIONS_JSON, std::ios::binary);
    f << dump_json(fns);
}

namespace {

// Every functions.json field that is present has the type to_json writes;
// file and name are required.
bool well_typed_function(const nlohmann::json& fj) {
    auto str = [&](const char* k, bool required) {
        auto it = fj.find(k);
        return it == fj.end() ? !required : it->is_string();
    };
    auto integer = [&](const char* k) {
        auto it = fj.find(k);
        return it == fj.end() || it->is_number_integer();
    };
    if (!str("file", true) || !str("name", true) || !str("kind", false) || !str("signature", false) ||
        !str("return_type", false) || !str("body", false))
        return false;
    if (!integer("line") || !integer("body_line") || !integer("body_col")) return false;
    if (auto it = fj.find("static"); it != fj.end() && !it->is_boolean()) return false;
    if (auto it = fj.find("span"); it != fj.end()) {
        if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number_integer() || !(*it)[1].is_number_integer())
            return false;
    }
    if (auto it = fj.find("params"); it != fj.end()) {
        if (!it->is_array()) return false;
        for (auto& p : *it)
            if (!p.is_array() || p.size() != 2 || !p[0].is_string() || !p[1].is_string()) return false;
    }
    return true;
}

}  // namespace

std::vector<FunctionInfo> journal_read_functions(const std::filesystem::path& out,
                                                 std::size_t* malformed) {
    if (malformed) *malformed = 0;
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
    std::size_t bad = 0;
    for (auto& fj : j) {
        if (!fj.is_object()) {
            ++bad;
            continue;
        }
        // The report loader is tolerant (a mistyped field takes its
        // default); a resume is not: a field of the wrong JSON type would
        // silently change what is analysed, so the entry is malformed.
        if (!well_typed_function(fj)) {
            ++bad;
            continue;
        }
        try {
            fns.push_back(fj.get<FunctionInfo>());
        } catch (...) {
            ++bad;
        }
    }
    if (malformed) *malformed = bad;
    // All or nothing: a partial list would let --resume skip classify and
    // never analyse the dropped functions (Law 7). An empty list makes the
    // pipeline rerun classify.
    if (bad) return {};
    return fns;
}

}  // namespace prism
