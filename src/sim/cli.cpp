// `prism sim`: build and query a symbolic-similarity index over an SMT-LIB corpus.

#include "prism/sim/cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "prism/exit.hpp"
#include "prism/sim/index.hpp"
#include "prism/sim/lean_goal.hpp"

namespace prism::sim {
namespace fs = std::filesystem;
namespace {

struct item { std::string name; std::string lean; };  // lean empty: an SMT-LIB file at `name`

void collect(const fs::path& p, std::vector<item>& out) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        std::vector<fs::path> files;
        for (auto& e : fs::recursive_directory_iterator(p, ec))
            if (e.is_regular_file() && e.path().extension() == ".smt2") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) out.push_back({f.generic_string(), {}});
    } else if (p.extension() == ".goals") {  // id <TAB> Lean statement
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) {
            const auto t = line.find('\t');
            if (t != std::string::npos) out.push_back({line.substr(0, t), line.substr(t + 1)});
        }
    } else {
        out.push_back({p.generic_string(), {}});
    }
}

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int usage() {
    return fail_usage("sim",
                      "usage: prism sim index PATH... [-o FILE] [--labels TSV] [--threads N] [--alpha A] | prism sim query FILE.smt2 "
                      "[-i FILE] [-k N] [--exact] [--ted] | prism sim hint FILE.smt2 [-i FILE] [-k N]",
                      "(docs/SIM_SEARCH.md)");
}

}  // namespace

int sim_main(int argc, char** argv) {
    if (argc < 1) return usage();
    const std::string sub = argv[0];
    std::vector<std::string> pos;
    std::string out = "prism-sim.idx", labels;
    std::size_t k = 10, threads = std::max(1u, std::thread::hardware_concurrency());
    float alpha = 0.5f;
    bool exact = false, ted = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "-o" || a == "-i" || a == "--index") { const char* v = val(); if (!v) return usage(); out = v; }
        else if (a == "--labels") { const char* v = val(); if (!v) return usage(); labels = v; }
        else if (a == "-k") { const char* v = val(); if (!v) return usage(); k = static_cast<std::size_t>(std::atoi(v)); }
        else if (a == "--threads") { const char* v = val(); if (!v) return usage(); threads = std::max<std::size_t>(1, static_cast<std::size_t>(std::atoi(v))); }
        else if (a == "--alpha") { const char* v = val(); if (!v) return usage(); alpha = static_cast<float>(std::atof(v)); }
        else if (a == "--exact") exact = true;
        else if (a == "--ted") ted = true;
        else if (!a.empty() && a[0] == '-') return usage();
        else pos.push_back(a);
    }

    if (sub == "index") {
        if (pos.empty()) return usage();
        std::vector<item> items;
        for (const auto& p : pos) collect(p, items);
        std::map<std::string, std::pair<std::string, double>> lab;  // name or basename -> strategy, seconds
        if (!labels.empty()) {
            std::ifstream in(labels);
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                std::string n, s, t;
                std::getline(ls, n, '\t');
                std::getline(ls, s, '\t');
                std::getline(ls, t, '\t');
                if (!n.empty() && !s.empty()) lab[n] = {s, t.empty() ? std::nan("") : std::atof(t.c_str())};
            }
        }
        const double t0 = now_s();
        std::vector<std::optional<sym_vec>> vecs(items.size());
        std::vector<std::string> why(items.size());
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> th;
        for (std::size_t w = 0; w < std::min(threads, items.size()); ++w)
            th.emplace_back([&] {
                for (std::size_t i; (i = next.fetch_add(1)) < items.size();) {
                    if (items[i].lean.empty()) vecs[i] = embed_smt2_file(items[i].name, &why[i]);
                    else if (auto t = lean_goal_to_term(items[i].lean, &why[i])) vecs[i] = embed(*t);
                }
            });
        for (auto& t : th) t.join();
        const double t1 = now_s();
        sym_index ix(symbolic_metric{alpha});
        std::size_t skipped = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!vecs[i]) { ++skipped; continue; }
            std::string label;
            double sec = std::nan("");
            auto it = lab.find(items[i].name);
            if (it == lab.end()) it = lab.find(fs::path(items[i].name).filename().string());
            if (it != lab.end()) { label = it->second.first; sec = it->second.second; }
            ix.add(items[i].name, std::move(*vecs[i]), label, sec);
        }
        ix.build();
        const double t2 = now_s();
        std::string err;
        if (!ix.save(out, &err)) return fail("sim", err, Exit::Error);
        std::printf("indexed %zu of %zu inputs (%zu skipped: not parsed or not translatable); embed %.2f s, hnsw build %.2f s -> %s\n", ix.size(),
                    items.size(), skipped, t1 - t0, t2 - t1, out.c_str());
        return code(Exit::Ok);
    }

    if (sub == "query" || sub == "hint") {
        if (pos.size() != 1) return usage();
        std::string err;
        auto ix = sym_index::load(out, &err);
        if (!ix) return fail("sim", err, Exit::Error);
        auto qt = from_smt2_file(pos[0], &err);
        if (!qt) return fail("sim", "query: " + err, Exit::Error);
        const auto qc = canonicalise(*qt);
        const auto qv = embed(qc);
        if (sub == "hint") {
            hint_params hp;
            hp.k = k;
            const auto hs = strategy_hint(*ix, qv, hp);
            if (hs.empty()) std::printf("no labelled neighbours\n");
            for (const auto& h : hs) std::printf("%-24s score %.4f votes %zu mean_s %.4f\n", h.strategy.c_str(), h.score, h.votes, h.mean_seconds);
            return code(Exit::Ok);
        }
        const double t0 = now_s();
        auto nn = exact ? ix->search_exact(qv, k) : ix->search(qv, k);
        const double t1 = now_s();
        if (ted)
            nn = rerank_ted(std::move(nn), qc, [](const entry& e) -> std::optional<term> {
                if (e.name.size() < 5 || e.name.substr(e.name.size() - 5) != ".smt2") return std::nullopt;
                return from_smt2_file(e.name);
            });
        for (std::size_t r = 0; r < nn.size(); ++r)
            std::printf("%2zu  d=%.4f  %s%s%s\n", r + 1, static_cast<double>(nn[r].dist), nn[r].e->name.c_str(), nn[r].e->label.empty() ? "" : "  [",
                        nn[r].e->label.empty() ? "" : (nn[r].e->label + "]").c_str());
        std::fprintf(stderr, "query %.3f ms over %zu entries\n", (t1 - t0) * 1e3, ix->size());
        return code(Exit::Ok);
    }
    return usage();
}

}  // namespace prism::sim
