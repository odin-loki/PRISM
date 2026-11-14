// Proof library, phase L1 (docs/PROOF_LIBRARY.md): import, index, stats, lookup.
#include "prism/library.hpp"

#include "prism/cache.hpp"
#include "prism/exit.hpp"
#include "prism/laws.hpp"
#include "prism/log.hpp"
#include "prism/pool.hpp"
#include "prism/threads.hpp"
#include "prism/verdict.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace prism::library {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kSchema = 1;
const char* const kIndexKinds[] = {"exact", "shape", "function"};

// Extra fields that are artefacts a later re-check can start from (L2).
const char* const kArtefacts[] = {"pir_invariants", "pir_invariants_surviving", "unwind", "unwind_closed",
                                  "unwind_deepened_to", "unwind_requested", "k_induction", "assumptions",
                                  "assumption", "certificate", "inlined", "cex", "prop", "nondet", "tv",
                                  "cxx_models", "frontend", "static_init", "globals"};

std::string level_of(const std::string& status) {
    if (status == laws::PROVED_CERTIFIED) return "L5c";
    if (status == laws::PROVED_UNBOUNDED) return "L5";
    if (status == laws::PROVED) return "L4";
    if (status == laws::PROVED_ASSUMING) return "L3";
    if (status == laws::BOUNDED) return "L2";
    if (status == laws::CLEAN) return "L1";
    return "L0";
}

// "UNENCODED: call @kmalloc (...)" -> "UNENCODED: call @kmalloc": the reason
// class counted for the P2 priority list (U4).
std::string reason_class(const std::string& message) {
    std::string m = message.substr(0, message.find('\n'));
    for (const char* cut : {" (", ": not", "; "})
        if (auto p = m.find(cut, 10); p != std::string::npos) m = m.substr(0, p);
    if (m.size() > 120) m = m.substr(0, 120);
    return m;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using Index = std::vector<std::pair<std::string, std::string>>;  // sorted (key, id)

Index load_index(const fs::path& lib, const std::string& kind) {
    Index ix;
    std::ifstream in(lib / "index" / kind);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        ix.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    std::sort(ix.begin(), ix.end());
    return ix;
}

bool save_index(const fs::path& lib, const std::string& kind, Index& ix) {
    std::sort(ix.begin(), ix.end());
    ix.erase(std::unique(ix.begin(), ix.end()), ix.end());
    std::error_code ec;
    fs::create_directories(lib / "index", ec);
    const auto tmp = lib / "index" / (kind + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (auto& [k, id] : ix) out << k << '\t' << id << '\n';
        if (!out) return false;
    }
    fs::rename(tmp, lib / "index" / kind, ec);
    return !ec;
}

struct Vetting {
    std::map<std::string, std::pair<std::string, std::string>> by_key;  // id or file::function -> (tier, note)
};

Vetting load_vetting(const fs::path& p, std::vector<std::string>& errors) {
    Vetting v;
    if (p.empty()) return v;
    std::ifstream in(p);
    if (!in) {
        errors.push_back("vetting file not readable: " + p.string());
        return v;
    }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            auto j = json::parse(line);
            const std::string tier = j.value("tier", "");
            if (tier != "T0" && tier != "T1" && tier != "T2" && tier != "T3" && tier != "Q") {
                errors.push_back(p.string() + ":" + std::to_string(n) + ": tier must be T0..T3 or Q");
                continue;
            }
            std::string key = j.contains("id") ? j.at("id").get<std::string>()
                                               : j.value("file", "") + "::" + j.value("function", "");
            v.by_key[key] = {tier, j.value("note", "")};
        } catch (const std::exception& ex) {
            errors.push_back(p.string() + ":" + std::to_string(n) + ": " + ex.what());
        }
    }
    return v;
}

json load_summary(const fs::path& lib) {
    auto txt = read_file(lib / "summary.json");
    if (!txt.empty()) try {
            auto j = json::parse(txt);
            if (j.value("schema", 0) == kSchema) return j;
        } catch (...) {
        }
    return json{{"schema", kSchema}, {"entries", 0}, {"by_status", json::object()}, {"by_tier", json::object()},
                {"by_level", json::object()}, {"by_stage", json::object()}, {"needs_harness", json::object()},
                {"runs", json::array()}};
}

void bump(json& j, const std::string& k, int d = 1) { j[k] = j.value(k, 0) + d; }

std::string identity_of(const json& e) {
    json idn = e;
    idn.erase("tier");
    idn.erase("vetting");
    idn.erase("id");
    return prism::cache::key_of(idn.dump());
}

}  // namespace

json entry_of(const json& row, const json& meta, const std::string& run, const std::string& tier) {
    const auto extra = row.value("extra", json::object());
    auto xs = [&](const char* k) { return extra.contains(k) && extra[k].is_string() ? extra[k].get<std::string>() : std::string(); };
    json e;
    e["schema"] = kSchema;
    e["stage"] = row.value("stage", "");
    e["file"] = row.value("file", "");
    e["function"] = row.contains("function") && row["function"].is_string() ? row["function"].get<std::string>() : "";
    e["line"] = row.contains("line") && row["line"].is_number() ? row["line"].get<int>() : 0;
    std::string status = row.value("status", "");
    // The verdict audit the pipeline runs (prism::verdict::audit): a row whose
    // stage may not write its status is imported with the audited status.
    if (auto v = verdict::parse(status)) {
        const bool cert = xs(std::string(laws::CERTIFICATE_KEY).c_str()) == laws::CERTIFICATE_CHECKED;
        auto a = verdict::audit(e["stage"].get<std::string>(), *v, cert);
        if (a.violation) {
            e["audit_demoted_from"] = status;
            status = std::string(verdict::name(a.status));
        }
    }
    e["status"] = status;
    e["level"] = level_of(status);
    e["cls"] = row.value("cls", "");
    e["message"] = row.value("message", "");
    e["counterexample"] = row.value("counterexample", "");
    json keys;
    if (auto k = xs("pir_fingerprint"); !k.empty()) keys["exact"] = k;
    if (auto k = xs("pir_shape"); !k.empty()) keys["shape"] = k;
    if (auto k = xs("pir_hash"); !k.empty()) keys["pir_hash"] = k;
    keys["function"] = e["file"].get<std::string>() + "::" + e["function"].get<std::string>();
    e["keys"] = keys;
    json art = json::object();
    for (const char* k : kArtefacts)
        if (extra.contains(k)) art[k] = extra[k];
    e["artefacts"] = art;
    if (auto f = xs("features"); !f.empty()) try {
            e["features"] = json::parse(f);
        } catch (...) {
        }
    e["provenance"] = {{"run", run}, {"root", meta.value("root", "")}, {"started", meta.value("started", "")}};
    e["tier"] = tier;
    return e;
}

ImportStats import_runs(const std::vector<fs::path>& runs, const ImportOptions& opt) {
    ImportStats st;
    auto vet = load_vetting(opt.vetting, st.errors);
    std::vector<fs::path> reports;
    for (const auto& r : runs) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) reports.push_back(r / "report.json");
        else reports.push_back(r);
    }
    st.reports = reports.size();
    // Parse on --jobs threads (the reports are the cost), merge in order.
    std::vector<std::vector<json>> parsed(reports.size());
    std::vector<std::string> errs(reports.size());
    std::vector<std::size_t> nrows(reports.size(), 0);
    pool::for_each_index(opt.jobs, reports.size(), [&](std::size_t i) {
        try {
            auto txt = read_file(reports[i]);
            if (txt.empty()) {
                errs[i] = "not readable or empty: " + reports[i].string();
                return;
            }
            auto rep = json::parse(txt);
            json meta{{"root", rep.value("root", "")}, {"started", rep.value("started", "")}};
            std::vector<const json*> rows;
            if (rep.contains("stages") && rep["stages"].is_array())
                for (auto& s : rep["stages"])
                    if (s.contains("findings") && s["findings"].is_array())
                        for (auto& f : s["findings"]) rows.push_back(&f);
            if (rep.contains("findings") && rep["findings"].is_array())
                for (auto& f : rep["findings"]) rows.push_back(&f);
            const auto run = fs::absolute(reports[i]).parent_path().string();
            for (auto* f : rows) {
                const std::string stage = f->value("stage", "");
                if (std::find(opt.stages.begin(), opt.stages.end(), stage) == opt.stages.end()) continue;
                if (!f->contains("function") || !(*f)["function"].is_string()) continue;  // unit-level rows
                ++nrows[i];
                parsed[i].push_back(entry_of(*f, meta, run, opt.tier));
            }
        } catch (const std::exception& ex) {
            errs[i] = reports[i].string() + ": " + ex.what();
        }
    });
    std::map<std::string, Index> ix;
    for (const char* k : kIndexKinds) ix[k] = load_index(opt.library, k);
    std::set<std::string> known;
    for (auto& [k, id] : ix["function"]) known.insert(id);
    auto sum = load_summary(opt.library);
    const fs::path store = opt.library / "entries";
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (!errs[i].empty()) {
            st.errors.push_back(errs[i]);
            continue;
        }
        st.rows += nrows[i];
        sum["runs"].push_back(fs::absolute(reports[i]).parent_path().string());
        for (auto& e : parsed[i]) {
            const auto fk = e["keys"]["function"].get<std::string>();
            // identity: what was observed (the row and its run), not its tier
            const auto id = identity_of(e);
            for (const auto* k : {&fk, &id})
                if (auto it = vet.by_key.find(*k); it != vet.by_key.end()) {
                    e["tier"] = it->second.first;
                    if (!it->second.second.empty()) e["vetting"] = it->second.second;
                }
            if (known.count(id)) {
                ++st.duplicates;
                continue;
            }
            if (!prism::cache::put(store, id, e.dump())) {
                st.errors.push_back("could not write entry " + id + " under " + store.string());
                continue;
            }
            known.insert(id);
            ++st.imported;
            if (e.contains("audit_demoted_from")) ++st.demoted;
            for (const char* k : kIndexKinds)
                if (e["keys"].contains(k)) ix[k].emplace_back(e["keys"][k].get<std::string>(), id);
            sum["entries"] = sum.value("entries", 0) + 1;
            bump(sum["by_status"], e["status"].get<std::string>());
            bump(sum["by_tier"], e["tier"].get<std::string>());
            bump(sum["by_level"], e["level"].get<std::string>());
            bump(sum["by_stage"], e["stage"].get<std::string>());
            if (e["status"] == laws::NEEDS_HARNESS || e["status"] == laws::NOTRUN)
                bump(sum["needs_harness"], reason_class(e["message"].get<std::string>()));
        }
    }
    for (const char* k : kIndexKinds)
        if (!save_index(opt.library, k, ix[k])) st.errors.push_back(std::string("could not write index ") + k);
    {
        std::error_code ec;
        fs::create_directories(opt.library, ec);
        std::ofstream(opt.library / "summary.json") << sum.dump(1) << "\n";
    }
    return st;
}

json summary(const fs::path& library) { return load_summary(library); }

std::pair<std::size_t, std::vector<std::string>> vet(const fs::path& library, const fs::path& vetting) {
    std::vector<std::string> errors;
    auto v = load_vetting(vetting, errors);
    auto fix = load_index(library, "function");
    auto sum = load_summary(library);
    std::size_t changed = 0;
    for (auto& [key, tn] : v.by_key) {
        std::vector<std::string> ids;
        auto it = std::lower_bound(fix.begin(), fix.end(), std::make_pair(key, std::string()));
        for (; it != fix.end() && it->first == key; ++it) ids.push_back(it->second);
        if (ids.empty() && key.find("::") == std::string::npos) ids.push_back(key);  // an entry id
        for (auto& id : ids) {
            auto txt = prism::cache::get(library / "entries", id);
            if (!txt) {
                errors.push_back("no entry " + id + " for " + key);
                continue;
            }
            auto e = json::parse(*txt);
            const std::string old_tier = e.value("tier", "T0");
            if (old_tier == tn.first && e.value("vetting", "") == tn.second) continue;
            e["tier"] = tn.first;
            if (!tn.second.empty()) e["vetting"] = tn.second;
            if (!prism::cache::put(library / "entries", id, e.dump())) {
                errors.push_back("could not rewrite entry " + id);
                continue;
            }
            bump(sum["by_tier"], old_tier, -1);
            bump(sum["by_tier"], tn.first);
            ++changed;
        }
    }
    std::ofstream(library / "summary.json") << sum.dump(1) << "\n";
    return {changed, errors};
}

std::vector<json> lookup(const fs::path& library, const std::string& kind, const std::string& key) {
    std::vector<json> out;
    auto ix = load_index(library, kind);
    auto it = std::lower_bound(ix.begin(), ix.end(), std::make_pair(key, std::string()));
    for (; it != ix.end() && it->first == key; ++it) {
        auto txt = prism::cache::get(library / "entries", it->second);
        if (!txt) continue;  // missing or corrupt (digest-checked): never served
        try {
            auto e = json::parse(*txt);
            e["id"] = it->second;
            out.push_back(std::move(e));
        } catch (...) {
        }
    }
    return out;
}

int library_main(int argc, char** argv) {
    const char* usage =
        "prism library import RUN_DIR|report.json... [--library DIR] [--tier T0|T1] [--vetting FILE.jsonl]\n"
        "                     [--stage NAME]... [--jobs N]\n"
        "prism library vet VETTING.jsonl [--library DIR]   (re-tier entries: {file,function|id,tier,note})\n"
        "prism library stats [--library DIR] [--json]\n"
        "prism library lookup (--exact KEY | --shape KEY | --function FILE::NAME) [--library DIR]\n"
        "Proof library (docs/PROOF_LIBRARY.md): earlier verdict rows with a trust tier, indexed by\n"
        "extra.pir_fingerprint, extra.pir_shape and file::function. An entry is data, never a verdict.\n"
        "Default --library prism-library, --tier T0, --stage pir.\n";
    if (argc < 1) {
        std::cerr << usage;
        return code(Exit::Usage);
    }
    const std::string sub = argv[0];
    ImportOptions opt;
    bool json_out = false, stages_given = false;
    std::string kind, key;
    std::vector<fs::path> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--library") opt.library = next();
        else if (a == "--tier") opt.tier = next();
        else if (a == "--vetting") opt.vetting = next();
        else if (a == "--jobs" || a == "-j") opt.jobs = std::atoi(next().c_str());
        else if (a == "--stage") {
            if (!stages_given) opt.stages.clear();
            stages_given = true;
            opt.stages.push_back(next());
        } else if (a == "--json") json_out = true;
        else if (a == "--exact" || a == "--shape" || a == "--function") {
            kind = a.substr(2);
            key = next();
        } else if (a == "-h" || a == "--help") {
            std::cout << usage;
            return code(Exit::Ok);
        } else if (!a.starts_with("-")) pos.emplace_back(a);
        else return fail("library", "unrecognized argument: " + a);
    }
    if (opt.tier != "T0" && opt.tier != "T1" && opt.tier != "T2" && opt.tier != "T3")
        return fail("library", "--tier must be T0, T1, T2 or T3 (Q comes from --vetting)");
    if (sub == "import") {
        if (pos.empty()) return fail("library", "import needs at least one run directory or report.json");
        auto st = import_runs(pos, opt);
        json j{{"reports", st.reports}, {"rows", st.rows},           {"imported", st.imported},
               {"duplicates", st.duplicates}, {"demoted_by_audit", st.demoted}, {"errors", st.errors}};
        std::cout << j.dump(json_out ? -1 : 1) << "\n";
        return code(st.errors.empty() ? Exit::Ok : Exit::Failed);
    }
    if (sub == "vet") {
        if (opt.vetting.empty() && !pos.empty()) opt.vetting = pos.front();
        if (opt.vetting.empty()) return fail("library", "vet needs a vetting file (JSONL)");
        auto [n, errors] = vet(opt.library, opt.vetting);
        std::cout << json{{"changed", n}, {"errors", errors}}.dump(json_out ? -1 : 1) << "\n";
        return code(errors.empty() ? Exit::Ok : Exit::Failed);
    }
    if (sub == "stats") {
        auto s = summary(opt.library);
        if (json_out) {
            std::cout << s.dump() << "\n";
            return code(Exit::Ok);
        }
        std::cout << "entries " << s.value("entries", 0) << " from " << s["runs"].size() << " run(s)\n";
        for (const char* part : {"by_tier", "by_level", "by_status", "by_stage"}) {
            std::cout << part << ":";
            for (auto& [k, v] : s[part].items()) std::cout << " " << k << "=" << v.get<long long>();
            std::cout << "\n";
        }
        std::vector<std::pair<long long, std::string>> nh;
        for (auto& [k, v] : s["needs_harness"].items()) nh.emplace_back(-v.get<long long>(), k);
        std::sort(nh.begin(), nh.end());
        std::cout << "top NEEDS-HARNESS / NOTRUN reasons (what to model next):\n";
        for (std::size_t i = 0; i < nh.size() && i < 25; ++i) std::cout << "  " << -nh[i].first << "  " << nh[i].second << "\n";
        return code(Exit::Ok);
    }
    if (sub == "lookup") {
        if (kind.empty()) return fail("library", "lookup needs --exact, --shape or --function");
        auto es = lookup(opt.library, kind, key);
        for (auto& e : es) std::cout << e.dump(json_out ? -1 : 1) << "\n";
        return code(es.empty() ? Exit::Failed : Exit::Ok);
    }
    return fail("library", "unknown subcommand '" + sub + "' (import, vet, stats, lookup)");
}

}  // namespace prism::library
