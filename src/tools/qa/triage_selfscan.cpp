// triage-selfscan OUT/: sorts the defects of a PRISM self-scan report.sarif
// into real / false_alarm / out_of_scope (docs/VERIFICATION_PLAN.md step 1).
//
//   third_party/                        out of scope (not our code)
//   testdata/, testdata_tp/, tests/     false alarm (planted bugs, fixtures)
//   .github/                            false alarm (workflow LANG-LINT)
//   src/prism/, src/gui/                false alarm after manual review,
//                                       except a sanitize CRASH (real)
//   anything else                       real

#include "qa.hpp"

#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
constexpr const char* CORPUS[] = {"testdata/", "testdata_tp/", "tests/"};
constexpr const char* SRC[] = {"src/prism/", "src/gui/"};

bool is_defect(const std::string& s) { return s == "FAILED" || s == "CRASH" || s == "SANFAIL"; }

template <std::size_t N>
bool starts_any(const std::string& p, const char* const (&prefixes)[N]) {
    for (auto* x : prefixes)
        if (p.starts_with(x)) return true;
    return false;
}

std::string str_at(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string norm(std::string uri) {
    for (auto& c : uri)
        if (c == '\\') c = '/';
    return uri.starts_with("./") ? uri.substr(2) : uri;
}

std::string path_of(const nlohmann::json& r) {
    auto it = r.find("locations");
    if (it == r.end() || !it->is_array() || it->empty()) return {};
    const auto& loc = (*it)[0];
    if (!loc.is_object()) return {};
    auto pl = loc.find("physicalLocation");
    if (pl == loc.end() || !pl->is_object()) return {};
    auto al = pl->find("artifactLocation");
    if (al == pl->end()) return {};
    return norm(str_at(*al, "uri"));
}

const nlohmann::json& props_of(const nlohmann::json& r) {
    static const nlohmann::json empty = nlohmann::json::object();
    auto it = r.find("properties");
    return it != r.end() && it->is_object() ? *it : empty;
}
}  // namespace

std::string triage_classify(const std::string& path, const std::string& status, const std::string& stage) {
    if (path.starts_with("third_party/") || path.find("/third_party/") != std::string::npos) return "out_of_scope";
    if (starts_any(path, CORPUS)) return "false_alarm";
    if (path.starts_with(".github/")) return "false_alarm";
    // only a defect reaches this function, and ERROR is not one: a sanitize
    // CRASH in PRISM's own sources is the one real kind there
    if (starts_any(path, SRC)) return status == "CRASH" && stage == "sanitize" ? "real" : "false_alarm";
    return "real";
}

TriageSummary triage_selfscan(const nlohmann::json& sarif) {
    TriageSummary s;
    const auto& results = sarif.at("runs").at(0).at("results");
    for (const auto& r : results) {
        const auto& props = props_of(r);
        const auto status = str_at(props, "status");
        const auto path = path_of(r);
        if (status == "FAILED") {
            ++s.failed;
            if (path.starts_with("third_party/")) ++s.third_party;
            if (starts_any(path, CORPUS)) ++s.corpus;
            if (starts_any(path, SRC)) ++s.src;
            if (path.starts_with(".github/")) ++s.github;
        }
        if (!is_defect(status)) continue;
        const auto k = triage_classify(path, status, str_at(props, "stage"));
        if (k == "real") ++s.real;
        else if (k == "false_alarm") ++s.false_alarm;
        else ++s.out_of_scope;
    }
    return s;
}

std::string triage_text(const TriageSummary& s) {
    std::ostringstream o;
    o << "total FAILED: " << s.failed << "\n"
      << "third_party (out of scope): " << s.third_party << "\n"
      << "testdata/tests (false alarm corpus): " << s.corpus << "\n"
      << "src/prism|gui (manual review): " << s.src << "\n"
      << ".github (false alarm LANG-LINT): " << s.github << "\n"
      << "summary: " << s.real << " / " << s.false_alarm << " / " << s.out_of_scope << "\n";
    return o.str();
}

int triage_selfscan_main(const Args& args) {
    if (args.size() != 1 || args[0] == "-h" || args[0] == "--help") {
        std::cout << "prism-qa triage-selfscan OUT/\n"
                     "Buckets the defects of OUT/report.sarif into real / false_alarm / out_of_scope.\n";
        return args.size() == 1 ? 0 : 2;
    }
    const auto sarif = load_json(fs::path(args[0]) / "report.sarif");
    if (!sarif) {
        std::cerr << "triage-selfscan: cannot read " << (fs::path(args[0]) / "report.sarif").string() << "\n";
        return 2;
    }
    try {
        std::cout << triage_text(triage_selfscan(*sarif));
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "triage-selfscan: not a SARIF log (" << e.what() << ")\n";
        return 2;
    }
    return 0;
}

}  // namespace prism::qa
