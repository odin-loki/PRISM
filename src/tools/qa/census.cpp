// prism-qa asm-census / harness-classify: the two measurement helpers that were
// tracked Python scripts (tools/pir/asm_census.py, scripts/pir_harness_classify.py),
// ported to C++ so the repo keeps no Python outside the phase-5 allow-list
// (tests/cpp/test_deps.cpp). They measure; nothing here decides a verdict.

#include "qa.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {

// A counter that keeps first-seen order, so ties print as Python's Counter.most_common did.
struct Counter {
    std::vector<std::pair<std::string, long>> items;
    void add(const std::string& k) {
        for (auto& [key, n] : items)
            if (key == k) {
                ++n;
                return;
            }
        items.emplace_back(k, 1);
    }
    std::vector<std::pair<std::string, long>> most_common() const {
        auto v = items;
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return v;
    }
    long total() const {
        long t = 0;
        for (auto& [k, n] : items) t += n;
        return t;
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string squeeze(const std::string& s) {  // \s+ -> ' ', trimmed
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            sp = true;
            continue;
        }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += c;
    }
    return out;
}

// ------------------------------------------------------------------ asm-census
std::string census_norm(std::string t) {
    auto rep = [&](const std::regex& re, const std::string& to) { t = std::regex_replace(t, re, to); };
    for (std::size_t p = 0; (p = t.find("\\n", p)) != std::string::npos;) t.replace(p, 2, ";");
    for (std::size_t p = 0; (p = t.find("\\t", p)) != std::string::npos;) t.replace(p, 2, " ");
    rep(std::regex(R"(%\[[A-Za-z_0-9]+\])"), "%N");
    rep(std::regex(R"(%[0-9]+)"), "%N");
    for (std::size_t p = 0; (p = t.find("%%", p)) != std::string::npos; ++p) t.replace(p, 2, "%");
    t = squeeze(t);
    rep(std::regex(R"(\s*;\s*)"), "; ");
    auto strip = [](std::string s) {
        auto ok = [](char c) { return c != ' ' && c != ';'; };
        auto b = std::find_if(s.begin(), s.end(), ok);
        auto e = std::find_if(s.rbegin(), s.rend(), ok).base();
        return b < e ? std::string(b, e) : std::string();
    };
    return strip(t);
}

void census_templates(const std::string& text, Counter& cnt) {
    static const std::regex call(
        R"(\b(?:__asm__|__asm|asm_inline|asm)\b\s*(?:volatile|__volatile__|goto|inline|\s)*\()");
    static const std::regex macro(R"([A-Z_][A-Z0-9_]{2,})");
    static const std::regex str(R"x("((?:[^"\\]|\\.)*)")x");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), call); it != std::sregex_iterator(); ++it) {
        auto i = static_cast<std::size_t>(it->position() + it->length());
        std::string out;
        bool any = false;  // a lone "" literal still counts (Python: the piece list is non-empty)
        for (;;) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
            std::smatch sm;
            auto b = text.begin() + static_cast<std::ptrdiff_t>(i);
            if (std::regex_search(b, text.end(), sm, str, std::regex_constants::match_continuous)) {
                out += sm[1].str();
                any = true;
                i += static_cast<std::size_t>(sm.length(0));
                continue;
            }
            if (std::regex_search(b, text.end(), sm, macro, std::regex_constants::match_continuous)) {
                any = true;
                out += " <" + sm[0].str() + "> ";  // a leading macro (LOCK_PREFIX, _ASM_ADD ...) is kept by name
                i += static_cast<std::size_t>(sm.length(0));
                continue;
            }
            break;
        }
        if (any) cnt.add(census_norm(out));
    }
}

}  // namespace

int asm_census_main(const Args& args) {
    long top = 40;
    std::vector<fs::path> dirs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "prism-qa asm-census [--top N] DIR [DIR...]\n"
                         "  counts asm/__asm__/asm volatile/asm goto/asm_inline sites in .c/.h/.inc files by\n"
                         "  normalised template (docs/ASM_CENSUS.md); prints `count<TAB>template`, most frequent first\n";
            return 0;
        }
        if (args[i] == "--top" && i + 1 < args.size()) top = std::atol(args[++i].c_str());
        else dirs.emplace_back(args[i]);
    }
    if (dirs.empty()) {
        std::cerr << "prism-qa asm-census: no DIR given\n";
        return 2;
    }
    Counter cnt;
    for (const auto& d : dirs) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto ext = it->path().extension().string();
            if (ext == ".c" || ext == ".h" || ext == ".inc") census_templates(slurp(it->path()), cnt);
        }
    }
    std::cout << "# " << cnt.total() << " sites, " << cnt.items.size() << " distinct templates\n";
    long n = 0;
    for (auto& [t, c] : cnt.most_common()) {
        if (n++ >= top) break;
        std::cout << c << "\t" << t << "\n";
    }
    return 0;
}

// ------------------------------------------------------------------ harness-classify
namespace {

std::string jstr(const nlohmann::json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string reason(const nlohmann::json& j) {
    const std::string m = jstr(j, "message");
    if (has(m, "container_of")) return "container_of (negative offset from a parameter)";
    if (has(m, "UNENCODED: call @")) return "unknown call";
    if (std::regex_search(m, std::regex(R"(indirect call|function pointer|call through)", std::regex::icase)))
        return "function-pointer call / field dereference";
    if (starts(m, "pointer parameter: violation only under")) {
        std::smatch mm;
        std::string cls = jstr(j, "class"), text;
        if (std::regex_search(m, mm, std::regex(R"(\(([\w+-]+): ([A-Z0-9-]+) \(([^)]*))"))) {
            cls = mm[2].str();
            text = mm[3].str();
        }
        if (has(text, "function pointer")) return "function-pointer field: call through a null/arbitrary pointer";
        if (cls == "MEM-PTR-ARITH" || cls == "PTR-NULL-DEREF" || cls == "PTR-INVALID-DEREF" || cls == "MEM-UAF")
            return "arbitrary pointer field: null/invalid deref or arithmetic (depth bound, no pointee type)";
        if (starts(cls, "MEM-OOB")) return "arbitrary field used as an index or size: out of bounds";
        if (starts(cls, "INT-") || starts(cls, "DIV") || has(cls.c_str(), "SHIFT") || has(cls.c_str(), "OVF"))
            return "arbitrary field value: integer undefined behaviour (overflow, shift, division)";
        if (starts(cls, "UNINIT")) return "uninitialised read (the harness gives initialised fields; a local)";
        return "harness violation: " + cls;
    }
    if (has(m, "automatic pointer harness for parameter")) {
        auto p = m.rfind(": ");
        return "no harness object (" + (p == std::string::npos ? m : m.substr(p + 2)).substr(0, 60) + ")";
    }
    if (has(m, "recursive call") || has(m, "call depth")) return "recursion / inline depth";
    if (starts(m, "UNENCODED")) return "other UNENCODED: " + squeeze(m).substr(0, 60);
    if (has(m, "mutable globals")) return "global state precondition";
    return "other: " + squeeze(m).substr(0, 60);
}

nlohmann::ordered_json counts_json(const Counter& c, long limit = -1) {
    nlohmann::ordered_json o = nlohmann::ordered_json::object();
    long n = 0;
    for (auto& [k, v] : c.most_common()) {
        if (limit >= 0 && n++ >= limit) break;
        o[k] = v;
    }
    return o;
}

}  // namespace

int harness_classify_main(const Args& args) {
    fs::path out;
    long top = 12;
    bool as_json = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "prism-qa harness-classify OUT_DIR [--top N] [--json]\n"
                         "  classify a pir_batch.sh run (OUT_DIR/shards/shard-NN.jsonl) by why a function is still\n"
                         "  NEEDS-HARNESS (docs/HARNESS_GENERATOR.md \"Measurement\"); the last record of a unit and\n"
                         "  function wins; nothing here decides a verdict\n";
            return 0;
        }
        if (args[i] == "--top" && i + 1 < args.size()) top = std::atol(args[++i].c_str());
        else if (args[i] == "--json") as_json = true;
        else out = args[i];
    }
    if (out.empty()) {
        std::cerr << "prism-qa harness-classify: no OUT_DIR given\n";
        return 2;
    }
    std::vector<fs::path> shards;
    std::error_code ec;
    for (fs::directory_iterator it(out / "shards", ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (starts(n, "shard-") && it->path().extension() == ".jsonl") shards.push_back(it->path());
    }
    std::sort(shards.begin(), shards.end());
    std::vector<std::string> order;
    std::map<std::string, nlohmann::json> rec;
    for (const auto& p : shards) {
        std::ifstream in(p);
        for (std::string line; std::getline(in, line);) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object() || jstr(j, "record") != "function") continue;
            const std::string key = j.value("unit", nlohmann::json()).dump() + "|" + j.value("function", nlohmann::json()).dump() +
                                    "|" + j.value("start_line", nlohmann::json()).dump();
            if (!rec.count(key)) order.push_back(key);
            rec[key] = std::move(j);
        }
    }
    Counter verdicts, disch, why, callee;
    long autoh = 0, discharged = 0;
    static const std::regex callee_re(R"(UNENCODED: call @([\w.$]+))");
    static const std::regex ob_re(R"(does not establish the harness: (ob-[a-z]+))");
    for (const auto& k : order) {
        const auto& j = rec[k];
        const std::string verdict = jstr(j, "verdict");
        verdicts.add(verdict);
        if (j.contains("harness") && j["harness"].is_object() && j["harness"].value("auto", false)) ++autoh;
        if (j.contains("assumptions") && j["assumptions"].is_array())
            for (const auto& a : j["assumptions"])
                if (a.is_string() && has(a.get<std::string>(), "the automatic harness is discharged")) {
                    ++discharged;
                    break;
                }
        const std::string d = jstr(j, "harness_discharge");
        if (!d.empty()) {
            std::smatch mm;
            if (starts(d, "discharged")) disch.add("discharged");
            else if (std::regex_search(d, mm, ob_re)) disch.add("not discharged: a call site fails " + mm[1].str());
            else {
                auto s = d.substr(0, d.find('('));
                s = s.substr(0, 90);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                disch.add(s);
            }
        }
        if (verdict == "NEEDS-HARNESS") {
            why.add(reason(j));
            std::smatch mm;
            const std::string m = jstr(j, "message");
            if (std::regex_search(m, mm, callee_re)) callee.add(mm[1].str());
        }
    }
    if (as_json) {
        nlohmann::ordered_json r = {{"functions", order.size()},
                            {"verdicts", counts_json(verdicts)},
                            {"results_with_auto_harness_assumption", autoh},
                            {"harness_discharged_results", discharged},
                            {"discharge_outcomes", counts_json(disch)},
                            {"needs_harness_by_reason", counts_json(why)},
                            {"top_unknown_callees", counts_json(callee, top)}};
        std::cout << r.dump(1, ' ', true) << "\n";
        return 0;
    }
    auto row = [](long n, const std::string& k) {
        std::string num = std::to_string(n);
        std::cout << "  " << std::string(num.size() < 6 ? 6 - num.size() : 0, ' ') << num << "  " << k << "\n";
    };
    std::cout << "functions: " << order.size() << "\nverdicts:\n";
    for (auto& [k, v] : verdicts.most_common()) {
        std::string num = std::to_string(v);
        std::cout << "  " << k << std::string(k.size() < 16 ? 16 - k.size() : 0, ' ') << " "
                  << std::string(num.size() < 6 ? 6 - num.size() : 0, ' ') << num << "\n";
    }
    std::cout << "results carrying an (auto harness) assumption: " << autoh << "\n"
              << "results whose harness was discharged at the call sites: " << discharged << "\n";
    if (!disch.items.empty()) {
        std::cout << "discharge outcomes (PROVED-ASSUMING under an automatic harness):\n";
        for (auto& [k, v] : disch.most_common()) row(v, k);
    }
    std::cout << "NEEDS-HARNESS by reason:\n";
    for (auto& [k, v] : why.most_common()) row(v, k);
    std::cout << "most frequent unknown callees:\n";
    long n = 0;
    for (auto& [k, v] : callee.most_common()) {
        if (n++ >= top) break;
        row(v, k);
    }
    return 0;
}

}  // namespace prism::qa
