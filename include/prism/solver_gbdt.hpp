#pragma once

// A tiny gradient-boosted regression model (roadmap 9.1): the trainer of the
// solver and bound prediction models that src/prism/solver/predict.cpp
// evaluates. Depth-limited regression trees (default depth 3) fitted to the
// residuals of squared loss, or of a censored (Tobit / accelerated failure
// time) loss, shrunk by a learning rate. Used offline by the prism_ai tool
// (src/tools/prism_ai/); nothing here runs inside the pipeline.
//
// JSON schema (the one the loader reads):
//
//     {"base": float, "lr": float, "trees": [node, ...]}
//     node = {"f": i, "t": thr, "l": node, "r": node} | {"v": value}
//
// A sample goes left when x[f] <= t. Training is deterministic: splits are
// searched over a stable sort of each feature, sums run left to right, so the
// same data gives the same trees on every run.

#include "prism/export.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace prism::solver::gbdt {

struct Node {
    int f = -1;  // -1: a leaf
    double t = 0.0;
    double v = 0.0;
    int l = -1, r = -1;  // child indices in Tree::nodes
};

// Nodes in creation order; the root is nodes[0].
struct Tree {
    std::vector<Node> nodes;
    PRISM_API double eval(const std::vector<double>& x) const;
    PRISM_API int depth() const;
    PRISM_API nlohmann::ordered_json to_json() const;
    // Lenient, like the loader: a malformed node is a leaf 0; a feature index
    // outside x reads 0; deeper than 16 levels reads 0.
    PRISM_API static Tree from_json(const nlohmann::ordered_json& node);
};

struct Model {
    int n_trees = 60;
    double lr = 0.1;
    int depth = 3;
    int min_leaf = 3;
    double base = 0.0;
    std::vector<Tree> trees;

    // Squared loss. Throws std::invalid_argument on empty data.
    PRISM_API Model& fit(const std::vector<std::vector<double>>& xs, const std::vector<double>& ys);
    // Censored boosting: for a censored row y is only a lower bound (the
    // solver timed out at y). Each tree fits sigma^2 x the negative gradient
    // of the censored normal log-likelihood: y - f for an observed row,
    // sigma * lambda(z), z = (y - f) / sigma, for a censored one (lambda: the
    // inverse Mills ratio), so a censored row pushes the prediction up only
    // while it is below y. sigma defaults to half the spread of the observed
    // targets, clamped to [0.25, 2].
    PRISM_API Model& fit_censored(const std::vector<std::vector<double>>& xs, const std::vector<double>& ys,
                                  const std::vector<bool>& censored, std::optional<double> sigma = std::nullopt);
    // base + lr * (sum of the trees).
    PRISM_API double predict(const std::vector<double>& x) const;
    PRISM_API nlohmann::ordered_json to_json() const;
    PRISM_API static Model from_json(const nlohmann::ordered_json& j);
};

PRISM_API double rmse(const Model& m, const std::vector<std::vector<double>>& xs, const std::vector<double>& ys);

}  // namespace prism::solver::gbdt
