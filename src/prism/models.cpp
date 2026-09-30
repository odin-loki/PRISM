#include "prism/models.hpp"
#include "prism/models_json.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace prism {
namespace {

using json = nlohmann::json;

const json* field(const json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return &*it;
}

// A string field: a number or bool is kept as its JSON text.
std::string get_str(const json& j, const char* key, const std::string& def = {}) {
    auto* v = field(j, key);
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    return v->dump(-1, ' ', false, json::error_handler_t::replace);
}

std::optional<double> as_double(const json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string()) {
        auto s = v.get<std::string>();
        double d = 0;
        auto b = s.data(), e = s.data() + s.size();
        while (b < e && *b == ' ') ++b;
        auto [p, ec] = std::from_chars(b, e, d);
        if (ec == std::errc() && p != b && std::isfinite(d)) return d;
    }
    return std::nullopt;
}

std::optional<int> as_int(const json& v) {
    auto d = as_double(v);
    if (!d || *d < std::numeric_limits<int>::min() || *d > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(*d);
}

double get_double(const json& j, const char* key) {
    auto* v = field(j, key);
    return v ? as_double(*v).value_or(0.0) : 0.0;
}

int get_int(const json& j, const char* key, int def = 0) {
    auto* v = field(j, key);
    return v ? as_int(*v).value_or(def) : def;
}

bool get_bool(const json& j, const char* key) {
    auto* v = field(j, key);
    if (!v) return false;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0;
    if (v->is_string()) return !v->get<std::string>().empty();
    return !v->empty();
}

std::string text_of(const json& v) {
    return v.is_string() ? v.get<std::string>() : v.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

std::string dump_json(const nlohmann::json& j, int indent) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

void to_json(nlohmann::json& j, const Finding& f) {
    json extra = json::object();
    for (auto& [k, v] : f.extra) extra[k] = v;
    j = {
        {"stage", f.stage},
        {"status", f.status},
        {"file", f.file},
        {"function", f.function ? json(*f.function) : json(nullptr)},
        {"line", f.line ? json(*f.line) : json(nullptr)},
        {"cls", f.cls},
        {"message", f.message},
        {"strength", f.strength},
        {"evidence", f.evidence},
        {"counterexample", f.counterexample},
        {"extra", extra},
    };
}

void from_json(const nlohmann::json& j, Finding& f) {
    f = Finding{};
    f.stage = get_str(j, "stage");
    f.status = get_str(j, "status");
    f.file = get_str(j, "file");
    if (auto* v = field(j, "function")) {
        // A list is its first element (the Python engine's Finding coercion).
        if (v->is_array()) {
            if (!v->empty() && !(*v)[0].is_null()) f.function = text_of((*v)[0]);
        } else {
            f.function = text_of(*v);
        }
    }
    if (auto* v = field(j, "line")) f.line = as_int(*v);
    f.cls = get_str(j, "cls");
    f.message = get_str(j, "message");
    f.strength = get_str(j, "strength");
    f.evidence = get_str(j, "evidence");
    f.counterexample = get_str(j, "counterexample");
    if (auto* v = field(j, "extra"); v && v->is_object())
        for (auto it = v->begin(); it != v->end(); ++it) f.extra[it.key()] = text_of(it.value());
}

void to_json(nlohmann::json& j, const StageResult& s) {
    json findings = json::array();
    for (auto& f : s.findings) findings.push_back(f);
    j = {
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

void from_json(const nlohmann::json& j, StageResult& s) {
    s = StageResult{};
    s.name = get_str(j, "name");
    s.status = get_str(j, "status");
    s.detail = get_str(j, "detail");
    s.started = get_double(j, "started");
    s.elapsed = get_double(j, "elapsed");
    s.install = get_str(j, "install");
    if (auto* v = field(j, "findings"); v && v->is_array())
        for (auto& fj : *v)
            if (fj.is_object()) s.findings.push_back(fj.get<Finding>());
    s.records = get_int(j, "records");
    if (!s.records) s.records = static_cast<int>(s.findings.size());
}

void to_json(nlohmann::json& j, const FunctionInfo& f) {
    json params = json::array();
    for (auto& [t, n] : f.params) params.push_back(json::array({t, n}));
    j = {
        {"file", f.file},
        {"name", f.name},
        {"kind", f.kind},
        {"line", f.line},
        {"signature", f.signature},
        {"params", params},
        {"return_type", f.return_type},
        {"static", f.is_static},
        {"body", f.body},
        {"span", json::array({f.span.first, f.span.second})},
        {"body_line", f.body_line},
        {"body_col", f.body_col},
    };
}

void from_json(const nlohmann::json& j, FunctionInfo& f) {
    f = FunctionInfo{};
    f.file = get_str(j, "file");
    f.name = get_str(j, "name");
    f.kind = get_str(j, "kind", "OTHER");
    f.line = get_int(j, "line");
    f.signature = get_str(j, "signature");
    f.return_type = get_str(j, "return_type", "int");
    f.is_static = get_bool(j, "static");
    f.body = get_str(j, "body");
    if (auto* v = field(j, "span"); v && v->is_array() && v->size() == 2)
        f.span = {as_int((*v)[0]).value_or(0), as_int((*v)[1]).value_or(0)};
    f.body_line = get_int(j, "body_line");
    f.body_col = get_int(j, "body_col");
    if (auto* v = field(j, "params"); v && v->is_array())
        for (auto& p : *v)
            if (p.is_array() && p.size() >= 2) f.params.emplace_back(text_of(p[0]), text_of(p[1]));
}

void to_json(nlohmann::json& j, const RunReport& r) {
    json stages = json::array();
    for (auto& s : r.stages) stages.push_back(s);
    json functions = json::array();
    for (auto& f : r.functions) {
        // report.json keeps the Python engine's function shape while both
        // engines exist; the body position lives in functions.json only.
        json fj = f;
        fj.erase("body_line");
        fj.erase("body_col");
        functions.push_back(std::move(fj));
    }
    j = {
        {"root", r.root},
        {"started", r.started},
        {"visibility", r.visibility},
        {"answer", r.answer},
        {"resolution", r.resolution},
        {"confidence", r.confidence},
        {"notes", r.notes},
        {"functions", functions},
        {"stages", stages},
    };
}

void from_json(const nlohmann::json& j, RunReport& r) {
    r = RunReport{};
    r.root = get_str(j, "root");
    r.started = get_double(j, "started");
    r.visibility = get_double(j, "visibility");
    r.answer = get_double(j, "answer");
    r.resolution = get_double(j, "resolution");
    r.confidence = get_double(j, "confidence");
    if (auto* v = field(j, "notes"); v && v->is_array())
        for (auto& n : *v) r.notes.push_back(text_of(n));
    if (auto* v = field(j, "functions"); v && v->is_array())
        for (auto& fj : *v)
            if (fj.is_object()) r.functions.push_back(fj.get<FunctionInfo>());
    if (auto* v = field(j, "stages"); v && v->is_array())
        for (auto& sj : *v)
            if (sj.is_object()) r.stages.push_back(sj.get<StageResult>());
}

std::string RunReport::dumps() const {
    return dump_json(json(*this), 2);
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
        auto j = json::parse(in);
        if (!j.is_object()) return std::nullopt;
        return j.get<RunReport>();
    } catch (...) {
        return std::nullopt;
    }
}

Finding finding_from_json_object(const std::string& json_object) {
    return json::parse(json_object).get<Finding>();
}

StageResult stage_from_json_object(const std::string& json_object) {
    return json::parse(json_object).get<StageResult>();
}

FunctionInfo function_from_json_object(const std::string& json_object) {
    return json::parse(json_object).get<FunctionInfo>();
}

}  // namespace prism
