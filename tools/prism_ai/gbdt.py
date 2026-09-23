"""A tiny gradient-boosted regression model in pure Python (roadmap 9.1).

Depth-limited regression trees (default depth 3) fitted to the residuals of
squared loss, shrunk by a learning rate. No dependencies. The exported JSON
is the format src/prism/solver/predict.cpp evaluates:

    {"base": float, "lr": float,
     "trees": [node, ...]}   node = {"f": i, "t": thr, "l": node, "r": node} | {"v": value}

A sample goes left when x[f] <= t (the same comparison as the C++ loader).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

Node = dict[str, Any]


def _mean(v: list[float]) -> float:
    return sum(v) / len(v) if v else 0.0


def _sse(v: list[float]) -> float:
    m = _mean(v)
    return sum((x - m) ** 2 for x in v)


def _mills(z: float) -> float:
    """phi(z) / (1 - Phi(z)), stable for large z."""
    if z > 8.0:
        return z + 1.0 / z
    tail = 0.5 * math.erfc(z / math.sqrt(2.0))
    return math.exp(-0.5 * z * z) / math.sqrt(2.0 * math.pi) / max(tail, 1e-300)


def _fit_tree(xs: list[list[float]], ys: list[float], idx: list[int], depth: int, min_leaf: int) -> Node:
    vals = [ys[i] for i in idx]
    if depth == 0 or len(idx) < 2 * min_leaf:
        return {"v": _mean(vals)}
    best: tuple[float, int, float, list[int], list[int]] | None = None
    total_sum = sum(vals)
    total_sq = sum(v * v for v in vals)
    n = len(idx)
    parent = total_sq - total_sum * total_sum / n
    nfeat = len(xs[idx[0]]) if idx else 0
    for f in range(nfeat):
        order = sorted(idx, key=lambda i: xs[i][f])
        ls = lsq = 0.0
        for k in range(n - 1):
            y = ys[order[k]]
            ls += y
            lsq += y * y
            nl = k + 1
            if nl < min_leaf or n - nl < min_leaf:
                continue
            a, b = xs[order[k]][f], xs[order[k + 1]][f]
            if a == b:
                continue
            rs, rsq, nr = total_sum - ls, total_sq - lsq, n - nl
            sse = (lsq - ls * ls / nl) + (rsq - rs * rs / nr)
            gain = parent - sse
            if gain > 1e-12 and (best is None or gain > best[0]):
                best = (gain, f, (a + b) / 2.0, order[:nl], order[nl:])
    if best is None:
        return {"v": _mean(vals)}
    _, f, t, left, right = best
    return {"f": f, "t": t,
            "l": _fit_tree(xs, ys, left, depth - 1, min_leaf),
            "r": _fit_tree(xs, ys, right, depth - 1, min_leaf)}


def eval_tree(node: Node, x: list[float]) -> float:
    while "v" not in node:
        node = node["l"] if x[node["f"]] <= node["t"] else node["r"]
    return float(node["v"])


@dataclass
class GBDT:
    n_trees: int = 60
    lr: float = 0.1
    depth: int = 3
    min_leaf: int = 3
    base: float = 0.0
    trees: list[Node] = field(default_factory=list)

    def fit(self, xs: list[list[float]], ys: list[float]) -> GBDT:
        if not xs:
            raise ValueError("no training data")
        self.base = _mean(ys)
        pred = [self.base] * len(ys)
        idx = list(range(len(ys)))
        self.trees = []
        for _ in range(self.n_trees):
            resid = [y - p for y, p in zip(ys, pred)]
            if _sse(resid) < 1e-12:
                break
            tree = _fit_tree(xs, resid, idx, self.depth, self.min_leaf)
            self.trees.append(tree)
            for i in idx:
                pred[i] += self.lr * eval_tree(tree, xs[i])
        return self

    def fit_censored(self, xs: list[list[float]], ys: list[float], censored: list[bool],
                     sigma: float | None = None) -> GBDT:
        """Tobit (accelerated failure time, normal errors) boosting: for a
        censored row y is only a lower bound (the solver timed out at y).
        Each tree fits sigma^2 x the negative gradient of the censored
        log-likelihood: y - f for an observed row, sigma * lambda(z) with
        z = (y - f) / sigma for a censored one (lambda: inverse Mills ratio),
        so a censored row pushes the prediction up only while it is below y.
        The prediction is the median log time; the JSON format is unchanged."""
        if not xs:
            raise ValueError("no training data")
        obs = [y for y, c in zip(ys, censored) if not c]
        if sigma is None:
            # Half the spread of the observed log times, clamped: a fixed
            # noise scale (the trees model the location).
            sd = math.sqrt(_sse(obs) / len(obs)) if len(obs) > 1 else 1.0
            sigma = min(2.0, max(0.25, 0.5 * sd if sd > 0 else 1.0))
        self.base = _mean(ys)
        pred = [self.base] * len(ys)
        idx = list(range(len(ys)))
        self.trees = []
        for _ in range(self.n_trees):
            resid = []
            for y, c, p in zip(ys, censored, pred):
                if not c:
                    resid.append(y - p)
                else:
                    z = (y - p) / sigma
                    resid.append(sigma * _mills(z))
            if _sse(resid) < 1e-12 and all(abs(r) < 1e-9 for r in resid):
                break
            tree = _fit_tree(xs, resid, idx, self.depth, self.min_leaf)
            self.trees.append(tree)
            for i in idx:
                pred[i] += self.lr * eval_tree(tree, xs[i])
        return self

    def predict(self, x: list[float]) -> float:
        return self.base + self.lr * sum(eval_tree(t, x) for t in self.trees)

    def to_json(self) -> dict[str, Any]:
        return {"base": self.base, "lr": self.lr, "trees": self.trees}

    @staticmethod
    def from_json(j: dict[str, Any]) -> GBDT:
        g = GBDT(lr=float(j.get("lr", 0.1)))
        g.base = float(j.get("base", 0.0))
        g.trees = list(j.get("trees", []))
        return g


def rmse(model: GBDT, xs: list[list[float]], ys: list[float]) -> float:
    if not xs:
        return math.nan
    return math.sqrt(sum((model.predict(x) - y) ** 2 for x, y in zip(xs, ys)) / len(xs))
