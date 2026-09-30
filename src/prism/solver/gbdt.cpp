// GBDT trainer (roadmap 9.1). See include/prism/solver_gbdt.hpp. The
// evaluator of the built-in / loaded model is predict.cpp; both read the
// JSON schema documented in the header.

#include "prism/solver_gbdt.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>

namespace prism::solver::gbdt {
using ojson = nlohmann::ordered_json;

namespace {

// Left-to-right sums, so results do not depend on the platform's reduction order.
double sum(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x;
    return s;
}

double mean(const std::vector<double>& v) { return v.empty() ? 0.0 : sum(v) / static_cast<double>(v.size()); }

double sse(const std::vector<double>& v) {
    const double m = mean(v);
    double s = 0.0;
    for (double x : v) s += std::pow(x - m, 2.0);
    return s;
}

// phi(z) / (1 - Phi(z)), stable for large z.
double mills(double z) {
    if (z > 8.0) return z + 1.0 / z;
    const double tail = 0.5 * std::erfc(z / std::sqrt(2.0));
    return std::exp(-0.5 * z * z) / std::sqrt(2.0 * std::numbers::pi) / std::max(tail, 1e-300);
}

int fit_tree(Tree& tree, const std::vector<std::vector<double>>& xs, const std::vector<double>& ys,
             const std::vector<std::size_t>& idx, int depth, int min_leaf) {
    const int self = static_cast<int>(tree.nodes.size());
    tree.nodes.emplace_back();
    std::vector<double> vals;
    vals.reserve(idx.size());
    for (auto i : idx) vals.push_back(ys[i]);
    const std::size_t n = idx.size();
    if (depth == 0 || n < 2 * static_cast<std::size_t>(std::max(min_leaf, 0))) {
        tree.nodes[self].v = mean(vals);
        return self;
    }
    const double total_sum = sum(vals);
    double total_sq = 0.0;
    for (double v : vals) total_sq += v * v;
    const double parent = total_sq - total_sum * total_sum / static_cast<double>(n);
    const std::size_t nfeat = n ? xs[idx[0]].size() : 0;
    bool have = false;
    double best_gain = 0.0, best_t = 0.0;
    int best_f = -1;
    std::vector<std::size_t> best_order;
    std::size_t best_nl = 0;
    for (std::size_t f = 0; f < nfeat; ++f) {
        std::vector<std::size_t> order = idx;
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b) { return xs[a][f] < xs[b][f]; });
        double ls = 0.0, lsq = 0.0;
        for (std::size_t k = 0; k + 1 < n; ++k) {
            const double y = ys[order[k]];
            ls += y;
            lsq += y * y;
            const std::size_t nl = k + 1;
            if (nl < static_cast<std::size_t>(min_leaf) || n - nl < static_cast<std::size_t>(min_leaf)) continue;
            const double a = xs[order[k]][f], b = xs[order[k + 1]][f];
            if (a == b) continue;
            const double rs = total_sum - ls, rsq = total_sq - lsq;
            const auto nr = static_cast<double>(n - nl);
            const double s = (lsq - ls * ls / static_cast<double>(nl)) + (rsq - rs * rs / nr);
            const double gain = parent - s;
            if (gain > 1e-12 && (!have || gain > best_gain)) {
                have = true;
                best_gain = gain;
                best_f = static_cast<int>(f);
                best_t = (a + b) / 2.0;
                best_order = order;
                best_nl = nl;
            }
        }
    }
    if (!have) {
        tree.nodes[self].v = mean(vals);
        return self;
    }
    const std::vector<std::size_t> left(best_order.begin(), best_order.begin() + static_cast<std::ptrdiff_t>(best_nl));
    const std::vector<std::size_t> right(best_order.begin() + static_cast<std::ptrdiff_t>(best_nl), best_order.end());
    tree.nodes[self].f = best_f;
    tree.nodes[self].t = best_t;
    const int l = fit_tree(tree, xs, ys, left, depth - 1, min_leaf);
    const int r = fit_tree(tree, xs, ys, right, depth - 1, min_leaf);
    tree.nodes[self].l = l;
    tree.nodes[self].r = r;
    return self;
}

Tree fit_one(const std::vector<std::vector<double>>& xs, const std::vector<double>& resid,
             const std::vector<std::size_t>& idx, int depth, int min_leaf) {
    Tree t;
    fit_tree(t, xs, resid, idx, depth, min_leaf);
    return t;
}

ojson node_json(const Tree& t, int i) {
    const auto& n = t.nodes[static_cast<std::size_t>(i)];
    ojson j = ojson::object();
    if (n.f < 0) {
        j["v"] = n.v;
        return j;
    }
    j["f"] = n.f;
    j["t"] = n.t;
    j["l"] = node_json(t, n.l);
    j["r"] = node_json(t, n.r);
    return j;
}

int node_from_json(Tree& t, const ojson& j, int depth) {
    const int self = static_cast<int>(t.nodes.size());
    t.nodes.emplace_back();
    if (depth > 16 || !j.is_object()) return self;  // a leaf 0
    if (j.contains("v")) {
        if (j["v"].is_number()) t.nodes[self].v = j["v"].get<double>();
        return self;
    }
    if (!j.contains("l") || !j.contains("r")) return self;
    int f = j.contains("f") && j["f"].is_number_integer() ? j["f"].get<int>() : -2;
    double thr = j.contains("t") && j["t"].is_number() ? j["t"].get<double>() : 0.0;
    t.nodes[self].f = f < 0 ? -2 : f;  // -2: a split on a missing feature (reads 0)
    t.nodes[self].t = thr;
    const int l = node_from_json(t, j["l"], depth + 1);
    const int r = node_from_json(t, j["r"], depth + 1);
    t.nodes[self].l = l;
    t.nodes[self].r = r;
    return self;
}

}  // namespace

double Tree::eval(const std::vector<double>& x) const {
    if (nodes.empty()) return 0.0;
    std::size_t i = 0;
    while (true) {
        const auto& n = nodes[i];
        if (n.f == -1) return n.v;
        const double xv = (n.f >= 0 && static_cast<std::size_t>(n.f) < x.size()) ? x[static_cast<std::size_t>(n.f)] : 0.0;
        i = static_cast<std::size_t>(xv <= n.t ? n.l : n.r);
    }
}

int Tree::depth() const {
    std::function<int(int)> d = [&](int i) -> int {
        const auto& n = nodes[static_cast<std::size_t>(i)];
        return n.f == -1 ? 0 : 1 + std::max(d(n.l), d(n.r));
    };
    return nodes.empty() ? 0 : d(0);
}

ojson Tree::to_json() const { return nodes.empty() ? ojson{{"v", 0.0}} : node_json(*this, 0); }

Tree Tree::from_json(const ojson& node) {
    Tree t;
    node_from_json(t, node, 0);
    return t;
}

Model& Model::fit(const std::vector<std::vector<double>>& xs, const std::vector<double>& ys) {
    if (xs.empty()) throw std::invalid_argument("no training data");
    base = mean(ys);
    std::vector<double> pred(ys.size(), base);
    std::vector<std::size_t> idx(ys.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    trees.clear();
    std::vector<double> resid(ys.size());
    for (int it = 0; it < n_trees; ++it) {
        for (std::size_t i = 0; i < ys.size(); ++i) resid[i] = ys[i] - pred[i];
        if (sse(resid) < 1e-12) break;
        trees.push_back(fit_one(xs, resid, idx, depth, min_leaf));
        for (auto i : idx) pred[i] += lr * trees.back().eval(xs[i]);
    }
    return *this;
}

Model& Model::fit_censored(const std::vector<std::vector<double>>& xs, const std::vector<double>& ys,
                           const std::vector<bool>& censored, std::optional<double> sigma) {
    if (xs.empty()) throw std::invalid_argument("no training data");
    if (!sigma) {
        std::vector<double> obs;
        for (std::size_t i = 0; i < ys.size(); ++i)
            if (!censored[i]) obs.push_back(ys[i]);
        // Half the spread of the observed targets, clamped: a fixed noise
        // scale (the trees model the location).
        const double sd = obs.size() > 1 ? std::sqrt(sse(obs) / static_cast<double>(obs.size())) : 1.0;
        sigma = std::min(2.0, std::max(0.25, sd > 0 ? 0.5 * sd : 1.0));
    }
    const double sg = *sigma;
    base = mean(ys);
    std::vector<double> pred(ys.size(), base);
    std::vector<std::size_t> idx(ys.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    trees.clear();
    std::vector<double> resid(ys.size());
    for (int it = 0; it < n_trees; ++it) {
        bool small = true;
        for (std::size_t i = 0; i < ys.size(); ++i) {
            if (!censored[i]) resid[i] = ys[i] - pred[i];
            else resid[i] = sg * mills((ys[i] - pred[i]) / sg);
            if (!(std::abs(resid[i]) < 1e-9)) small = false;
        }
        if (sse(resid) < 1e-12 && small) break;
        trees.push_back(fit_one(xs, resid, idx, depth, min_leaf));
        for (auto i : idx) pred[i] += lr * trees.back().eval(xs[i]);
    }
    return *this;
}

double Model::predict(const std::vector<double>& x) const {
    double s = 0.0;
    for (const auto& t : trees) s += t.eval(x);
    return base + lr * s;
}

ojson Model::to_json() const {
    ojson j = ojson::object();
    j["base"] = base;
    j["lr"] = lr;
    j["trees"] = ojson::array();
    for (const auto& t : trees) j["trees"].push_back(t.to_json());
    return j;
}

Model Model::from_json(const ojson& j) {
    Model m;
    if (!j.is_object()) return m;
    if (j.contains("lr") && j["lr"].is_number()) m.lr = j["lr"].get<double>();
    if (j.contains("base") && j["base"].is_number()) m.base = j["base"].get<double>();
    if (j.contains("trees") && j["trees"].is_array())
        for (const auto& t : j["trees"]) m.trees.push_back(Tree::from_json(t));
    return m;
}

double rmse(const Model& m, const std::vector<std::vector<double>>& xs, const std::vector<double>& ys) {
    if (xs.empty()) return std::nan("");
    double s = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) s += std::pow(m.predict(xs[i]) - ys[i], 2.0);
    return std::sqrt(s / static_cast<double>(xs.size()));
}

}  // namespace prism::solver::gbdt
