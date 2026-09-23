"""Learned solver scheduling: replay, models, held-out evaluation (roadmap 3.1 / 9.3).

Input: ``solve_runs.jsonl`` from the collection doctest
(``PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*"``):
one line per verification condition with the time of EVERY portfolio member
run ALONE (``runs: {member: {kind, wall_s}}``). A member that timed out is a
censored time (>= the timeout); ``unknown``/``error`` is a member that gave
up without an answer (the ProbSAT walker on an unsat query, Bitwuzla on a
formula it cannot read); a missing member does not take that query.

Replay. ``simulate`` runs the portfolio scheduler of
src/prism/solver/portfolio.cpp on those alone-times: members sorted by their
expected time, a head start for the leader (or for in-process Z3 when there
is none), ``k`` slots (cores), the first definitive answer wins, the query
times out at ``T``. It assumes members on separate cores do not slow each
other (the real portfolio pays some contention, docs/SOLVERS.md), and that
CaDiCaL and Kissat each pay their own bit-blast (the real one shares it).

Policies (every one only orders members and sets the head start; the answer
is whatever the member that finishes first says, and the portfolio still
validates every SAT model in Z3):

* ``rules``      the default today: feature rules, Z3 alone for 0.15 s;
* ``history``    per-bucket mean times and winners (what the scheduler does
                 once ``solve_times.json`` has entries for the bucket);
* ``gbdt``       one GBDT per member on log(seconds), a timeout counted as T
                 (the model the C++ loader reads);
* ``aft``        one GBDT per member with a censored (Tobit / accelerated
                 failure time) loss: a timeout says only ">= T";
* ``knn``        k nearest training queries (standardised features), the
                 median of their log times (with one common censoring point
                 this is the Kaplan-Meier median);
* ``winner``     a classifier for "which member answers first"; used only to
                 pick the first member when there are fewer cores than
                 members, the rest in rule order.

Split: held out by SOURCE FILE (deterministic hash of the path); identical
VCs (same SMT-LIB2 hash) are kept once, in the file that sorts first, so no
query is in both halves.
"""

from __future__ import annotations

import hashlib
import json
import math
import random
from pathlib import Path
from typing import Any, Callable

from tools.prism_ai.gbdt import GBDT

MEMBERS = ["z3", "bitwuzla", "cadical", "kissat", "sls"]  # portfolio.cpp add order
Z3_FIRST_S = 0.15  # portfolio.cpp kZ3FirstS
EPS = 1e-3


# ------------------------------------------------------------------ data
def held_out(key: str, share: float = 0.3) -> bool:
    h = int(hashlib.sha256(key.encode()).hexdigest()[:8], 16)
    return (h % 1000) / 1000.0 < share


def load(paths: list[Path]) -> list[dict[str, Any]]:
    """Rows {key, sha, bucket, x, T, runs{member: (state, seconds)}}, one per distinct VC.

    state: "ans" (sat/unsat, seconds exact), "cens" (timed out: >= T),
    "gave" (no answer after `seconds`: unknown / error).
    """
    recs: list[dict[str, Any]] = []
    for p in paths:
        for line in p.read_text(encoding="utf-8").splitlines():
            try:
                recs.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    recs.sort(key=lambda r: (str(r.get("file", "")), str(r.get("function", "")), str(r.get("vc", ""))))
    seen: set[str] = set()
    rows = []
    for r in recs:
        runs = r.get("runs") or {}
        x = r.get("features")
        if not isinstance(x, list) or len(x) != 9 or "z3" not in runs:
            continue
        sha = str(r.get("sha") or "")
        if sha:
            if sha in seen:
                continue
            seen.add(sha)
        T = float(r.get("timeout_s", 8.0))
        out: dict[str, tuple[str, float]] = {}
        for m, v in runs.items():
            k, s = str(v.get("kind", "")), float(v.get("wall_s", 0.0))
            if k in ("sat", "unsat") and s < T:  # an answer after the timeout: a stall, counted as one
                out[m] = ("ans", s)
            elif k == "timeout" or s >= T:
                out[m] = ("cens", T)
            else:
                out[m] = ("gave", s)
        kinds = {v.get("kind") for v in runs.values() if v.get("kind") in ("sat", "unsat")}
        rows.append({"key": str(r.get("file", "")), "sha": sha, "bucket": str(r.get("bucket", "")),
                     "x": [float(v) for v in x], "T": T, "runs": out,
                     "answer": kinds.pop() if len(kinds) == 1 else ("disagree" if kinds else "none")})
    return rows


def split(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    return [r for r in rows if not held_out(r["key"])], [r for r in rows if held_out(r["key"])]


def log_target(state: str, s: float, T: float) -> tuple[float, bool]:
    """(log seconds, censored?) of one member on one query. A member that gave
    up never answers: for ordering it is as bad as a timeout."""
    if state == "ans":
        return math.log(s + EPS), False
    return math.log(T + EPS), True


# ------------------------------------------------------------------ replay
def sls_budget(T: float) -> float:
    return max(1.0, 0.1 * T)


def simulate(row: dict[str, Any], est: dict[str, float], lead: str | None, lead_delay: float, k: int,
             T: float | None = None) -> tuple[float, str]:
    """Replay one query: (wall seconds, winner or "" on timeout)."""
    T = row["T"] if T is None else T
    present = [m for m in MEMBERS if m in row["runs"]] + sorted(m for m in row["runs"] if m not in MEMBERS)
    order = sorted(present, key=lambda m: est.get(m, 1.5))  # stable, like std::stable_sort
    not_before = {m: 0.0 for m in order}
    if lead is not None and lead in not_before and len(order) > 1:
        for m in order:
            if m != lead:
                not_before[m] = lead_delay
    elif len(order) > 1 and "z3" in not_before:
        for m in order:
            if m != "z3":
                not_before[m] = min(Z3_FIRST_S, 0.1 * T)
    started: dict[str, float] = {}
    ends: dict[str, float] = {}   # member -> time its slot frees
    answer_at: dict[str, float] = {}
    t = 0.0
    while True:
        running = [m for m in started if ends[m] > t]
        for m in order:  # portfolio.cpp fill_slots
            if m in started or not_before[m] > t:
                continue
            if len(running) >= k:
                break
            started[m] = t
            state, s = row["runs"][m]
            if state == "ans":
                ends[m] = t + s
                answer_at[m] = t + s
            elif state == "cens":
                ends[m] = math.inf
            else:
                ends[m] = t + (max(s, sls_budget(T)) if m == "sls" else s)
            running.append(m)
        first = min(answer_at.items(), key=lambda kv: (kv[1], order.index(kv[0])), default=None)
        nxt = [e for e in ends.values() if e > t] + [nb for m, nb in not_before.items()
                                                     if m not in started and nb > t]
        nt = min(nxt, default=math.inf)
        if first is not None and first[1] <= nt:
            return (first[1], first[0]) if first[1] < T else (T, "")
        if nt >= T or nt == math.inf:
            return T, ""
        t = nt


# ------------------------------------------------------------------ models
def rule_estimate(member: str, x: list[float]) -> float:
    """portfolio.cpp rule_estimate, from the logged features."""
    width = 2 ** x[0] - 1
    nodes = 10 ** x[1] - 1
    arrays, fp, uf, arith, quant, muldiv = (x[3] > 0.5, x[4] > 0.5, x[5] > 0.5, x[6] > 0.5, x[7] > 0.5,
                                            x[8] > 0.5)
    wide, big = width > 32, nodes > 2000
    if member == "z3":
        return 0.5 if (arrays or uf or arith or quant) else 1.2 if (big or wide) else 0.6
    if member == "bitwuzla":
        return 0.4 if (fp or wide or muldiv or big) else 0.8
    if member == "cadical":
        return 2.0 if (muldiv and wide) else 0.9 if big else 1.1
    if member == "kissat":
        return 2.2 if (muldiv and wide) else 0.95 if big else 1.2
    if member == "sls":
        return 3.0
    return 1.5


class Model:
    name = "model"

    def seconds(self, member: str, x: list[float]) -> float | None:
        raise NotImplementedError


class GbdtModel(Model):
    """One GBDT per member on log(seconds + 1e-3); censored=True uses the Tobit loss."""

    def __init__(self, train: list[dict[str, Any]], censored: bool, n_trees: int = 60) -> None:
        self.name = "aft" if censored else "gbdt"
        self.models: dict[str, GBDT] = {}
        for m in MEMBERS:
            rs = [r for r in train if m in r["runs"]]
            if len(rs) < 10:
                continue
            ys, cs = zip(*(log_target(r["runs"][m][0], r["runs"][m][1], r["T"]) for r in rs))
            g = GBDT(n_trees=n_trees)
            if censored:
                g.fit_censored([r["x"] for r in rs], list(ys), list(cs))
            else:
                g.fit([r["x"] for r in rs], list(ys))
            self.models[m] = g

    def seconds(self, member: str, x: list[float]) -> float | None:
        g = self.models.get(member)
        return None if g is None else max(0.0, math.exp(g.predict(x)) - EPS)


class KnnModel(Model):
    name = "knn"

    def __init__(self, train: list[dict[str, Any]], k: int = 15) -> None:
        self.k = k
        n = max(1, len(train))
        dims = len(train[0]["x"]) if train else 9
        self.mu = [sum(r["x"][i] for r in train) / n for i in range(dims)]
        self.sd = [max(1e-6, math.sqrt(sum((r["x"][i] - self.mu[i]) ** 2 for r in train) / n)) for i in range(dims)]
        self.pts = [([(v - self.mu[i]) / self.sd[i] for i, v in enumerate(r["x"])],
                     {m: log_target(r["runs"][m][0], r["runs"][m][1], r["T"])[0] for m in r["runs"]}) for r in train]
        self._cache: dict[tuple[float, ...], list[int]] = {}

    def _nn(self, x: list[float]) -> list[int]:
        key = tuple(x)
        if key not in self._cache:
            z = [(v - self.mu[i]) / self.sd[i] for i, v in enumerate(x)]
            d = sorted(range(len(self.pts)),
                       key=lambda j: (sum((a - b) ** 2 for a, b in zip(z, self.pts[j][0])), j))
            self._cache[key] = d[: self.k]
        return self._cache[key]

    def seconds(self, member: str, x: list[float]) -> float | None:
        vals = sorted(self.pts[j][1][member] for j in self._nn(x) if member in self.pts[j][1])
        if not vals:
            return None
        return max(0.0, math.exp(vals[len(vals) // 2]) - EPS)

    def winner(self, x: list[float], row_members: list[str]) -> str | None:
        votes: dict[str, int] = {}
        for j in self._nn(x):
            t = self.pts[j][1]
            cand = [m for m in row_members if m in t]
            if cand:
                w = min(cand, key=lambda m: (t[m], MEMBERS.index(m) if m in MEMBERS else 99))
                votes[w] = votes.get(w, 0) + 1
        if not votes:
            return None
        return max(sorted(votes), key=lambda m: votes[m])


class History:
    """solve_times.json per bucket from the training rows: mean seconds per
    member (a timeout counts T) and wins (fastest alone)."""

    def __init__(self, train: list[dict[str, Any]]) -> None:
        self.b: dict[str, dict[str, list[float]]] = {}
        for r in train:
            hb = self.b.setdefault(r["bucket"], {})
            best = fastest(r)
            for m, (st, s) in r["runs"].items():
                e = hb.setdefault(m, [0.0, 0.0, 0.0])  # n, total, wins
                e[0] += 1
                e[1] += s if st == "ans" else r["T"]
                e[2] += m == best


def fastest(r: dict[str, Any]) -> str | None:
    ans = [(s, MEMBERS.index(m) if m in MEMBERS else 99, m) for m, (st, s) in r["runs"].items() if st == "ans"]
    return min(ans)[2] if ans else None


# ------------------------------------------------------------------ policies
Plan = tuple[dict[str, float], "str | None", float]


def plan_rules(r: dict[str, Any]) -> Plan:
    return {m: rule_estimate(m, r["x"]) for m in r["runs"]}, None, 0.0


def make_plan_history(h: History) -> Callable[[dict[str, Any]], Plan]:
    def plan(r: dict[str, Any]) -> Plan:
        est = {m: rule_estimate(m, r["x"]) for m in r["runs"]}
        hb = h.b.get(r["bucket"], {})
        lead = None
        for m in [m for m in MEMBERS if m in r["runs"]]:
            if m in hb and hb[m][0] >= 1:
                est[m] = hb[m][1] / hb[m][0]
                if hb[m][2] >= 1 and (lead is None or est[m] < est[lead]):
                    lead = m
        delay = min(3.0 * est[lead] + 0.2, 0.3 * r["T"]) if lead else 0.0
        return est, lead, delay
    return plan


def make_plan_model(model: Model, head_start: bool = True) -> Callable[[dict[str, Any]], Plan]:
    """The portfolio.cpp predict hook: predicted seconds replace the estimates;
    the fastest predicted member leads by min(3 x its time + 0.2, 30% T).
    head_start=False: the prediction only orders the members (Z3 first as today)."""
    def plan(r: dict[str, Any]) -> Plan:
        est = {m: rule_estimate(m, r["x"]) for m in r["runs"]}
        best = None
        for m in [m for m in MEMBERS if m in r["runs"]]:
            p = model.seconds(m, r["x"])
            if p is not None:
                est[m] = p
                if best is None or p < est[best]:
                    best = m
        if not head_start or best is None:
            return est, None, 0.0
        return est, best, min(3.0 * est[best] + 0.2, 0.3 * r["T"])
    return plan


def make_plan_winner(knn: KnnModel) -> Callable[[dict[str, Any]], Plan]:
    """Winner classifier: the predicted winner starts first (Z3's 0.15 s slot);
    the others in rule order."""
    def plan(r: dict[str, Any]) -> Plan:
        est = {m: rule_estimate(m, r["x"]) for m in r["runs"]}
        w = knn.winner(r["x"], list(r["runs"]))
        if w is None:
            return est, None, 0.0
        est[w] = -1.0
        return est, w, min(Z3_FIRST_S, 0.1 * r["T"])
    return plan


def plan_static_first(member: str) -> Callable[[dict[str, Any]], Plan]:
    """A diagnostic static rule: `member` (when it takes the query) gets the
    0.15 s head start Z3 gets today. Tells a learned gain from a simpler
    fixed rule."""
    def plan(r: dict[str, Any]) -> Plan:
        est = {m: rule_estimate(m, r["x"]) for m in r["runs"]}
        if member not in r["runs"]:
            return est, None, 0.0
        est[member] = -1.0
        return est, member, min(Z3_FIRST_S, 0.1 * r["T"])
    return plan


def evaluate(rows: list[dict[str, Any]], plan: Callable[[dict[str, Any]], Plan], k: int) -> dict[str, Any]:
    walls, first_ok = [], 0
    timeouts = 0
    for r in rows:
        est, lead, delay = plan(r)
        wall, win = simulate(r, est, lead, delay, k)
        walls.append(wall)
        timeouts += win == ""
        first = lead if lead is not None else min(r["runs"], key=lambda m: (est.get(m, 1.5),
                                                                         MEMBERS.index(m) if m in MEMBERS else 99))
        first_ok += first == fastest(r)
    s = sorted(walls)
    n = max(1, len(s))
    return {"total_s": round(sum(walls), 3), "timeouts": timeouts,
            "median_s": round(s[len(s) // 2], 4) if s else 0.0,
            "p90_s": round(s[min(len(s) - 1, int(0.9 * len(s)))], 4) if s else 0.0,
            "first_is_fastest": round(first_ok / n, 3), "walls": walls}


def bootstrap_delta(rows: list[dict[str, Any]], a: list[float], b: list[float], n: int = 1000,
                    seed: int = 7) -> tuple[float, float]:
    """95% interval of (sum b - sum a) / sum a, resampling SOURCE FILES."""
    by: dict[str, list[int]] = {}
    for i, r in enumerate(rows):
        by.setdefault(r["key"], []).append(i)
    files = sorted(by)
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        sa = sb = 0.0
        for _f in files:
            for i in by[files[rnd.randrange(len(files))]]:
                sa += a[i]
                sb += b[i]
        out.append((sb - sa) / sa if sa else 0.0)
    out.sort()
    return out[int(0.025 * n)], out[int(0.975 * n)]


def run_all(rows: list[dict[str, Any]], ks: list[int], n_trees: int = 60,
            repeat: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    train, test = split(rows)
    res: dict[str, Any] = {"queries": len(rows), "train": len(train), "test": len(test),
                           "train_files": len({r["key"] for r in train}), "test_files": len({r["key"] for r in test}),
                           "members": {m: sum(m in r["runs"] for r in rows) for m in MEMBERS}}
    if len(train) < 20 or len(test) < 10:
        res["note"] = "not enough data"
        return res
    gbdt = GbdtModel(train, censored=False, n_trees=n_trees)
    aft = GbdtModel(train, censored=True, n_trees=n_trees)
    knn = KnnModel(train)
    hist = History(train)
    plans = {"rules": plan_rules, "history": make_plan_history(hist),
             "gbdt": make_plan_model(gbdt), "aft": make_plan_model(aft), "knn": make_plan_model(knn),
             "gbdt-order": make_plan_model(gbdt, head_start=False), "aft-order": make_plan_model(aft, head_start=False),
             "knn-order": make_plan_model(knn, head_start=False), "winner": make_plan_winner(knn),
             "static-bitwuzla": plan_static_first("bitwuzla")}
    res["k"] = {}
    for k in ks:
        per = {name: evaluate(test, p, k) for name, p in plans.items()}
        oracle_walls = []
        for r in test:
            f = fastest(r)
            oracle_walls.append(r["runs"][f][1] if f else r["T"])
        base = per["rules"]["walls"]
        for name, v in per.items():
            if name != "rules":
                lo, hi = bootstrap_delta(test, base, v["walls"])
                v["delta_vs_rules_ci95"] = [round(lo, 4), round(hi, 4)]
        per["oracle"] = {"total_s": round(sum(oracle_walls), 3),
                         "timeouts": sum(1 for r in test if fastest(r) is None)}
        for v in per.values():
            v.pop("walls", None)
        res["k"][str(k)] = per
    if repeat:
        # Run-to-run noise: the same held-out queries timed twice, replayed
        # with the rule policy.
        rep = {r["sha"]: r for r in repeat if r["sha"]}
        common = [r for r in test if r["sha"] in rep]
        res["noise"] = {"queries": len(common)}
        for k in ks:
            a = evaluate(common, plan_rules, k)["total_s"]
            b = evaluate([rep[r["sha"]] for r in common], plan_rules, k)["total_s"]
            res["noise"][str(k)] = {"run1_s": a, "run2_s": b,
                                    "rel": round(abs(a - b) / max(a, 1e-9), 4)}
    res["_models"] = {"gbdt": gbdt, "aft": aft, "knn": knn}
    return res


def decide(res: dict[str, Any], ks: list[int], min_margin: float = 0.05) -> tuple[str | None, str]:
    """A model is enabled only if, at EVERY k, it beats both baselines by more
    than max(5%, run-to-run noise) in total time and has no more timeouts."""
    if "k" not in res:
        return None, "not enough data"
    noise = max([res.get("noise", {}).get(str(k), {}).get("rel", 0.0) for k in ks] + [0.0])
    margin = max(min_margin, noise)
    why = []
    for name in ("gbdt", "aft", "gbdt-order", "aft-order"):  # tree models: what the C++ loader evaluates
        ok = True
        for k in ks:
            per = res["k"][str(k)]
            base = min(per["rules"]["total_s"], per["history"]["total_s"])
            m = per[name]
            if not (m["total_s"] < (1 - margin) * base and
                    m["timeouts"] <= min(per["rules"]["timeouts"], per["history"]["timeouts"])):
                ok = False
                why.append(f"{name} k={k}: {m['total_s']} s / {m['timeouts']} timeouts vs baseline {base} s")
                break
        if ok:
            return name, f"beats the baselines by > {margin:.1%} at k in {ks}"
    return None, f"no model beats the baselines by > {margin:.1%}: " + "; ".join(why)


def markdown(res: dict[str, Any]) -> str:
    out = [f"{res['queries']} distinct VCs ({res['train']} train from {res.get('train_files')} files, "
           f"{res['test']} held out from {res.get('test_files')} files); members: "
           + ", ".join(f"{m} {n}" for m, n in res.get("members", {}).items()), ""]
    for k, per in res.get("k", {}).items():
        out.append(f"k = {k} cores")
        out.append("")
        out.append("| policy | total s | timeouts | median s | p90 s | first pick fastest | total vs rules (95% CI) |")
        out.append("|---|---:|---:|---:|---:|---:|---|")
        for name, v in per.items():
            ci = v.get("delta_vs_rules_ci95")
            cis = f"{ci[0] * 100:+.1f}% .. {ci[1] * 100:+.1f}%" if ci else ""
            out.append(f"| {name} | {v['total_s']} | {v['timeouts']} | {v.get('median_s', '')} | "
                       f"{v.get('p90_s', '')} | {v.get('first_is_fastest', '')} | {cis} |")
        out.append("")
    if "noise" in res:
        out.append(f"run-to-run noise (rules, {res['noise']['queries']} held-out VCs timed twice): "
                   + ", ".join(f"k={k}: {v['run1_s']} s vs {v['run2_s']} s ({v['rel'] * 100:.1f}%)"
                               for k, v in res["noise"].items() if k != "queries"))
        out.append("")
    return "\n".join(out) + "\n"
