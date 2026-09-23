// Triage and deduplication (roadmap 9.3; embeddings roadmap 9.1).
//
// Findings are clustered by root cause and the clusters are ranked. This
// affects ORDERING ONLY: triage reads a const RunReport and writes
// <out>/triage.json plus a "## Clusters" section of report.md. It never
// touches a status (tests/cpp/test_ai_assist.cpp locks that).
//
// The deterministic embedding is a TF-IDF vector over
//   * tokens: the finding class (weighted), function, file base name, the
//     normalised message words and bigrams, identifiers of the code snippet
//     at the finding's line;
//   * character 3-grams of the normalised message + snippet;
// compared by cosine similarity; pairs above a threshold are joined with
// union-find. Only pairs that share a file, a class or a function are
// compared (blocking), so large reports stay near-linear.
//
// When a small code embedding model is reachable (llama.cpp /embedding at
// PRISM_EMBED_SERVER), the similarity is the mean of the TF-IDF cosine and
// the model cosine. Without it the TF-IDF result is used and triage.json says
// why (Law 7).

#include "ai_internal.hpp"

#include "prism/ai_assist.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

namespace prism::ai {
namespace fs = std::filesystem;

// ---------------------------------------------------------------- finding ids
std::vector<FindingRef> enumerate_findings(const RunReport& report) {
    std::vector<FindingRef> out;
    for (const auto& s : report.stages)
        for (std::size_t i = 0; i < s.findings.size(); ++i)
            out.push_back({s.name + "#" + std::to_string(i), &s.findings[i]});
    return out;
}

const Finding* find_by_id(const RunReport& report, const std::string& id) {
    auto h = id.rfind('#');
    if (h == std::string::npos) return nullptr;
    auto stage = id.substr(0, h);
    std::size_t idx = 0;
    try {
        std::size_t used = 0;
        auto n = std::stoul(id.substr(h + 1), &used);
        if (used != id.size() - h - 1) return nullptr;
        idx = n;
    } catch (...) {
        return nullptr;
    }
    for (const auto& s : report.stages)
        if (s.name == stage) return idx < s.findings.size() ? &s.findings[idx] : nullptr;
    return nullptr;
}

// ---------------------------------------------------------------- embedder
Embedder::~Embedder() = default;

namespace {

std::mutex g_embed_mu;
std::shared_ptr<Embedder> g_embed_test;
bool g_embed_test_set = false;

class LlamaEmbedder final : public Embedder {
public:
    explicit LlamaEmbedder(std::string url) : url_(std::move(url)) {}
    std::string name() const override { return "llama-server-embedding:" + url_; }
    std::optional<std::vector<double>> embed(const std::string& text) override {
        nlohmann::json body{{"content", text}};
        auto resp = http_request_raw("POST", url_ + "/embedding", body.dump(), 30000);
        if (!resp) return std::nullopt;
        try {
            auto j = nlohmann::json::parse(*resp);
            // {"embedding": [...]}, or [{"index":0,"embedding":[[...]]}] (newer servers)
            const nlohmann::json* e = nullptr;
            if (j.is_object() && j.contains("embedding")) e = &j["embedding"];
            else if (j.is_array() && !j.empty() && j[0].is_object() && j[0].contains("embedding")) e = &j[0]["embedding"];
            if (!e) return std::nullopt;
            const nlohmann::json* v = e;
            if (v->is_array() && !v->empty() && (*v)[0].is_array()) v = &(*v)[0];
            std::vector<double> out;
            for (auto& x : *v) {
                if (!x.is_number()) return std::nullopt;
                out.push_back(x.get<double>());
            }
            if (out.empty()) return std::nullopt;
            return out;
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    std::string url_;
};

}  // namespace

std::shared_ptr<Embedder> connect_embedder(std::string* why) {
    {
        std::lock_guard<std::mutex> g(g_embed_mu);
        if (g_embed_test_set) {
            if (!g_embed_test && why) *why = "test: no embedder";
            return g_embed_test;
        }
    }
    const char* url = std::getenv("PRISM_EMBED_SERVER");
    if (!url || !*url) {
        if (why) *why = "NOTRUN: no embedding model (set PRISM_EMBED_SERVER to a llama.cpp server started with "
                        "--embedding); deterministic TF-IDF embedding used";
        return nullptr;
    }
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (!http_request_raw("GET", u + "/health", {}, 1500)) {
        if (why) *why = "NOTRUN: embedding server " + u + " not reachable; deterministic TF-IDF embedding used";
        return nullptr;
    }
    return std::make_shared<LlamaEmbedder>(u);
}

void set_embedder_for_testing(std::shared_ptr<Embedder> e) {
    std::lock_guard<std::mutex> g(g_embed_mu);
    g_embed_test = std::move(e);
    g_embed_test_set = static_cast<bool>(g_embed_test);
}

// ---------------------------------------------------------------- features
namespace {

const std::set<std::string>& triage_statuses() {
    static const std::set<std::string> s = {
        std::string(laws::CRASH),   std::string(laws::FAILED),        std::string(laws::SANFAIL),
        std::string(laws::ERROR),   std::string(laws::TIMEOUT),       std::string(laws::UNKNOWN),
        std::string(laws::NEEDS_HARNESS), std::string(laws::BOUNDED), std::string(laws::HYPOTHESIS)};
    return s;
}

int severity(const std::string& st) {
    static const std::vector<std::string> order = {"CRASH", "FAILED", "SANFAIL", "ERROR", "TIMEOUT",
                                                   "UNKNOWN", "NEEDS-HARNESS", "BOUNDED", "HYPOTHESIS"};
    auto it = std::find(order.begin(), order.end(), st);
    return it == order.end() ? 99 : static_cast<int>(it - order.begin());
}

int stage_pos(const std::string& stage) {
    int i = 0;
    for (auto* p = STAGE_ORDER; *p; ++p, ++i)
        if (stage == *p) return i;
    return 999;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Numbers, hex and bit-vector literals -> "0"; quotes and paths flattened.
std::string normalise(const std::string& in) {
    std::string s = lower(in);
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool start_num = std::isdigit(c) && (i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_'));
        if (c == '#' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'b')) start_num = true;
        if (start_num) {
            std::size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            o += '0';
            i = j;
            continue;
        }
        o += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : ' ';
        ++i;
    }
    std::istringstream is(o);
    std::string w, r;
    while (is >> w) r += (r.empty() ? "" : " ") + w;
    return r;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> out;
    for (std::string w; is >> w;) out.push_back(w);
    return out;
}

std::string snippet_for(const Finding& f, const fs::path& root) {
    if (f.file.empty() || !f.line || *f.line <= 0) return {};
    fs::path p(f.file);
    std::error_code ec;
    if (!p.is_absolute()) {
        fs::path r = root;
        if (fs::is_regular_file(r, ec)) r = r.parent_path();
        if (fs::is_regular_file(r / p, ec)) p = r / p;
    }
    if (!fs::is_regular_file(p, ec)) return {};
    if (fs::file_size(p, ec) > (8u << 20)) return {};
    std::ifstream in(p, std::ios::binary);
    std::string line, out;
    for (int n = 1; std::getline(in, line); ++n) {
        if (n >= *f.line - 1 && n <= *f.line + 1) out += line + "\n";
        if (n > *f.line + 1) break;
    }
    return out.substr(0, 600);
}

using Vec = std::vector<std::pair<std::uint32_t, double>>;  // sorted by feature id

struct Space {
    std::unordered_map<std::string, std::uint32_t> ids;
    std::vector<int> df;
    std::uint32_t id(const std::string& k) {
        auto [it, fresh] = ids.emplace(k, static_cast<std::uint32_t>(ids.size()));
        if (fresh) df.push_back(0);
        return it->second;
    }
};

// Weighted raw features of one text (the output of triage_text).
std::map<std::string, double> raw_features(const std::string& text) {
    std::map<std::string, double> tf;
    // triage_text lines: "cls <x>", "fn <x>", "file <x>", "msg <...>", "code <...>"
    std::istringstream is(text);
    std::string line, chars;
    while (std::getline(is, line)) {
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        auto key = line.substr(0, sp);
        auto val = line.substr(sp + 1);
        if (key == "cls" || key == "fn" || key == "file" || key == "status") {
            if (!val.empty()) tf[key + ":" + val] += key == "cls" ? 3.0 : key == "status" ? 0.5 : 2.0;
            for (auto& w : words(normalise(val)))
                if (w.size() > 1) tf["w:" + w] += 1.0;
            continue;
        }
        auto ws = words(val);
        for (std::size_t i = 0; i < ws.size(); ++i) {
            if (ws[i].size() <= 1 && ws[i] != "0") continue;
            tf["w:" + ws[i]] += 1.0;
            if (i + 1 < ws.size()) tf["b:" + ws[i] + "_" + ws[i + 1]] += 1.0;
        }
        chars += " " + val;
    }
    for (std::size_t i = 0; i + 3 <= chars.size(); ++i) {
        auto g = chars.substr(i, 3);
        if (g.find("  ") != std::string::npos) continue;
        tf["c:" + g] += 0.3;
    }
    return tf;
}

std::vector<Vec> tfidf_vectors(const std::vector<std::string>& texts) {
    Space sp;
    std::vector<std::map<std::uint32_t, double>> raw(texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        for (auto& [k, v] : raw_features(texts[i])) raw[i][sp.id(k)] += v;
        for (auto& [id, v] : raw[i]) ++sp.df[id];
    }
    const double n = static_cast<double>(texts.size());
    std::vector<Vec> out(texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        double norm = 0;
        for (auto& [id, v] : raw[i]) {
            double w = (1.0 + std::log(v > 0 ? v : 1.0)) * (std::log((1.0 + n) / (1.0 + sp.df[id])) + 1.0);
            out[i].emplace_back(id, w);
            norm += w * w;
        }
        norm = std::sqrt(norm);
        if (norm > 0)
            for (auto& [id, w] : out[i]) w /= norm;
    }
    return out;
}

double dot(const Vec& a, const Vec& b) {
    double s = 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].first == b[j].first) s += a[i++].second * b[j++].second;
        else if (a[i].first < b[j].first) ++i;
        else ++j;
    }
    return s;
}

double dense_cos(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double d = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return na > 0 && nb > 0 ? d / std::sqrt(na * nb) : 0.0;
}

struct UnionFind {
    std::vector<std::size_t> p;
    explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    std::size_t find(std::size_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a != b) p[std::max(a, b)] = std::min(a, b);
    }
};

}  // namespace

std::string triage_text(const Finding& f, const std::string& snippet) {
    std::string t;
    t += "cls " + lower(f.cls) + "\n";
    t += "fn " + lower(f.function.value_or("")) + "\n";
    t += "file " + lower(fs::path(f.file).filename().string()) + "\n";
    t += "status " + lower(f.status) + "\n";
    t += "msg " + normalise(f.message) + "\n";
    if (!snippet.empty()) t += "code " + normalise(snippet) + "\n";
    return t;
}

double cosine_tfidf(const std::string& a, const std::string& b, const std::vector<std::string>& corpus) {
    std::vector<std::string> texts = corpus;
    texts.push_back(a);
    texts.push_back(b);
    auto v = tfidf_vectors(texts);
    return dot(v[v.size() - 2], v[v.size() - 1]);
}

TriageResult triage(const RunReport& report, const TriageOptions& opt) {
    TriageResult res;
    res.threshold = opt.threshold;
    res.embedder = "tfidf(tokens+bigrams+char3)";
    const fs::path root = opt.root.empty() ? fs::path(report.root) : opt.root;
    std::vector<FindingRef> items;
    for (auto& r : enumerate_findings(report))
        if (triage_statuses().contains(r.f->status)) items.push_back(r);
    res.considered = items.size();
    if (items.empty()) {
        res.embedder_note = "no findings to triage";
        return res;
    }
    std::vector<std::string> texts;
    texts.reserve(items.size());
    for (auto& r : items) texts.push_back(triage_text(*r.f, snippet_for(*r.f, root)));
    auto vecs = tfidf_vectors(texts);

    // Optional model embeddings (all-or-nothing: a partial set would make
    // similarities incomparable).
    std::vector<std::vector<double>> dense;
    if (opt.use_embedder) {
        std::string why;
        if (auto e = connect_embedder(&why)) {
            bool ok = items.size() <= 5000;
            for (std::size_t i = 0; ok && i < items.size(); ++i) {
                auto v = e->embed(texts[i]);
                if (!v) ok = false;
                else dense.push_back(std::move(*v));
            }
            if (ok) {
                res.embedder = "tfidf+" + e->name();
            } else {
                dense.clear();
                res.embedder_note = "embedding model " + e->name() +
                                    " failed on some findings; deterministic TF-IDF embedding used";
            }
        } else {
            res.embedder_note = why;
        }
    } else {
        res.embedder_note = "embedding model disabled; deterministic TF-IDF embedding used";
    }
    auto sim = [&](std::size_t i, std::size_t j) {
        double s = dot(vecs[i], vecs[j]);
        if (!dense.empty()) s = 0.5 * s + 0.5 * dense_cos(dense[i], dense[j]);
        return s;
    };

    // Blocking: only pairs sharing a file, class or function are compared.
    std::map<std::string, std::vector<std::size_t>> blocks;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& f = *items[i].f;
        if (!f.file.empty()) blocks["file:" + f.file].push_back(i);
        if (!f.cls.empty()) blocks["cls:" + lower(f.cls)].push_back(i);
        if (f.function && !f.function->empty()) blocks["fn:" + *f.function].push_back(i);
    }
    UnionFind uf(items.size());
    std::set<std::pair<std::size_t, std::size_t>> done;
    for (auto& [k, v] : blocks) {
        if (v.size() < 2 || v.size() > 3000) continue;
        for (std::size_t a = 0; a < v.size(); ++a)
            for (std::size_t b = a + 1; b < v.size(); ++b) {
                auto key = std::make_pair(v[a], v[b]);
                if (uf.find(v[a]) == uf.find(v[b]) || !done.insert(key).second) continue;
                if (sim(v[a], v[b]) >= opt.threshold) uf.unite(v[a], v[b]);
            }
    }
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < items.size(); ++i) groups[uf.find(i)].push_back(i);

    struct Pending {
        TriageCluster c;
        int sev;
        std::string file;
        int line;
    };
    std::vector<Pending> pend;
    for (auto& [root_i, mem] : groups) {
        // Representative: most severe, then one with a counterexample, then earliest stage.
        std::sort(mem.begin(), mem.end(), [&](std::size_t a, std::size_t b) {
            const auto& fa = *items[a].f;
            const auto& fb = *items[b].f;
            int sa = severity(fa.status), sb = severity(fb.status);
            if (sa != sb) return sa < sb;
            bool ca = !fa.counterexample.empty(), cb = !fb.counterexample.empty();
            if (ca != cb) return ca;
            int pa = stage_pos(fa.stage), pb = stage_pos(fb.stage);
            if (pa != pb) return pa < pb;
            return a < b;
        });
        Pending p;
        const auto& rep = *items[mem[0]].f;
        p.c.representative = items[mem[0]].id;
        p.c.top_status = rep.status;
        std::set<std::string> st;
        double coh = 0;
        for (auto i : mem) {
            p.c.members.push_back(items[i].id);
            st.insert(items[i].f->stage);
            coh += i == mem[0] ? 1.0 : sim(mem[0], i);
        }
        p.c.cohesion = coh / static_cast<double>(mem.size());
        p.c.stages.assign(st.begin(), st.end());
        std::sort(p.c.stages.begin(), p.c.stages.end(),
                  [](auto& a, auto& b) { return stage_pos(a) < stage_pos(b); });
        std::string what = !rep.cls.empty() ? rep.cls : rep.message.substr(0, 60);
        p.c.label = what;
        if (rep.function && !rep.function->empty()) p.c.label += " in " + *rep.function;
        if (!rep.file.empty()) p.c.label += " (" + fs::path(rep.file).filename().string() + ")";
        p.sev = severity(rep.status);
        p.file = rep.file;
        p.line = rep.line.value_or(0);
        pend.push_back(std::move(p));
    }
    // Rank: severity, then corroboration (distinct stages), then size, then location.
    std::sort(pend.begin(), pend.end(), [](const Pending& a, const Pending& b) {
        if (a.sev != b.sev) return a.sev < b.sev;
        if (a.c.stages.size() != b.c.stages.size()) return a.c.stages.size() > b.c.stages.size();
        if (a.c.members.size() != b.c.members.size()) return a.c.members.size() > b.c.members.size();
        if (a.file != b.file) return a.file < b.file;
        if (a.line != b.line) return a.line < b.line;
        return a.c.representative < b.c.representative;
    });
    int rank = 0;
    for (auto& p : pend) {
        p.c.rank = ++rank;
        p.c.id = "C" + std::to_string(rank);
        res.clusters.push_back(std::move(p.c));
    }
    return res;
}

std::string triage_json(const TriageResult& t) {
    nlohmann::json j;
    j["schema"] = 1;
    j["kind"] = "prism-triage";
    j["note"] = "Root-cause clusters order findings only; no status is changed (roadmap 9.3).";
    j["embedder"] = t.embedder;
    j["embedder_note"] = t.embedder_note;
    j["threshold"] = t.threshold;
    j["considered"] = t.considered;
    j["clusters"] = nlohmann::json::array();
    for (auto& c : t.clusters)
        j["clusters"].push_back({{"id", c.id}, {"rank", c.rank}, {"label", c.label},
                                 {"top_status", c.top_status}, {"stages", c.stages},
                                 {"representative", c.representative}, {"members", c.members},
                                 {"size", c.members.size()},
                                 {"cohesion", std::round(c.cohesion * 1000) / 1000}});
    return j.dump(2);
}

std::string triage_markdown(const TriageResult& t, const RunReport& report) {
    std::ostringstream o;
    o << "\n## Clusters\n\n";
    o << "Root-cause clusters (triage, roadmap 9.3): they order findings only; no status is changed. "
      << t.considered << " findings in " << t.clusters.size() << " clusters, embedding " << t.embedder
      << ", threshold " << t.threshold << ".";
    if (!t.embedder_note.empty()) o << " " << t.embedder_note << ".";
    o << " Full list: triage.json.\n\n";
    std::size_t shown = 0;
    for (auto& c : t.clusters) {
        if (++shown > 200) {
            o << "- ... " << (t.clusters.size() - 200) << " more clusters in triage.json\n";
            break;
        }
        const Finding* rep = find_by_id(report, c.representative);
        o << c.rank << ". **" << c.id << "** `" << c.top_status << "` " << c.label;
        if (rep && rep->line && !rep->file.empty()) o << " — " << rep->file << ":" << *rep->line;
        o << " — " << c.members.size() << (c.members.size() == 1 ? " finding" : " findings");
        if (c.stages.size() > 1) {
            o << " from " << c.stages.size() << " stages (";
            for (std::size_t i = 0; i < c.stages.size(); ++i) o << (i ? ", " : "") << c.stages[i];
            o << ")";
        }
        o << ": ";
        for (std::size_t i = 0; i < c.members.size() && i < 12; ++i) o << (i ? ", " : "") << "`" << c.members[i] << "`";
        if (c.members.size() > 12) o << ", ...";
        o << "\n";
    }
    return o.str();
}

void write_triage(const RunReport& report, const fs::path& out, const TriageOptions& opt) {
    auto t = triage(report, opt);
    std::error_code ec;
    fs::create_directories(out, ec);
    std::ofstream(out / "triage.json", std::ios::binary) << triage_json(t);
    std::ofstream(out / "report.md", std::ios::binary | std::ios::app) << triage_markdown(t, report);
}

}  // namespace prism::ai
