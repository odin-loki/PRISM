#pragma once

// prism::sim index: forge::ann (flat exact + HNSW) over sym_vec with
// symbolic_metric, labelled entries, a file format, an optional bounded
// tree-edit-distance rerank, and the strategy-hint API for the FastSMT port.
// Opt-in; nothing here changes a verdict, a solver default or the certified path.

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "prism/sim/symbolic_metric.hpp"

namespace forge::task { class pool; }

namespace prism::sim {

struct entry {
    std::uint64_t id = 0;
    std::string name;      // corpus path, goal id, ...
    std::string label;     // optional: the best tactic strategy for this formula (strategy hints)
    double seconds = std::numeric_limits<double>::quiet_NaN();  // time of that strategy, NaN if unknown
    sym_vec vec;
    // Seconds of every strategy tried on this formula (the table the regression hint needs).
    std::vector<std::pair<std::string, double>> times;
};

struct neighbour {
    std::uint64_t id = 0;
    float dist = 0;
    const entry* e = nullptr;
};

struct hnsw_config {
    std::size_t M = 16, ef_construction = 200, ef_search = 128;  // FORGE: recall@10 is only 0.86 at ef 64 on hard data
    std::uint64_t seed = 0x5eed;
};

class sym_index {
public:
    explicit sym_index(symbolic_metric m = {}, hnsw_config cfg = {});
    ~sym_index();
    sym_index(sym_index&&) noexcept;
    sym_index& operator=(sym_index&&) noexcept;

    // Ids are assigned in insertion order (0, 1, ...). Add everything, then build().
    std::uint64_t add(std::string name, sym_vec v, std::string label = {}, double seconds = std::numeric_limits<double>::quiet_NaN(),
                       std::vector<std::pair<std::string, double>> times = {});
    void build(forge::task::pool* pool = nullptr);  // HNSW; the exact index needs no build
    std::size_t size() const;
    const entry& at(std::uint64_t id) const;
    const symbolic_metric& metric() const;

    std::vector<neighbour> search(const sym_vec& q, std::size_t k, std::size_t ef = 0) const;                              // HNSW
    std::vector<neighbour> search_exact(const sym_vec& q, std::size_t k, forge::task::pool* pool = nullptr) const;         // brute force
    // The ANN candidates re-scored by the exact combined metric (what a metric
    // with a cheaper ANN-side approximation would do), then cut to k.
    std::vector<neighbour> search_rerank(const sym_vec& q, std::size_t k, std::size_t candidates = 0) const;

    // File format: SIM1, count, entries (name, label, seconds, vec). The HNSW
    // graph is rebuilt on load (deterministic for a fixed seed).
    bool save(const std::string& path, std::string* why = nullptr) const;
    static std::optional<sym_index> load(const std::string& path, std::string* why = nullptr, symbolic_metric m = {}, hnsw_config cfg = {});

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

// Bounded tree-edit-distance rerank of neighbours: `term_of(entry)` returns the
// canonical term of a candidate (empty: skipped, keeps its distance). The final
// distance is (1 - beta) * d + beta * ted for the top `top` candidates; the rest
// keep d. Costly (Zhang-Shasha, <= max_nodes per tree), so `top` stays small.
std::vector<neighbour> rerank_ted(std::vector<neighbour> cand, const term& query_canonical,
                                  const std::function<std::optional<term>(const entry&)>& term_of, std::size_t top = 20,
                                  float beta = 0.5f, std::size_t max_nodes = 160);

// ---- strategy hints (FastSMT port; API and tests only, not wired into the solver) ----
struct strategy_score {
    std::string strategy;
    double score = 0;         // regression: -predicted seconds; vote: similarity-weighted vote
    std::size_t votes = 0;    // neighbours that contributed
    double mean_seconds = std::numeric_limits<double>::quiet_NaN();  // vote: over its voters with a known time
    double predicted_seconds = std::numeric_limits<double>::quiet_NaN();  // regression: similarity-weighted mean over the neighbours
};
struct hint_params {
    enum class method {
        regression,  // default: kNN regression of each strategy's expected time over the neighbours' time tables
        vote,        // option: similarity-weighted vote of the neighbours' best strategies
    };
    method how = method::regression;
    std::size_t k = 15;
    double power = 2.0;    // weight = (1 - d)^power
    double min_sim = 0.0;  // neighbours with 1 - d below this do not contribute
    // Guard (regression): stay on `default_strategy` unless the best predicted strategy is at least `min_gain` faster
    // (0.2: 20%). Empty default_strategy: no guard.
    std::string default_strategy = "default";
    double min_gain = 0.2;
};
// Ranked best first. Regression needs entries labelled with a time table (sym_index::add(..., times)); when no
// neighbour has one it falls back to the vote. The vote lost badly to plain Z3 in the measurement (docs/SIM_SEARCH.md):
// a vote of winners ignores how much a wrong strategy costs. Never used to decide an answer, only an order.
std::vector<strategy_score> strategy_hint(const sym_index& idx, const sym_vec& query, const hint_params& = {});

}  // namespace prism::sim
