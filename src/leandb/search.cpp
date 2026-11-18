// leandb search: parallel over shards through forge::db; BM25 with global statistics,
// hashed-text ANN (Hamming), symbolic ANN (prism::sim), reciprocal-rank fusion.

#include <algorithm>
#include <bit>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

#include <forge/ann/fusion.hpp>
#include <forge/db/sqlite.hpp>
#include <forge/task/algorithms.hpp>
#include <forge/task/pool.hpp>
#include <nlohmann/json.hpp>

#include "internal.hpp"
#include "prism/sim/lean_goal.hpp"
#include "prism/sim/symbolic_metric.hpp"

namespace prism::leandb {
namespace db = forge::db;
using json = nlohmann::json;

namespace {

struct shard_ref {
    fs::path path;
    bool local = false;
    std::uint64_t n_indexed = 0, n_tokens = 0;
};

struct hit {
    std::int64_t id = 0;
    double score = 0;  // larger is better
    std::uint32_t shard = 0;
};
bool hit_before(const hit& a, const hit& b) { return a.score != b.score ? a.score > b.score : a.id < b.id; }

db::connection open_shard(const fs::path& p) {
    db::open_options o;
    o.immutable = true;
    o.create = false;
    return db::connection::open(p.string(), o);
}

std::vector<shard_ref> collect_shards(const search_options& opt) {
    std::vector<shard_ref> out;
    for (const auto& d : opt.dirs) {
        std::error_code ec;
        const auto mf = d / "manifest.json";
        if (!fs::exists(mf, ec)) continue;
        std::ifstream in(mf, std::ios::binary);
        json j;
        try { in >> j; } catch (...) { continue; }
        const bool local = j.value("class", "") == "local";
        for (const auto& s : j.value("shards", json::array())) {
            if (s.value("kind", "") != "search") continue;
            const fs::path p = d / s.value("file", "");
            if (!fs::exists(p, ec)) continue;
            out.push_back({p, local, s.value("n_indexed", std::uint64_t{0}), s.value("n_tokens", std::uint64_t{0})});
        }
    }
    return out;
}

std::string fts_query(const std::vector<std::string>& terms) {
    std::string q;
    for (const auto& t : terms) {
        if (!q.empty()) q += " OR ";
        q += "\"" + t + "\"";
    }
    return q;
}

std::vector<hit> bm25_search(const std::vector<shard_ref>& shards, const std::vector<std::string>& terms, std::size_t k, forge::task::pool* pool) {
    if (terms.empty() || shards.empty() || k == 0) return {};
    detail::bm25_stats st;
    st.df.assign(terms.size(), 0.0);
    double total_tokens = 0;
    for (const auto& s : shards) {
        st.n_docs += static_cast<double>(s.n_indexed);
        total_tokens += static_cast<double>(s.n_tokens);
    }
    st.avg_len = st.n_docs > 0 ? total_tokens / st.n_docs : 1.0;
    std::vector<std::vector<double>> dfs(shards.size(), std::vector<double>(terms.size(), 0.0));
    forge::task::parallel_blocks(pool, shards.size(), 1, [&](std::size_t i, std::size_t, std::size_t) {
        auto c = open_shard(shards[i].path);
        auto q = c.prepare("SELECT count(*) FROM fts WHERE fts MATCH ?1");
        for (std::size_t t = 0; t < terms.size(); ++t) {
            q.reset();
            q.bind(1, "\"" + terms[t] + "\"");
            if (q.step()) dfs[i][t] = static_cast<double>(q.get<std::int64_t>(0));
        }
    });
    for (const auto& d : dfs)
        for (std::size_t t = 0; t < terms.size(); ++t) st.df[t] += d[t];
    const std::string match = fts_query(terms);
    std::vector<std::vector<hit>> runs(shards.size());
    forge::task::parallel_blocks(pool, shards.size(), 1, [&](std::size_t i, std::size_t, std::size_t) {
        auto c = open_shard(shards[i].path);
        detail::register_bm25(c);
        detail::set_bm25_stats(&st);
        auto q = c.prepare("SELECT rowid, prism_bm25(fts, ?1, ?2) AS s FROM fts WHERE fts MATCH ?3 ORDER BY s, rowid LIMIT ?4");
        q.bind(1, detail::kWeightName).bind(2, detail::kWeightStatement).bind(3, match).bind(4, static_cast<std::int64_t>(k));
        for (auto& r : q.rows()) runs[i].push_back({r.get<std::int64_t>(0), -r.get<double>(1), static_cast<std::uint32_t>(i)});
        detail::set_bm25_stats(nullptr);
    });
    auto out = forge::task::merge_runs(pool, std::move(runs), hit_before);
    if (out.size() > k) out.resize(k);
    return out;
}

// Scan one blob column of the searchable items and keep the k nearest.
std::vector<hit> scan_blobs(const std::vector<shard_ref>& shards, std::size_t k, forge::task::pool* pool, const char* col,
                            const std::function<double(std::span<const std::byte>)>& dist) {
    std::vector<std::vector<hit>> runs(shards.size());
    forge::task::parallel_blocks(pool, shards.size(), 1, [&](std::size_t i, std::size_t, std::size_t) {
        auto c = open_shard(shards[i].path);
        auto q = c.prepare(std::string("SELECT id, ") + col + " FROM items WHERE searchable<>0 AND " + col + " IS NOT NULL");
        std::vector<hit> heap;
        for (auto& r : q.rows()) {
            const hit h{r.get<std::int64_t>(0), 1.0 - dist(r.get<std::span<const std::byte>>(1)), static_cast<std::uint32_t>(i)};
            if (heap.size() < k) { heap.push_back(h); std::push_heap(heap.begin(), heap.end(), hit_before); }
            else if (hit_before(h, heap.front())) { std::pop_heap(heap.begin(), heap.end(), hit_before); heap.back() = h; std::push_heap(heap.begin(), heap.end(), hit_before); }
        }
        std::sort_heap(heap.begin(), heap.end(), hit_before);
        runs[i] = std::move(heap);
    });
    auto out = forge::task::merge_runs(pool, std::move(runs), hit_before);
    if (out.size() > k) out.resize(k);
    return out;
}

std::vector<hit> ann_search(const std::vector<shard_ref>& shards, const std::string& query, std::size_t k, forge::task::pool* pool) {
    const auto q = detail::simhash(query);
    return scan_blobs(shards, k, pool, "simhash", [&](std::span<const std::byte> b) {
        if (b.size() != 32) return 1.0;
        std::uint64_t w[4];
        std::memcpy(w, b.data(), 32);
        int d = 0;
        for (int i = 0; i < 4; ++i) d += std::popcount(w[i] ^ q[static_cast<std::size_t>(i)]);
        return d / 256.0;
    });
}

std::optional<sim::sym_vec> query_sym(const std::string& query) {
    if (auto t = sim::lean_goal_to_term(query)) return sim::embed(*t);
    if (!query.empty() && query[0] == '(')
        if (auto v = sim::embed_smt2(query)) return v;
    return std::nullopt;
}

std::vector<hit> sym_search(const std::vector<shard_ref>& shards, const std::string& query, std::size_t k, forge::task::pool* pool) {
    const auto qv = query_sym(query);
    if (!qv) return {};
    const sim::symbolic_metric m{0.5f};
    return scan_blobs(shards, k, pool, "symvec", [&](std::span<const std::byte> b) {
        const auto v = sim::decode_sym_vec(b.data(), b.size());
        return static_cast<double>(m.distance(*qv, v));
    });
}

}  // namespace

std::vector<result> search(const std::string& query, const search_options& opt) {
    const auto shards = collect_shards(opt);
    if (shards.empty() || opt.k == 0) return {};
    forge::task::pool pool(opt.threads);
    auto terms = detail::words(query);
    {
        std::vector<std::string> uniq;
        for (auto& t : terms)
            if (std::find(uniq.begin(), uniq.end(), t) == uniq.end() && uniq.size() < 64) uniq.push_back(t);
        terms = std::move(uniq);
    }
    std::vector<hit> final_hits;
    const std::size_t depth = std::max<std::size_t>(50, opt.k * 5);
    switch (opt.m) {
        case mode::bm25: final_hits = bm25_search(shards, terms, opt.k, &pool); break;
        case mode::ann: final_hits = ann_search(shards, query, opt.k, &pool); break;
        case mode::sym: final_hits = sym_search(shards, query, opt.k, &pool); break;
        case mode::hybrid: {
            std::vector<std::vector<hit>> lists;
            lists.push_back(bm25_search(shards, terms, depth, &pool));
            lists.push_back(ann_search(shards, query, depth, &pool));
            auto sy = sym_search(shards, query, depth, &pool);
            if (!sy.empty()) lists.push_back(std::move(sy));
            std::vector<std::vector<std::uint64_t>> ids;
            std::map<std::int64_t, std::uint32_t> where;
            for (const auto& l : lists) {
                ids.emplace_back();
                for (const auto& h : l) {
                    ids.back().push_back(static_cast<std::uint64_t>(h.id));
                    where.emplace(h.id, h.shard);
                }
            }
            for (const auto& f : forge::ann::reciprocal_rank_fusion(ids, opt.k))
                final_hits.push_back({static_cast<std::int64_t>(f.id), f.score, where[static_cast<std::int64_t>(f.id)]});
            break;
        }
    }
    std::vector<result> out;
    // Details, one prepared statement per shard that holds a hit.
    std::map<std::uint32_t, db::connection> conns;
    for (const auto& h : final_hits) {
        if (!conns.count(h.shard)) conns.emplace(h.shard, open_shard(shards[h.shard].path));
        auto& c = conns.at(h.shard);
        auto q = c.prepare("SELECT s.name, i.tbl, i.kind, i.module, i.name, i.statement FROM items i JOIN srcs s ON s.id=i.src WHERE i.id=?1");
        q.bind(1, h.id);
        if (!q.step()) continue;
        result r;
        r.id = h.id;
        r.score = h.score;
        r.source = q.get<std::string>(0);
        r.tbl = detail::tbl_name(static_cast<int>(q.get<std::int64_t>(1)));
        r.kind = q.get<std::string>(2);
        r.module = q.get<std::string>(3);
        r.name = q.get<std::string>(4);
        r.statement = q.get<std::string>(5);
        r.local = shards[h.shard].local;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace prism::leandb
