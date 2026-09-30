// Per-feature measurements for docs/AI.md (roadmap 9.7), and the seeded
// sample of triage merges for manual review.
//
//     prism_ai measure triage OUT/            # clusters vs exact-key dedup
//     prism_ai measure ask OUT/ tests/data/ask_questions.jsonl --prism build/prism
//     prism_ai measure regress OUT/           # manifest of `prism regress --run`
//     prism_ai measure draft OUT/             # draft_*.json of `prism draft`
//     prism_ai sample-pairs OUT/ [--n 40] [--seed 1]
//
// Each measure prints one JSON object. Every metric compares against a no-AI
// baseline where one exists; the numbers go into docs/AI.md.

#include "prism_ai.hpp"
#include "pycompat.hpp"

#include "prism/config.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace prism_ai::measure {

namespace {

const std::set<std::string> kTriageStatuses = {"FAILED",  "CRASH",         "SANFAIL", "ERROR",     "TIMEOUT",
                                               "UNKNOWN", "NEEDS-HARNESS", "BOUNDED", "HYPOTHESIS"};

ojson read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return ojson::parse(in);
}

const ojson& get_or_null(const ojson& o, const char* k) {
    static const ojson null;
    return o.is_object() && o.contains(k) ? o[k] : null;
}

bool truthy(const ojson& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.empty();
}

// report.json findings keyed "<stage>#<index>" (the triage.json member ids).
std::vector<std::pair<std::string, ojson>> findings(const ojson& report) {
    std::vector<std::pair<std::string, ojson>> out;
    if (!report.contains("stages")) return out;
    for (const auto& st : report["stages"]) {
        if (!st.contains("findings")) continue;
        std::size_t i = 0;
        for (const auto& f : st["findings"]) out.emplace_back(py(st["name"]) + "#" + std::to_string(i++), f);
    }
    return out;
}

// Code-point aware helpers for the review listing.
std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
std::string utf8_prefix(const std::string& s, std::size_t n) {
    std::size_t cps = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (cps == n) break;
            ++cps;
        }
    }
    return s.substr(0, i);
}
std::string pad(const std::string& s, std::size_t w) {
    const auto n = utf8_len(s);
    return n >= w ? s : s + std::string(w - n, ' ');
}

}  // namespace

ojson pairwise(const std::vector<std::vector<std::string>>& groups, const std::map<std::string, std::string>& label) {
    // Pairwise precision / recall / F1 of a clustering against labels.
    std::set<std::pair<std::string, std::string>> same_cluster, same_label;
    auto add_pairs = [](std::vector<std::string> g, auto& into) {
        std::sort(g.begin(), g.end());
        for (std::size_t a = 0; a < g.size(); ++a)
            for (std::size_t b = a + 1; b < g.size(); ++b) into.insert({g[a], g[b]});
    };
    for (const auto& g : groups) add_pairs(g, same_cluster);
    std::map<std::string, std::vector<std::string>> by_label;
    for (const auto& [k, v] : label) by_label[v].push_back(k);
    for (const auto& [v, g] : by_label) add_pairs(g, same_label);
    std::size_t tp = 0;
    for (const auto& p : same_cluster) tp += same_label.count(p);
    const double p = same_cluster.empty() ? 1.0 : static_cast<double>(tp) / static_cast<double>(same_cluster.size());
    const double r = same_label.empty() ? 1.0 : static_cast<double>(tp) / static_cast<double>(same_label.size());
    const double f1 = (p + r) != 0.0 ? 2 * p * r / (p + r) : 0.0;
    return ojson{{"precision", py_round(p, 4)}, {"recall", py_round(r, 4)}, {"f1", py_round(f1, 4)},
                 {"groups", groups.size()}};
}

bool in_split(const std::string& file, const std::string& split) {
    // Deterministic half split by source file: "tune" (threshold chosen here) / "eval".
    if (split == "all") return true;
    const auto h = std::stoull(prism::sha256_hex(file).substr(0, 8), nullptr, 16) % 2;
    return (h == 0) == (split == "tune");
}

ojson measure_triage(const fs::path& out, const std::string& split, const std::optional<fs::path>& triage_json) {
    const auto report = read_json(out / "report.json");
    const auto tri = read_json(triage_json ? *triage_json : out / "triage.json");
    std::vector<std::pair<std::string, ojson>> fs_;
    std::set<std::string> keys;
    for (auto& [k, f] : findings(report)) {
        const auto& st = get_or_null(f, "status");
        if (!st.is_string() || !kTriageStatuses.count(st.get<std::string>())) continue;
        const auto& file = get_or_null(f, "file");
        if (!in_split(truthy(file) ? py(file) : "", split)) continue;
        fs_.emplace_back(k, f);
        keys.insert(k);
    }
    // Proxy root cause: the function (file + name); findings without one keep
    // their own (file, line). Documented as a proxy in docs/AI.md.
    std::map<std::string, std::string> label;
    std::set<std::string> labels;
    std::vector<std::pair<std::string, std::vector<std::string>>> base, exact;
    auto group = [](auto& into, const std::string& key, const std::string& member) {
        for (auto& [k, v] : into)
            if (k == key) {
                v.push_back(member);
                return;
            }
        into.emplace_back(key, std::vector<std::string>{member});
    };
    for (const auto& [k, f] : fs_) {
        const auto& fn = get_or_null(f, "function");
        const std::string l = py(get_or_null(f, "file")) + "::" +
                              (truthy(fn) ? py(fn) : "L" + py(get_or_null(f, "line")));
        label[k] = l;
        labels.insert(l);
        const ojson flc = ojson::array({get_or_null(f, "file"), get_or_null(f, "line"), get_or_null(f, "cls")});
        ojson flcm = flc;
        flcm.push_back(get_or_null(f, "message"));
        group(base, flcm.dump(), k);
        group(exact, flc.dump(), k);
    }
    auto values = [](const auto& g) {
        std::vector<std::vector<std::string>> out;
        for (const auto& [k, v] : g) out.push_back(v);
        return out;
    };
    std::vector<std::vector<std::string>> clusters;
    if (tri.contains("clusters"))
        for (const auto& c : tri["clusters"]) {
            std::vector<std::string> m;
            if (c.contains("members"))
                for (const auto& id : c["members"])
                    if (keys.count(py(id))) m.push_back(py(id));
            if (!m.empty()) clusters.push_back(m);
        }
    ojson r = ojson::object();
    r["findings"] = fs_.size();
    r["labels"] = labels.size();
    r["baseline_identical"] = pairwise(values(base), label);
    r["baseline_file_line_cls"] = pairwise(values(exact), label);
    r["triage"] = pairwise(clusters, label);
    r["embedder"] = get_or_null(tri, "embedder");
    return r;
}

ojson measure_ask(const fs::path& out, const fs::path& questions, const std::string& prism) {
    std::vector<ojson> rows = load_jsonl(questions);
    int ok = 0, fields_ok = 0, fields = 0;
    ojson misses = ojson::array();
    for (const auto& r : rows) {
        const auto q = py(r["question"]);
        auto p = e2e::run_with_env({prism, "ask", q, "--report", (out / "report.json").string(), "--json", "--no-llm"},
                                   {}, {}, 120.0);
        if (p.timed_out) throw std::runtime_error("prism ask timed out on: " + q);
        const auto got = ojson::parse(p.out).at("query");
        const auto& exp = r["expect"];
        bool good = true;
        for (auto it = exp.begin(); it != exp.end(); ++it) {
            ++fields;
            const auto& g = get_or_null(got, it.key().c_str());
            bool same;
            if (it.value().is_array() && g.is_array()) {
                std::vector<ojson> a(g.begin(), g.end()), b(it.value().begin(), it.value().end());
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                same = a == b;
            } else {
                same = g == it.value();
            }
            fields_ok += same;
            good = good && same;
        }
        ok += good;
        if (!good) {
            ojson gotk = ojson::object();
            for (auto it = exp.begin(); it != exp.end(); ++it) gotk[it.key()] = get_or_null(got, it.key().c_str());
            misses.push_back(ojson{{"question", q}, {"got", gotk}, {"expect", exp}});
        }
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, rows.size()));
    return ojson{{"questions", rows.size()},
                 {"exact", ok},
                 {"exact_share", py_round(ok / n, 4)},
                 {"field_accuracy", py_round(fields_ok / static_cast<double>(std::max(1, fields)), 4)},
                 {"misses", misses}};
}

ojson measure_regress(const fs::path& out) {
    const auto m = read_json(out / "regression_tests" / "manifest.json");
    ojson by = ojson::object();
    for (const auto& t : m.at("tests")) {
        const auto s = py(t.at("status"));
        by[s] = by.value(s, 0) + 1;
    }
    long long tests = 0;
    for (auto it = by.begin(); it != by.end(); ++it)
        if (it.key() != "unsupported" && it.key() != "duplicate") tests += it.value().get<long long>();
    const double rep = by.contains("reproduces") ? by["reproduces"].get<double>() : 0.0;
    return ojson{{"findings", m["tests"].size()},
                 {"by_status", by},
                 {"tests", tests},
                 {"reproduce_share", py_round(rep / static_cast<double>(std::max(1LL, tests)), 4)}};
}

ojson measure_draft(const fs::path& out) {
    ojson res = ojson::object();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(out, ec)) {
        const auto name = e.path().filename().string();
        if (name.starts_with("draft_") && name.ends_with(".json")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        const auto d = read_json(p);
        std::size_t claims = 0, linked = 0;
        for (const auto& s : d.at("sections"))
            for (const auto& c : s.at("claims")) {
                ++claims;
                linked += truthy(get_or_null(c, "links"));
            }
        res[p.stem().string()] = ojson{{"claims", claims},
                                       {"linked", linked},
                                       {"rejected", d.at("rejected").size()},
                                       {"theorems_indexed", get_or_null(d, "theorems_indexed")},
                                       {"author", get_or_null(d, "author")}};
    }
    return res;
}

int measure_main(int argc, char** argv, std::ostream& out, std::ostream& err) {
    auto usage = [&] {
        err << "usage: prism_ai measure {triage,ask,regress,draft} OUT [QUESTIONS] [--prism BIN] "
               "[--split all|tune|eval] [--triage-json FILE]\n";
        return 2;
    };
    std::vector<std::string> pos;
    std::string prism = "build/prism", split = "all";
    std::optional<fs::path> triage_json;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--prism" || a == "--split" || a == "--triage-json") && i + 1 >= argc) return usage();
        if (a == "--prism") prism = argv[++i];
        else if (a == "--split") split = argv[++i];
        else if (a == "--triage-json") triage_json = argv[++i];
        else if (a.starts_with("--")) return usage();
        else pos.push_back(a);
    }
    if (pos.size() < 2 || (split != "all" && split != "tune" && split != "eval")) return usage();
    const auto& what = pos[0];
    const fs::path o = pos[1];
    try {
        ojson r;
        if (what == "triage") r = measure_triage(o, split, triage_json);
        else if (what == "ask") {
            if (pos.size() < 3) {
                err << "prism_ai measure: ask needs a questions file\n";
                return 2;
            }
            r = measure_ask(o, pos[2], prism);
        } else if (what == "regress") r = measure_regress(o);
        else if (what == "draft") r = measure_draft(o);
        else return usage();
        out << r.dump(1) << "\n";
    } catch (const std::exception& e) {
        err << "prism_ai measure " << what << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int sample_pairs_main(int argc, char** argv, std::ostream& out, std::ostream& err) {
    // A seeded random sample of finding pairs that triage joins but the exact
    // (file, line, class) key does not, at most one per cluster, for manual
    // review (docs/AI.md "Triage": the reviewed precision of the merges
    // triage adds). The draws are CPython's random.Random(seed).shuffle, so
    // the documented sample is reproducible.
    std::optional<fs::path> dir;
    long long n = 40;
    std::uint64_t seed = 1;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) n = std::stoll(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (!a.starts_with("--") && !dir) dir = a;
        else {
            err << "usage: prism_ai sample-pairs OUT [--n 40] [--seed 1]\n";
            return 2;
        }
    }
    if (!dir) {
        err << "usage: prism_ai sample-pairs OUT [--n 40] [--seed 1]\n";
        return 2;
    }
    try {
        const auto rep = read_json(*dir / "report.json");
        const auto tri = read_json(*dir / "triage.json");
        std::map<std::string, ojson> fs_;
        for (auto& [k, f] : findings(rep)) fs_[k] = f;
        using Pair = std::pair<std::string, std::string>;
        std::vector<Pair> pairs;
        auto flc = [&](const ojson& f) {
            return ojson::array({get_or_null(f, "file"), get_or_null(f, "line"), get_or_null(f, "cls")});
        };
        for (const auto& c : tri.at("clusters")) {
            std::vector<std::string> members;
            for (const auto& m : c.at("members")) members.push_back(py(m));
            for (std::size_t a = 0; a < members.size(); ++a)
                for (std::size_t b = a + 1; b < members.size(); ++b)
                    if (flc(fs_.at(members[a])) != flc(fs_.at(members[b]))) pairs.emplace_back(members[a], members[b]);
        }
        PyRandom rnd(seed);
        // One pair per cluster at most, so a few huge clusters do not dominate.
        rnd.shuffle(pairs);
        std::map<std::string, std::string> member_of;
        for (const auto& c : tri["clusters"])
            for (const auto& m : c["members"]) member_of[py(m)] = c.at("id").dump();
        std::vector<std::pair<std::string, Pair>> by_cluster;
        for (const auto& p : pairs) {
            const auto& cid = member_of.at(p.first);
            if (std::none_of(by_cluster.begin(), by_cluster.end(), [&](const auto& e) { return e.first == cid; }))
                by_cluster.emplace_back(cid, p);
        }
        std::vector<Pair> sample;
        for (const auto& [c, p] : by_cluster) sample.push_back(p);
        rnd.shuffle(sample);
        const auto shown = static_cast<std::size_t>(std::max(0LL, std::min<long long>(n, static_cast<long long>(sample.size()))));
        out << pairs.size() << " added pairs in " << by_cluster.size() << " clusters; sample "
            << std::min<long long>(n, static_cast<long long>(sample.size())) << "\n";
        for (std::size_t i = 0; i < shown; ++i) {
            for (const auto& k : {sample[i].first, sample[i].second}) {
                const auto& f = fs_.at(k);
                const auto& fn = get_or_null(f, "function");
                const auto& msg = get_or_null(f, "message");
                out << "  " << pad(k, 14) << " " << pad(py(f.at("status")), 13) << " " << py(get_or_null(f, "file"))
                    << ":" << py(get_or_null(f, "line")) << " " << (truthy(fn) ? py(fn) : "-") << " "
                    << py(get_or_null(f, "cls")) << " | " << utf8_prefix(msg.is_null() ? "" : py(msg), 90) << "\n";
            }
            out << "\n";
        }
    } catch (const std::exception& e) {
        err << "prism_ai sample-pairs: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace prism_ai::measure
