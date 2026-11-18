// --jsonl: one JSON line per function (docs/PIR.md "Machine-readable output").

#include "jsonl.hpp"

#include "prism/laws.hpp"
#include "prism/membudget.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace prism::pir {

namespace {

using nlohmann::json;

std::string get(const Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

bool compiled(const Finding& f) {
    // a function row exists only when the front end produced IR for the unit;
    // a function cut out of the unit by the front end's salvage, or a unit-level
    // row (clang failed, no function), did not compile
    if (!f.function) return false;
    return f.extra.find("frontend_salvage") == f.extra.end();
}

std::optional<double> num(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* e = nullptr;
    const double v = std::strtod(s.c_str(), &e);
    if (e == s.c_str()) return std::nullopt;
    return v;
}

json assumptions_of(const Finding& f, const std::string& verdict) {
    json a = json::array();
    if (auto s = get(f, "assumptions_json"); !s.empty()) {
        auto j = json::parse(s, nullptr, false);
        if (j.is_array())
            for (auto& x : j) a.push_back(x);
    }
    if (auto u = get(f, "unwind"); !u.empty() && verdict != "NEEDS-HARNESS")
        a.push_back("bound: loops unwound " + u + " times" +
                    (verdict == "BOUNDED" ? " (nothing found within it; deeper iterations are not covered)"
                                          : " (closed: the unwinding assertions hold)"));
    if (auto s = get(f, "strict_aliasing"); !s.empty()) a.push_back("strict aliasing: " + s);
    if (auto s = get(f, "libm_unconstrained"); !s.empty())
        a.push_back("libm results unconstrained beyond their range facts: " + s);
    if (auto s = get(f, "throws_not_followed"); !s.empty())
        a.push_back("paths that throw end there (exceptions not followed): " + s);
    if (auto s = get(f, "globals"); !s.empty()) a.push_back("mutable globals: " + s);
    if (auto s = get(f, "inlined"); !s.empty())
        a.push_back("calls to functions of the unit are inlined (depth-bounded): " + s);
    if (auto s = get(f, "unknown_call_policy"); !s.empty()) {
        a.push_back("unknown-call policy: " + s);
    } else if (verdict != "NEEDS-HARNESS" && verdict != "ERROR" && verdict != "NOTRUN") {
        a.push_back("unknown-call policy: assume nothing; a call without a model makes the function "
                    "NEEDS-HARNESS (UNENCODED: call @f), so every call this verdict covers is modelled or inlined");
    }
    if (auto s = get(f, "front_end_flags"); !s.empty()) a.push_back("front end flags: " + s);
    if (auto s = get(f, "cxx_models"); !s.empty()) a.push_back("C++ library: " + s);
    return a;
}

std::string reason_key(std::string r) {
    // group "type x86_fp80" / "type i128" style variants by their first words
    if (r.size() > 90) r.resize(90);
    return r;
}

}  // namespace

std::string proof_level(const Finding& f) {
    const std::string& s = f.status;
    if (laws::is_proof(s)) return "L2";
    if (s == laws::BOUNDED) return "L1";
    if (compiled(f)) return "L0";
    return "";
}

bool hit_memory_limit(const Finding& f) {
    if (f.status != laws::UNKNOWN && f.status != laws::TIMEOUT) return false;
    const auto& m = f.message;
    return m.find("memory budget") != std::string::npos || m.find("machine memory low") != std::string::npos ||
           m.find("encoder's memory") != std::string::npos || m.find("soft memory cap") != std::string::npos;
}

json jsonl_record(const Finding& f, const Config& cfg) {
    json r;
    r["schema"] = "prism-pir-jsonl/1";
    r["record"] = f.function ? "function" : "unit";
    r["unit"] = f.file;
    r["function"] = f.function ? json(*f.function) : json(nullptr);
    std::optional<int> start, end;
    if (auto v = num(get(f, "start_line"))) start = static_cast<int>(*v);
    if (auto v = num(get(f, "end_line"))) end = static_cast<int>(*v);
    if (!start && f.line) start = f.line;
    r["start_line"] = start ? json(*start) : json(nullptr);
    r["end_line"] = end ? json(*end) : json(nullptr);
    r["key"] = (start && end) ? json(f.file + ":" + std::to_string(*start) + "-" + std::to_string(*end)) : json(nullptr);
    r["engine"] = "prism-pir";
#ifdef PRISM_VERSION
    r["engine_version"] = PRISM_VERSION;
#endif
    r["verdict"] = f.status;
    const std::string lvl = proof_level(f);
    r["level"] = lvl.empty() ? json(nullptr) : json(lvl);
    const bool mem = hit_memory_limit(f);
    r["outcome"] = mem ? "memory-limit" : (f.status == laws::TIMEOUT ? "timeout" : "complete");
    if (mem) {
        r["memory_budget_mb"] = membudget::budget_mb();
        r["memory_budget_source"] = membudget::budget_source();
    }
    r["message"] = f.message;
    if (!f.cls.empty()) r["class"] = f.cls;
    if (!f.counterexample.empty()) r["counterexample"] = f.counterexample;
    json a = assumptions_of(f, f.status);
    r["assumptions"] = a;
    r["conditional"] = !get(f, "assumptions_json").empty() && laws::is_proof(f.status);
    // harness: pointer parameters bound to objects, and by what
    json h;
    h["auto"] = false;
    if (auto s = get(f, "assumptions_json"); !s.empty()) {
        auto j = json::parse(s, nullptr, false);
        if (j.is_array())
            for (auto& x : j)
                if (x.is_string() && x.get<std::string>().find("(auto harness)") != std::string::npos) h["auto"] = true;
    }
    {
        json pp = json::array();
        std::string cur;
        for (char ch : get(f, "ptr_params") + ",") {
            if (ch == ',') {
                if (!cur.empty()) pp.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        h["pointer_parameters_bound"] = pp;
    }
    r["harness"] = h;
    // --pir-discharge / --pir-vacuity (docs/HARNESS_GENERATOR.md)
    for (const char* k : {"harness_discharge", "discharge_entry", "non_vacuity"})
        if (auto v = get(f, k); !v.empty()) r[k] = v;
    if (auto v = num(get(f, "properties"))) r["vcs"] = static_cast<long long>(*v);
    else r["vcs"] = nullptr;
    if (auto v = num(get(f, "unwind"))) r["unwind"] = static_cast<int>(*v);
    if (auto v = num(get(f, "time_s"))) r["time_s"] = *v;
    else r["time_s"] = nullptr;
    if (auto v = num(get(f, "peak_mb"))) r["peak_mb"] = static_cast<long long>(*v);
    else r["peak_mb"] = nullptr;
    json lean;
    if (auto s = get(f, "lean_supported"); !s.empty()) {
        lean["supported"] = s == "yes";
        if (s != "yes") lean["reason"] = get(f, "lean_reason");
    } else {
        lean["supported"] = nullptr;
        lean["reason"] = "not computed (no translation for this row)";
    }
    r["lean"] = lean;
    if (auto s = get(f, "certificate"); !s.empty()) r["certificate"] = s;
    (void)cfg;
    return r;
}

void write_jsonl(const Config& cfg, const std::vector<Finding>& findings) {
    if (cfg.jsonl.empty()) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    std::string text;
    long long n_fn = 0, n_harness = 0, n_lean = 0, n_nolean = 0, n_mem = 0;
    std::map<std::string, long long> by_verdict, by_level, by_reason;
    for (const auto& f : findings) {
        auto rec = jsonl_record(f, cfg);
        text += rec.dump() + "\n";
        if (!f.function) continue;
        ++n_fn;
        ++by_verdict[f.status];
        ++by_level[rec["level"].is_null() ? std::string("none") : rec["level"].get<std::string>()];
        if (rec["harness"]["auto"].get<bool>()) ++n_harness;
        if (hit_memory_limit(f)) ++n_mem;
        const auto& lean = rec["lean"];
        if (lean["supported"].is_boolean()) {
            if (lean["supported"].get<bool>()) {
                ++n_lean;
            } else {
                ++n_nolean;
                ++by_reason[reason_key(lean["reason"].get<std::string>())];
            }
        }
    }
    std::error_code ec;
    if (cfg.jsonl.has_parent_path()) std::filesystem::create_directories(cfg.jsonl.parent_path(), ec);
    {
        std::ofstream out(cfg.jsonl, std::ios::app | std::ios::binary);
        out << text;
        out.flush();
    }
    json s;
    s["schema"] = "prism-pir-jsonl-summary/1";
    s["functions"] = n_fn;
    s["by_verdict"] = by_verdict;
    s["by_level"] = by_level;
    s["auto_harness_functions"] = n_harness;
    s["memory_limit"] = n_mem;
    s["lean"] = {{"inside_proved_fragment", n_lean}, {"outside", n_nolean}, {"outside_by_reason", by_reason}};
    auto sp = cfg.jsonl;
    sp += ".summary.json";
    std::ofstream(sp, std::ios::trunc) << s.dump(2) << "\n";
}

}  // namespace prism::pir
