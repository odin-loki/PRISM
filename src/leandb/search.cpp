// leandb search over forge::db shard sets: exact global BM25 (db::shard_bm25_topk), per-shard HNSW
// (ann::shard_ann) for the hashed-text and symbolic vectors, reciprocal-rank fusion (ann::reciprocal_rank_fusion).
//
// Every present search directory (committed, local, a private complete copy) is merged into one shard_set by a small
// in-memory manifest whose shard files are absolute paths, so BM25 statistics and the vector search see all of them.

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include <forge/ann/fusion.hpp>
#include <forge/ann/shard_ann.hpp>
#include <forge/db/shards.hpp>
#include <forge/task/pool.hpp>

#include "internal.hpp"
#include "prism/sim/ann.hpp"
#include "prism/sim/lean_goal.hpp"
#include "prism/sim/symbolic_metric.hpp"

namespace prism::leandb {
namespace db = forge::db;

namespace {
using text_ann = forge::ann::shard_ann<std::vector<std::uint64_t>, forge::ann::hamming>;
using sym_ann = forge::ann::shard_ann<sim::sym_vec, sim::symbolic_metric>;

std::optional<sim::sym_vec> query_sym(const std::string& query) {
    if (auto t = sim::lean_goal_to_term(query)) return sim::embed(*t);
    if (!query.empty() && query[0] == '(')
        if (auto v = sim::embed_smt2(query)) return v;
    return std::nullopt;
}
}  // namespace

struct searcher::impl {
    search_options opt;
    forge::task::pool pool;
    fs::path tmp;
    std::unique_ptr<db::shard_set> set;
    std::vector<char> local;  // per shard of the merged set
    std::unique_ptr<text_ann> text;
    std::unique_ptr<sym_ann> sym;
    std::vector<std::size_t> text_shards, sym_shards;

    explicit impl(const search_options& o) : opt(o), pool(o.threads) {
        struct entry { db::shard_info info; bool local; };
        std::vector<entry> all;
        db::shard_manifest base;
        bool have = false;
        for (const auto& d : opt.dirs) {
            std::error_code ec;
            const fs::path sd = d / "search";
            if (!fs::exists(sd / "manifest.json", ec)) continue;
            std::ifstream in(sd / "manifest.json", std::ios::binary);
            std::string text_((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto m = db::shard_manifest::parse(text_);
            bool is_local = false;
            std::ifstream lj(d / "leandb.json", std::ios::binary);
            std::string ltxt((std::istreambuf_iterator<char>(lj)), std::istreambuf_iterator<char>());
            is_local = ltxt.find("\"class\": \"local\"") != std::string::npos;
            if (!have) { base = m; have = true; }
            for (auto info : m.shards) {
                info.file = fs::absolute(sd / info.file).string();
                all.push_back({std::move(info), is_local});
            }
        }
        if (all.empty()) return;
        std::sort(all.begin(), all.end(), [](const entry& a, const entry& b) { return a.info.min_key < b.info.min_key; });
        base.shards.clear();
        base.rows = base.bytes = 0;
        for (auto& e : all) {
            base.rows += e.info.rows;
            base.bytes += e.info.bytes;
            local.push_back(e.local ? 1 : 0);
            base.shards.push_back(e.info);
        }
        static std::atomic<unsigned> counter{0};
        tmp = fs::temp_directory_path() / ("prism-leandb-" + std::to_string(counter.fetch_add(1)) + "-" +
                                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tmp);
        std::ofstream(tmp / "manifest.json", std::ios::binary) << base.to_text();
        db::shard_open_options oo;
        oo.verify = db::verify_mode::size;  // `prism leandb verify` does the full hash check
        set = std::make_unique<db::shard_set>(db::open(tmp / "manifest.json", oo));
    }
    ~impl() {
        std::error_code ec;
        if (!tmp.empty()) fs::remove_all(tmp, ec);
    }

    // shards that have a given table
    std::vector<std::size_t> with_table(const char* table) {
        std::vector<char> has(set->size(), 0);
        db::shard_map<int>(*set, &pool, set->all_shards(), [&](db::connection& c, std::size_t i) { has[i] = c.table_exists(table) ? 1 : 0; return 0; }, db::run_options{false, db::shard_schedule::dynamic});
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < has.size(); ++i)
            if (has[i]) out.push_back(i);
        return out;
    }

    struct hit { std::int64_t id; double score; };

    std::vector<hit> bm25(const std::vector<std::string>& terms, std::size_t k) {
        db::bm25_options bo;
        bo.weights = {detail::kWeightName, detail::kWeightStatement};
        std::vector<hit> out;
        for (const auto& h : db::shard_bm25_topk(*set, &pool, "fts", terms, k, bo)) out.push_back({h.id, h.score});
        return out;
    }
    std::vector<hit> ann_text(const std::string& query, std::size_t k) {
        if (!text) {
            text = std::make_unique<text_ann>(*set, "ann_text", forge::ann::hamming{});
            text_shards = with_table("ann_text");
        }
        std::vector<hit> out;
        if (text_shards.empty()) return out;
        for (const auto& h : text->search(detail::simhash(query), k, &pool, detail::kEfSearch, &text_shards)) out.push_back({static_cast<std::int64_t>(h.id), 1.0 - h.dist / 256.0});
        return out;
    }
    std::vector<hit> ann_sym(const std::string& query, std::size_t k) {
        const auto qv = query_sym(query);
        if (!qv) return {};
        if (!sym) {
            sym = std::make_unique<sym_ann>(*set, "ann_sym", sim::symbolic_metric{0.5f});
            sym_shards = with_table("ann_sym");
        }
        std::vector<hit> out;
        if (sym_shards.empty()) return out;
        for (const auto& h : sym->search(*qv, k, &pool, detail::kEfSearch, &sym_shards)) out.push_back({static_cast<std::int64_t>(h.id), 1.0 - h.dist});
        return out;
    }

    // Details of the final ids, looked up by primary key in every shard (the vector searches return ids only).
    std::vector<result> details(const std::vector<hit>& hits) {
        std::vector<std::vector<std::optional<result>>> per = db::shard_map<std::vector<std::optional<result>>>(
            *set, &pool, set->all_shards(), [&](db::connection& c, std::size_t i) {
                std::vector<std::optional<result>> out(hits.size());
                auto q = c.prepare("SELECT s.name, i.tbl, i.kind, i.module, i.name, i.statement FROM items i JOIN srcs s ON s.id=i.src WHERE i.id=?1");
                for (std::size_t h = 0; h < hits.size(); ++h) {
                    q.reset();
                    q.bind(1, hits[h].id);
                    if (!q.step()) continue;
                    result r;
                    r.id = hits[h].id;
                    r.score = hits[h].score;
                    r.source = q.get<std::string>(0);
                    r.tbl = detail::tbl_name(static_cast<int>(q.get<std::int64_t>(1)));
                    r.kind = q.get<std::string>(2);
                    r.module = q.get<std::string>(3);
                    r.name = q.get<std::string>(4);
                    r.statement = q.get<std::string>(5);
                    r.local = local[i] != 0;
                    out[h] = std::move(r);
                }
                return out;
            }, db::run_options{false, db::shard_schedule::dynamic});
        std::vector<result> out;
        for (std::size_t h = 0; h < hits.size(); ++h)
            for (auto& p : per)
                if (p[h]) { out.push_back(std::move(*p[h])); break; }
        return out;
    }

    std::vector<result> run(const std::string& query) {
        if (!set || opt.k == 0) return {};
        auto terms = detail::words(query);
        {
            std::vector<std::string> uniq;
            for (auto& t : terms)
                if (std::find(uniq.begin(), uniq.end(), t) == uniq.end() && uniq.size() < 64) uniq.push_back(t);
            terms = std::move(uniq);
        }
        const std::size_t depth = std::max<std::size_t>(50, opt.k * 5);
        std::vector<hit> final_hits;
        switch (opt.m) {
            case mode::bm25: final_hits = bm25(terms, opt.k); break;
            case mode::ann: final_hits = ann_text(query, opt.k); break;
            case mode::sym: final_hits = ann_sym(query, opt.k); break;
            case mode::hybrid: {
                std::vector<std::vector<hit>> lists;
                lists.push_back(bm25(terms, depth));
                lists.push_back(ann_text(query, depth));
                auto sy = ann_sym(query, depth);
                if (!sy.empty()) lists.push_back(std::move(sy));
                std::vector<std::vector<std::uint64_t>> ids;
                for (const auto& l : lists) {
                    ids.emplace_back();
                    for (const auto& h : l) ids.back().push_back(static_cast<std::uint64_t>(h.id));
                }
                for (const auto& f : forge::ann::reciprocal_rank_fusion(ids, opt.k)) final_hits.push_back({static_cast<std::int64_t>(f.id), f.score});
                break;
            }
        }
        return details(final_hits);
    }
};

searcher::searcher(const search_options& opt) : p_(std::make_unique<impl>(opt)) {}
searcher::~searcher() = default;
std::vector<result> searcher::search(const std::string& query) { return p_->run(query); }

std::vector<result> search(const std::string& query, const search_options& opt) { return searcher(opt).search(query); }

}  // namespace prism::leandb
