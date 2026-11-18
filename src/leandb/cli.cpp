// `prism leandb`: import, search, verify, build-local (docs/LEANDB.md).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "prism/exit.hpp"
#include "prism/leandb.hpp"

namespace prism::leandb {

namespace {

int usage() {
    return fail_usage("leandb",
                      "usage: prism leandb import DB.sqlite [--pin P] [--committed DIR] [--local DIR] [--all DIR] [--sources a,b] [--threads N] | "
                      "search \"QUERY\"|--batch FILE [-k N] [--mode bm25|ann|sym|hybrid] [--threads N] [--db DIR]... | verify [--db DIR]... [--pin P] | "
                      "build-local DB.sqlite [--pin P] [--out DIR]",
                      "(docs/LEANDB.md)");
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Default location: $PRISM_LEANDB_DIR, else ./data/leandb (PIN.json, shards/, local/).
fs::path leandb_root() {
    if (const char* e = std::getenv("PRISM_LEANDB_DIR")) return e;
    return fs::current_path() / "data" / "leandb";
}

}  // namespace

int leandb_main(int argc, char** argv) {
    if (argc < 1) return usage();
    const std::string sub = argv[0];
    std::vector<std::string> pos;
    fs::path pin = leandb_root() / "PIN.json";
    fs::path committed, local, all, out;
    std::vector<fs::path> dbs;
    std::vector<std::string> sources;
    std::string mode_s = "hybrid", batch;
    std::size_t k = 10;
    unsigned threads = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--pin") { if (!(v = val())) return usage(); pin = v; }
        else if (a == "--committed") { if (!(v = val())) return usage(); committed = v; }
        else if (a == "--local") { if (!(v = val())) return usage(); local = v; }
        else if (a == "--all") { if (!(v = val())) return usage(); all = v; }
        else if (a == "--out") { if (!(v = val())) return usage(); out = v; }
        else if (a == "--db") { if (!(v = val())) return usage(); dbs.emplace_back(v); }
        else if (a == "--sources") { if (!(v = val())) return usage(); sources = split_csv(v); }
        else if (a == "--batch") { if (!(v = val())) return usage(); batch = v; }
        else if (a == "--mode") { if (!(v = val())) return usage(); mode_s = v; }
        else if (a == "-k") { if (!(v = val())) return usage(); k = static_cast<std::size_t>(std::atoi(v)); }
        else if (a == "--threads") { if (!(v = val())) return usage(); threads = static_cast<unsigned>(std::atoi(v)); }
        else if (!a.empty() && a[0] == '-') return usage();
        else pos.push_back(a);
    }
    if (dbs.empty()) {
        // committed shards, plus the local shards whenever they are present: no flag needed
        dbs.push_back(leandb_root() / "shards");
        dbs.push_back(leandb_root() / "local");
    }
    try {
        if (sub == "import" || sub == "build-local") {
            if (pos.size() != 1) return usage();
            import_options o;
            o.pin_file = pin;
            o.only_sources = sources;
            o.threads = threads;
            if (sub == "build-local") {
                o.out_local = out.empty() ? leandb_root() / "local" : out;
            } else {
                o.out_committed = committed;
                o.out_local = local;
                o.out_all = all;
                if (o.out_committed.empty() && o.out_local.empty() && o.out_all.empty()) return usage();
            }
            const auto r = import_db(pos[0], o);
            std::printf("imported %llu items (%llu searchable, %llu eval, %llu overlapping eval) in %.1f s\n", static_cast<unsigned long long>(r.items),
                        static_cast<unsigned long long>(r.searchable), static_cast<unsigned long long>(r.eval_items),
                        static_cast<unsigned long long>(r.eval_overlap), r.seconds);
            std::printf("goals translated for symbolic search: %llu of %llu\n", static_cast<unsigned long long>(r.translatable),
                        static_cast<unsigned long long>(r.goals_tried));
            std::printf("committed/all set: %llu shards, %llu bytes; local set: %llu shards, %llu bytes\n", static_cast<unsigned long long>(r.committed_shards),
                        static_cast<unsigned long long>(r.committed_bytes), static_cast<unsigned long long>(r.local_shards),
                        static_cast<unsigned long long>(r.local_bytes));
            return code(Exit::Ok);
        }
        if (sub == "search" && !batch.empty()) {
            // KEY <TAB> QUERY per line -> KEY <TAB> source:name <TAB> ... (the k best), for the premise-retrieval evaluation
            std::ifstream in(batch);
            search_options o;
            o.dirs = dbs;
            o.k = k;
            o.threads = threads;
            o.m = mode_s == "bm25" ? mode::bm25 : mode_s == "ann" ? mode::ann : mode_s == "sym" ? mode::sym : mode::hybrid;
            for (std::string line; std::getline(in, line);) {
                const auto t = line.find('\t');
                if (t == std::string::npos) continue;
                std::printf("%s", line.substr(0, t).c_str());
                for (const auto& r : search(line.substr(t + 1), o)) std::printf("\t%s:%s", r.source.c_str(), r.name.c_str());
                std::printf("\n");
            }
            return code(Exit::Ok);
        }
        if (sub == "search") {
            if (pos.size() != 1) return usage();
            search_options o;
            o.dirs = dbs;
            o.k = k;
            o.threads = threads;
            o.m = mode_s == "bm25" ? mode::bm25 : mode_s == "ann" ? mode::ann : mode_s == "sym" ? mode::sym : mode::hybrid;
            const auto t0 = std::chrono::steady_clock::now();
            const auto rs = search(pos[0], o);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            for (std::size_t i = 0; i < rs.size(); ++i) {
                std::string st = rs[i].statement.substr(0, 110);
                for (auto& ch : st) if (ch == '\n') ch = ' ';
                std::printf("%2zu  %.4f  %s%s  %s/%s  %s\n", i + 1, rs[i].score, rs[i].source.c_str(), rs[i].local ? " [local]" : "", rs[i].module.c_str(),
                            rs[i].name.c_str(), st.c_str());
            }
            std::fprintf(stderr, "%zu results in %.1f ms\n", rs.size(), ms);
            return code(Exit::Ok);
        }
        if (sub == "verify") {
            bool ok = true;
            for (const auto& d : dbs) {
                std::error_code ec;
                if (!fs::exists(d / "manifest.json", ec)) continue;
                const auto r = verify(d, pin, d.filename() == "local");
                std::printf("%s: %llu shards, %llu bytes: %s\n", d.string().c_str(), static_cast<unsigned long long>(r.shards),
                            static_cast<unsigned long long>(r.bytes), r.ok ? "OK" : "FAILED");
                for (const auto& p : r.problems) std::printf("  - %s\n", p.c_str());
                ok = ok && r.ok;
            }
            return code(ok ? Exit::Ok : Exit::Failed);
        }
    } catch (const std::exception& e) {
        return fail("leandb", e.what(), Exit::Error);
    }
    return usage();
}

}  // namespace prism::leandb
