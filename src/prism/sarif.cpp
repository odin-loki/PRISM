// SARIF 2.1.0 export and CI exit policy. Port of the Python engine
// prism/sarif.py; tests/test_sarif.py compares the two on the same report.

#include "prism/pipeline.hpp"
#include "prism/laws.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

namespace prism {
namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;

namespace {

bool is_defect(const std::string& s) {
    return s == laws::FAILED || s == laws::CRASH || s == laws::SANFAIL;
}

bool is_gap(const std::string& s) {
    return s == laws::NOTRUN || s == laws::ERROR || s == laws::TIMEOUT;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::string> level_of(const Finding& f) {
    if (is_defect(f.status)) {
        std::string sev;
        if (auto it = f.extra.find("severity"); it != f.extra.end()) sev = lower(it->second);
        if (sev == "warning" || sev == "style" || f.strength == laws::STRENGTH_SOME)
            return "warning";
        return "error";
    }
    if (f.status == laws::HYPOTHESIS) return "note";
    return std::nullopt;
}

std::string uri_of(std::string p) {
    std::string o;
    for (char c : p) {
        if (c == '\\') o += '/';
        else if (c == '%') o += "%25";
        else if (c == ' ') o += "%20";
        else o += c;
    }
    return o;
}

// pathlib.Path.as_uri(): percent-encode everything but unreserved and '/'.
std::string file_uri(const fs::path& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string g = p.generic_string();
    if (g.empty() || g.front() != '/') g = "/" + g;
    std::string o = "file://";
    for (unsigned char c : g) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string rstrip_colon_space(std::string s) {
    while (!s.empty() && (s.back() == ':' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

std::string to_sarif(const RunReport& report) {
    std::map<std::string, ojson> rules;
    ojson results = ojson::array();
    for (auto& s : report.stages) {
        for (auto& f : s.findings) {
            auto level = level_of(f);
            if (!level) continue;
            std::string rid = f.cls.empty() ? f.stage + "/" + f.status : f.cls;
            if (!rules.contains(rid))
                rules[rid] = ojson{{"id", rid}, {"shortDescription", {{"text", rid}}}};
            std::string text = "[" + f.stage + " " + f.status + "] " + f.message;
            if (!f.counterexample.empty()) text += " (counterexample: " + f.counterexample + ")";
            ojson res = {
                {"ruleId", rid},
                {"level", *level},
                {"message", {{"text", text}}},
                {"properties", {{"stage", f.stage}, {"status", f.status}, {"strength", f.strength}}},
            };
            if (f.function && !f.function->empty()) res["properties"]["function"] = *f.function;
            if (f.status == laws::HYPOTHESIS) res["properties"]["hypothesis"] = true;
            if (!f.file.empty()) {
                ojson phys = {{"artifactLocation", {{"uri", uri_of(f.file)}, {"uriBaseId", "SRCROOT"}}}};
                if (f.line && *f.line > 0) phys["region"] = {{"startLine", *f.line}};
                res["locations"] = ojson::array({{{"physicalLocation", phys}}});
            }
            results.push_back(std::move(res));
        }
    }
    ojson notes = ojson::array();
    bool crashed = false;
    for (auto& s : report.stages) {
        if (s.status == "failed") crashed = true;
        if (s.status != "NOTRUN" && s.status != "failed") continue;
        notes.push_back({{"level", s.status == "failed" ? "error" : "warning"},
                         {"message",
                          {{"text", rstrip_colon_space(s.name + " " + s.status + ": " +
                                                       (s.detail.empty() ? s.install : s.detail))}}}});
    }
    ojson rule_arr = ojson::array();
    for (auto& [id, r] : rules) rule_arr.push_back(r);  // std::map: sorted by id
    ojson run = {
        {"tool",
         {{"driver",
           {{"name", "PRISM"},
            {"version", PRISM_VERSION},
            {"informationUri", "https://github.com/odin-loki/PRISM"},
            {"rules", rule_arr}}}}},
        {"invocations",
         ojson::array({{{"executionSuccessful", !crashed}, {"toolExecutionNotifications", notes}}})},
        {"results", results},
        {"properties", {{"confidence", report.confidence}}},
    };
    if (!report.root.empty()) {
        std::error_code ec;
        fs::path root(report.root);
        fs::path base = fs::is_directory(root, ec) ? root : root.parent_path();
        auto canon = fs::weakly_canonical(base, ec);
        run["originalUriBaseIds"] = {{"SRCROOT", {{"uri", file_uri(ec ? base : canon) + "/"}}}};
    }
    ojson doc = {{"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
                 {"version", "2.1.0"},
                 {"runs", ojson::array({run})}};
    return doc.dump(2);
}

void write_sarif(const RunReport& report, const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << to_sarif(report);
}

int exit_code(const RunReport& report, std::string_view fail_on) {
    for (auto& s : report.stages)
        if (s.status == "failed") return 2;
    if (fail_on == "never") return 0;
    for (auto& s : report.stages)
        for (auto& f : s.findings)
            if (is_defect(f.status)) return 1;
    if (fail_on == "gap") {
        for (auto& s : report.stages) {
            if (s.status == "NOTRUN") return 1;
            for (auto& f : s.findings)
                if (is_gap(f.status)) return 1;
        }
    }
    return 0;
}

}  // namespace prism
