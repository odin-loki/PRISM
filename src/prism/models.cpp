#include "prism/models.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <sstream>

namespace prism {
namespace {

nlohmann::json finding_to_json(const Finding& f) {
    nlohmann::json extra = nlohmann::json::object();
    for (auto& [k, v] : f.extra) extra[k] = v;
    return {
        {"stage", f.stage},
        {"status", f.status},
        {"file", f.file},
        {"function", f.function ? nlohmann::json(*f.function) : nlohmann::json(nullptr)},
        {"line", f.line ? nlohmann::json(*f.line) : nlohmann::json(nullptr)},
        {"cls", f.cls},
        {"message", f.message},
        {"strength", f.strength},
        {"evidence", f.evidence},
        {"counterexample", f.counterexample},
        {"extra", extra},
    };
}

Finding finding_from_json(const nlohmann::json& j) {
    Finding f;
    f.stage = j.value("stage", "");
    f.status = j.value("status", "");
    f.file = j.value("file", "");
    if (j.contains("function") && !j["function"].is_null())
        f.function = j["function"].get<std::string>();
    if (j.contains("line") && !j["line"].is_null())
        f.line = j["line"].get<int>();
    f.cls = j.value("cls", "");
    f.message = j.value("message", "");
    f.strength = j.value("strength", "");
    f.evidence = j.value("evidence", "");
    f.counterexample = j.value("counterexample", "");
    if (j.contains("extra") && j["extra"].is_object()) {
        for (auto it = j["extra"].begin(); it != j["extra"].end(); ++it) {
            if (it.value().is_string()) f.extra[it.key()] = it.value().get<std::string>();
            else f.extra[it.key()] = it.value().dump();
        }
    }
    return f;
}

nlohmann::json stage_to_json(const StageResult& s) {
    nlohmann::json findings = nlohmann::json::array();
    for (auto& f : s.findings) findings.push_back(finding_to_json(f));
    return {
        {"name", s.name},
        {"status", s.status},
        {"detail", s.detail},
        {"started", s.started},
        {"elapsed", s.elapsed},
        {"records", s.records},
        {"install", s.install},
        {"findings", findings},
    };
}

StageResult stage_from_json(const nlohmann::json& j) {
    StageResult s;
    s.name = j.value("name", "");
    s.status = j.value("status", "");
    s.detail = j.value("detail", "");
    s.started = j.value("started", 0.0);
    s.elapsed = j.value("elapsed", 0.0);
    s.records = j.value("records", 0);
    s.install = j.value("install", "");
    if (j.contains("findings") && j["findings"].is_array()) {
        for (auto& fj : j["findings"]) s.findings.push_back(finding_from_json(fj));
        if (!s.records) s.records = static_cast<int>(s.findings.size());
    }
    return s;
}

nlohmann::json fn_to_json(const FunctionInfo& f) {
    nlohmann::json params = nlohmann::json::array();
    for (auto& [t, n] : f.params) params.push_back(nlohmann::json::array({t, n}));
    return {
        {"file", f.file},
        {"name", f.name},
        {"kind", f.kind},
        {"line", f.line},
        {"signature", f.signature},
        {"params", params},
        {"return_type", f.return_type},
        {"static", f.is_static},
        {"body", f.body},
        {"span", nlohmann::json::array({f.span.first, f.span.second})},
    };
}

FunctionInfo fn_from_json(const nlohmann::json& j) {
    FunctionInfo f;
    f.file = j.value("file", "");
    f.name = j.value("name", "");
    f.kind = j.value("kind", "OTHER");
    f.line = j.value("line", 0);
    f.signature = j.value("signature", "");
    f.return_type = j.value("return_type", "int");
    f.is_static = j.value("static", false);
    f.body = j.value("body", "");
    if (j.contains("span") && j["span"].is_array() && j["span"].size() == 2)
        f.span = {j["span"][0].get<int>(), j["span"][1].get<int>()};
    if (j.contains("params") && j["params"].is_array()) {
        for (auto& p : j["params"]) {
            if (p.is_array() && p.size() >= 2)
                f.params.emplace_back(p[0].get<std::string>(), p[1].get<std::string>());
        }
    }
    return f;
}

}  // namespace

std::string RunReport::dumps() const {
    nlohmann::json stages = nlohmann::json::array();
    for (auto& s : this->stages) stages.push_back(stage_to_json(s));
    nlohmann::json functions = nlohmann::json::array();
    for (auto& f : this->functions) functions.push_back(fn_to_json(f));
    nlohmann::json j = {
        {"root", root},
        {"started", started},
        {"visibility", visibility},
        {"answer", answer},
        {"resolution", resolution},
        {"confidence", confidence},
        {"notes", notes},
        {"functions", functions},
        {"stages", stages},
    };
    return j.dump(2);
}

void RunReport::save(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << dumps();
}

std::optional<RunReport> RunReport::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(in);
        RunReport r;
        r.root = j.value("root", "");
        r.started = j.value("started", 0.0);
        r.visibility = j.value("visibility", 0.0);
        r.answer = j.value("answer", 0.0);
        r.resolution = j.value("resolution", 0.0);
        r.confidence = j.value("confidence", 0.0);
        if (j.contains("notes") && j["notes"].is_array())
            r.notes = j["notes"].get<std::vector<std::string>>();
        if (j.contains("functions"))
            for (auto& fj : j["functions"]) r.functions.push_back(fn_from_json(fj));
        if (j.contains("stages"))
            for (auto& sj : j["stages"]) r.stages.push_back(stage_from_json(sj));
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

Finding finding_from_json_object(const std::string& json_object) {
    return finding_from_json(nlohmann::json::parse(json_object));
}

StageResult stage_from_json_object(const std::string& json_object) {
    return stage_from_json(nlohmann::json::parse(json_object));
}

FunctionInfo function_from_json_object(const std::string& json_object) {
    return fn_from_json(nlohmann::json::parse(json_object));
}

}  // namespace prism
