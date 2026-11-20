// prism::sim index over forge::ann.

#include "prism/sim/index.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

#include "prism/sim/ann.hpp"

namespace prism::sim {

struct sym_index::impl {
    symbolic_metric m;
    hnsw_config cfg;
    std::vector<entry> entries;
    forge::ann::flat_index<sym_vec, symbolic_metric> flat;
    forge::ann::hnsw<sym_vec, symbolic_metric> graph;
    bool built = false;
    impl(symbolic_metric mm, hnsw_config c)
        : m(mm), cfg(c), flat(mm),
          graph(forge::ann::hnsw_params{c.M, c.ef_construction, c.ef_search, c.seed}, mm) {}
};

sym_index::sym_index(symbolic_metric m, hnsw_config cfg) : p_(std::make_unique<impl>(m, cfg)) {}
sym_index::~sym_index() = default;
sym_index::sym_index(sym_index&&) noexcept = default;
sym_index& sym_index::operator=(sym_index&&) noexcept = default;

std::uint64_t sym_index::add(std::string name, sym_vec v, std::string label, double seconds, std::vector<std::pair<std::string, double>> times) {
    const std::uint64_t id = p_->entries.size();
    p_->flat.add(id, v);
    p_->graph.add(id, v);
    p_->entries.push_back({id, std::move(name), std::move(label), seconds, std::move(v), std::move(times)});
    p_->built = false;
    return id;
}
void sym_index::build(forge::task::pool* pool) {
    p_->graph.build(pool);
    p_->built = true;
}
std::size_t sym_index::size() const { return p_->entries.size(); }
const entry& sym_index::at(std::uint64_t id) const { return p_->entries.at(id); }
const symbolic_metric& sym_index::metric() const { return p_->m; }

namespace {
std::vector<neighbour> wrap(const sym_index& ix, const std::vector<forge::ann::hit>& hs) {
    std::vector<neighbour> out;
    out.reserve(hs.size());
    for (const auto& h : hs) out.push_back({h.id, h.dist, &ix.at(h.id)});
    return out;
}
}  // namespace

std::vector<neighbour> sym_index::search(const sym_vec& q, std::size_t k, std::size_t ef) const {
    return wrap(*this, p_->graph.search(q, k, ef));
}
std::vector<neighbour> sym_index::search_exact(const sym_vec& q, std::size_t k, forge::task::pool* pool) const {
    return wrap(*this, p_->flat.search(q, k, pool));
}
std::vector<neighbour> sym_index::search_rerank(const sym_vec& q, std::size_t k, std::size_t candidates) const {
    const auto cand = p_->graph.search(q, std::max(candidates, k), 0);
    const auto get = [this](std::uint64_t id) -> const sym_vec& { return p_->entries[id].vec; };
    return wrap(*this, forge::ann::rerank<sym_vec, symbolic_metric>(cand, q, get, p_->m, k));
}

bool sym_index::save(const std::string& path, std::string* why) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (why) *why = "cannot write " + path;
        return false;
    }
    auto w32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto wstr = [&](const std::string& s) { w32(static_cast<std::uint32_t>(s.size())); out.write(s.data(), static_cast<std::streamsize>(s.size())); };
    out.write("SIM2", 4);
    w32(static_cast<std::uint32_t>(p_->entries.size()));
    out.write(reinterpret_cast<const char*>(&p_->m.alpha), sizeof p_->m.alpha);
    for (const auto& e : p_->entries) {
        wstr(e.name);
        wstr(e.label);
        out.write(reinterpret_cast<const char*>(&e.seconds), sizeof e.seconds);
        w32(static_cast<std::uint32_t>(e.times.size()));
        for (const auto& [sn, st] : e.times) {
            wstr(sn);
            out.write(reinterpret_cast<const char*>(&st), sizeof st);
        }
        std::vector<std::byte> b;
        encode_sym_vec(e.vec, b);
        w32(static_cast<std::uint32_t>(b.size()));
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    if (!out) {
        if (why) *why = "write failed: " + path;
        return false;
    }
    return true;
}

std::optional<sym_index> sym_index::load(const std::string& path, std::string* why, symbolic_metric m, hnsw_config cfg) {
    std::ifstream in(path, std::ios::binary);
    auto fail = [&](const char* msg) -> std::optional<sym_index> {
        if (why) *why = std::string(msg) + ": " + path;
        return std::nullopt;
    };
    if (!in) return fail("cannot read");
    char magic[4];
    in.read(magic, 4);
    if (!in || std::memcmp(magic, "SIM2", 4) != 0) return fail("not a sim index");
    std::uint32_t n = 0;
    in.read(reinterpret_cast<char*>(&n), 4);
    float alpha = m.alpha;
    in.read(reinterpret_cast<char*>(&alpha), sizeof alpha);
    m.alpha = alpha;
    sym_index ix(m, cfg);
    auto rstr = [&](std::string& s) {
        std::uint32_t len = 0;
        in.read(reinterpret_cast<char*>(&len), 4);
        if (!in || len > (1u << 28)) return false;
        s.resize(len);
        in.read(s.data(), len);
        return static_cast<bool>(in);
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        std::string name, label;
        double sec = 0;
        std::uint32_t blen = 0;
        if (!rstr(name) || !rstr(label)) return fail("truncated");
        in.read(reinterpret_cast<char*>(&sec), sizeof sec);
        std::uint32_t nt = 0;
        in.read(reinterpret_cast<char*>(&nt), 4);
        if (!in || nt > (1u << 16)) return fail("truncated");
        std::vector<std::pair<std::string, double>> times(nt);
        for (auto& [sn, st] : times) {
            if (!rstr(sn)) return fail("truncated");
            in.read(reinterpret_cast<char*>(&st), sizeof st);
        }
        in.read(reinterpret_cast<char*>(&blen), 4);
        if (!in || blen > (1u << 28)) return fail("truncated");
        std::vector<std::byte> b(blen);
        in.read(reinterpret_cast<char*>(b.data()), blen);
        if (!in) return fail("truncated");
        ix.add(std::move(name), decode_sym_vec(b.data(), b.size()), std::move(label), sec, std::move(times));
    }
    ix.build();
    return ix;
}

std::vector<neighbour> rerank_ted(std::vector<neighbour> cand, const term& q,
                                  const std::function<std::optional<term>(const entry&)>& term_of, std::size_t top, float beta,
                                  std::size_t max_nodes) {
    for (std::size_t i = 0; i < cand.size() && i < top; ++i) {
        auto t = term_of(*cand[i].e);
        if (!t) continue;
        const auto c = t->canonical ? *t : canonicalise(*t);
        if (auto d = tree_edit_distance(q, c, max_nodes))
            cand[i].dist = (1.0f - beta) * cand[i].dist + beta * static_cast<float>(*d);
    }
    // Candidates past `top` keep their combined distance; the head is re-ordered among itself and the tail.
    std::stable_sort(cand.begin(), cand.end(), [](const neighbour& a, const neighbour& b) {
        return a.dist != b.dist ? a.dist < b.dist : a.id < b.id;
    });
    return cand;
}

namespace {
std::vector<strategy_score> vote_hint(const std::vector<neighbour>& nn, const hint_params& hp) {
    std::map<std::string, strategy_score> acc;
    std::map<std::string, std::size_t> timed;
    for (const auto& n : nn) {
        if (n.e->label.empty()) continue;
        const double sim = 1.0 - static_cast<double>(n.dist);
        if (sim < hp.min_sim) continue;
        auto& s = acc[n.e->label];
        s.strategy = n.e->label;
        s.score += std::pow(std::max(0.0, sim), hp.power) + 1e-9;
        ++s.votes;
        if (!std::isnan(n.e->seconds)) {
            s.mean_seconds = (std::isnan(s.mean_seconds) ? 0.0 : s.mean_seconds * static_cast<double>(timed[n.e->label])) + n.e->seconds;
            s.mean_seconds /= static_cast<double>(++timed[n.e->label]);
        }
    }
    std::vector<strategy_score> out;
    for (auto& [k, v] : acc) out.push_back(std::move(v));
    std::sort(out.begin(), out.end(), [](const strategy_score& a, const strategy_score& b) {
        if (a.score != b.score) return a.score > b.score;
        const bool an = std::isnan(a.mean_seconds), bn = std::isnan(b.mean_seconds);
        if (an != bn) return bn;
        if (!an && a.mean_seconds != b.mean_seconds) return a.mean_seconds < b.mean_seconds;
        return a.strategy < b.strategy;
    });
    return out;
}
}  // namespace

std::vector<strategy_score> strategy_hint(const sym_index& idx, const sym_vec& query, const hint_params& hp) {
    // exact search when the index is small enough that ANN buys nothing and exact is deterministic
    const auto nn = idx.size() <= 2000 ? idx.search_exact(query, hp.k) : idx.search(query, hp.k);
    if (hp.how == hint_params::method::vote) return vote_hint(nn, hp);
    // regression: weighted mean of seconds per strategy over the neighbours that have its time. The arithmetic mean (the
    // expected time) is deliberate: a geometric mean ignores a strategy that takes seconds on one neighbour in fifteen.
    std::map<std::string, double> num, den;
    std::map<std::string, std::size_t> cnt;
    for (const auto& n : nn) {
        const double sim = 1.0 - static_cast<double>(n.dist);
        if (sim < hp.min_sim) continue;
        const double w = std::pow(std::max(0.0, sim), hp.power) + 1e-9;
        for (const auto& [sn, st] : n.e->times) {
            num[sn] += w * st;
            den[sn] += w;
            ++cnt[sn];
        }
    }
    if (num.empty()) return vote_hint(nn, hp);
    std::vector<strategy_score> out;
    for (const auto& [sn, v] : num) {
        strategy_score s;
        s.strategy = sn;
        const double mean = v / den[sn];
        s.score = -mean;
        s.votes = cnt[sn];
        s.predicted_seconds = mean;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const strategy_score& a, const strategy_score& b) {
        return a.score != b.score ? a.score > b.score : a.strategy < b.strategy;
    });
    if (!hp.default_strategy.empty() && out.size() > 1 && out[0].strategy != hp.default_strategy) {
        auto it = std::find_if(out.begin(), out.end(), [&](const strategy_score& s) { return s.strategy == hp.default_strategy; });
        // stay on the default unless the best prediction is at least min_gain faster
        if (it != out.end() && !(-out[0].score <= (1.0 - hp.min_gain) * -it->score)) std::rotate(out.begin(), it, it + 1);
    }
    return out;
}

}  // namespace prism::sim
