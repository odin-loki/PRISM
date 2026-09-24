"""Solver and bound prediction: train, measure, export (roadmap 3.1 / 9.1 / 9.3 / 9.7).

Input logs (JSON lines):

* solver runs — ``solve_runs.jsonl`` from the collection doctest
  (``PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*"``,
  source directories listed in ``DIR/roots.txt``): one line per verification
  condition with the wall time of every portfolio member run ALONE
  (``runs: {z3: {kind, wall_s}, ...}``; a timeout is a censored time); or the
  production ``<cache>/solve_log.jsonl`` written by every portfolio solve
  (only members that finished have a time; not used as training targets).
* bound runs — ``bound_runs.jsonl``: per function the verdict and seconds at
  unwind 1, 2, 4, 8, 16.
* ``--repeat-*-log``: the held-out files collected a second time, for the
  run-to-run noise a model must beat.

Measurement is on a held-out split (by source file, deterministic hash):

* solver choice (quick metric): the member given the head start, and the
  seconds that member alone needs;
* solver replay (the gate, tools/prism_ai/sched.py): the portfolio scheduler
  replayed on the alone-times with k = 1, 2, 3, 5 cores for the rules, the
  per-bucket history, GBDT, censored (AFT) GBDT, k-NN and a winner
  classifier; total time, timeouts, median / p90 latency, bootstrap interval;
* bound: the unwind tried first; agreement with the verdict at unwind 16,
  total seconds, BOUNDED / no-verdict counts, time to first counterexample.

The exported model carries ``"enabled": true`` only when it beats the
baselines on the held-out split by more than max(5%, run-to-run noise) with
no more timeouts (roadmap 9.7); src/prism/solver/predict.cpp ignores a model
that is not enabled.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.prism_ai import sched  # noqa: E402
from tools.prism_ai.gbdt import GBDT  # noqa: E402

# Must equal src/prism/solver/predict.cpp (tests/test_ai_assist.py checks).
QUERY_FEATURES = ["log2_bv_width", "log10_nodes", "log10_consts", "arrays", "fp",
                  "uf", "arith", "quant", "bv_mul_div"]
FUNCTION_FEATURES = ["params", "log2_vars", "blocks", "log2_stmts", "checks", "back_edges",
                     "phis", "mul_div", "max_width", "log2_max_const"]
UNWINDS = [1, 2, 4, 8, 16]
DEFAULT_UNWIND = 8
EPS = 1e-3


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def held_out(key: str, share: float = 0.3) -> bool:
    h = int(hashlib.sha256(key.encode()).hexdigest()[:8], 16)
    return (h % 1000) / 1000.0 < share


# ------------------------------------------------------------------ solver data
def solver_rows(recs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Normalise both log formats to {key, bucket, x, times{member: s}}."""
    rows = []
    for r in recs:
        x = r.get("features")
        if not isinstance(x, list) or len(x) != len(QUERY_FEATURES):
            continue
        times: dict[str, float] = {}
        if "runs" in r:  # collection: every member ran alone
            for m, v in r["runs"].items():
                times[m] = float(v.get("wall_s", 0.0))
        else:  # production portfolio log: finished members only
            for m, s in (r.get("times") or {}).items():
                times[m] = float(s)
        if not times:
            continue
        key = str(r.get("file") or r.get("hash") or "")
        rows.append({"key": key, "bucket": r.get("bucket", ""), "x": [float(v) for v in x], "times": times})
    return rows


# portfolio.cpp rule_estimate (one copy, in sched.py).
rule_estimate = sched.rule_estimate


def measure_solver(rows: list[dict[str, Any]], n_trees: int = 60) -> dict[str, Any]:
    members = sorted({m for r in rows for m in r["times"]})
    full = [r for r in rows if all(m in r["times"] for m in members)]
    train = [r for r in full if not held_out(r["key"])]
    test = [r for r in full if held_out(r["key"])]
    res: dict[str, Any] = {"members": members, "queries": len(full), "train": len(train), "test": len(test)}
    if len(train) < 10 or len(test) < 5 or len(members) < 2:
        res["note"] = "not enough complete solver runs to measure"
        res["enabled"] = False
        res["models"] = {}
        return res
    models = {}
    for m in members:
        models[m] = GBDT(n_trees=n_trees).fit([r["x"] for r in train],
                                              [math.log(r["times"][m] + EPS) for r in train])
    # history baseline: per-bucket mean seconds per member (the current scheduler)
    hist: dict[str, dict[str, list[float]]] = {}
    for r in train:
        for m, s in r["times"].items():
            hist.setdefault(r["bucket"], {}).setdefault(m, []).append(s)

    def pick_rules(r: dict[str, Any]) -> str:
        return min(members, key=lambda m: rule_estimate(m, r["x"]))

    def pick_hist(r: dict[str, Any]) -> str:
        hb = hist.get(r["bucket"])
        if not hb:
            return pick_rules(r)
        return min(members, key=lambda m: sum(hb[m]) / len(hb[m]) if m in hb else rule_estimate(m, r["x"]))

    def pick_model(r: dict[str, Any]) -> str:
        return min(members, key=lambda m: models[m].predict(r["x"]))

    def pick_oracle(r: dict[str, Any]) -> str:
        return min(members, key=lambda m: r["times"][m])

    pol: dict[str, Any] = {}
    for name, pick in (("rules", pick_rules), ("history", pick_hist), ("gbdt", pick_model), ("oracle", pick_oracle)):
        total = acc = 0.0
        for r in test:
            m = pick(r)
            total += r["times"][m]
            acc += m == pick_oracle(r)
        pol[name] = {"seconds": round(total, 3), "accuracy": round(acc / len(test), 3)}
    res["policies"] = pol
    base = min(pol["rules"]["seconds"], pol["history"]["seconds"])
    res["baseline"] = "history" if pol["history"]["seconds"] <= pol["rules"]["seconds"] else "rules"
    # Beat the better baseline by at least 5% of its time on the held-out split.
    res["enabled"] = pol["gbdt"]["seconds"] < 0.95 * base
    res["models"] = {m: g.to_json() for m, g in models.items()}
    return res


# ------------------------------------------------------------------ bound data
def bound_rows(recs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for r in recs:
        x = r.get("features")
        runs = {int(u["unwind"]): u for u in r.get("runs", [])}
        if not isinstance(x, list) or len(x) != len(FUNCTION_FEATURES) or any(u not in runs for u in UNWINDS):
            continue
        final = runs[UNWINDS[-1]]["status"]
        if final in ("ERROR", "UNKNOWN", "TIMEOUT"):
            continue
        # Label: the smallest unwind from which the verdict equals the one at 16.
        # A BOUNDED verdict is never "reached early": bounded depth is its
        # assurance, so its label is the largest bound.
        label = UNWINDS[-1]
        if final != "BOUNDED":
            for u in UNWINDS:
                if all(runs[v]["status"] == final for v in UNWINDS if v >= u):
                    label = u
                    break
        rows.append({"key": str(r.get("file", "")), "function": r.get("function", ""),
                     "x": [float(v) for v in x], "runs": runs, "final": final, "label": label})
    return rows


NON_VERDICTS = ("ERROR", "UNKNOWN", "TIMEOUT")


def _policy(rows: list[dict[str, Any]], choose: Any) -> dict[str, Any]:
    agree = 0
    seconds = 0.0
    ttfc = 0.0
    n_failed = 0
    bounded = 0
    no_verdict = 0
    contradictions = 0
    for r in rows:
        u = choose(r)
        run = r["runs"][u]
        seconds += float(run["seconds"])
        agree += run["status"] == r["final"]
        bounded += run["status"] == "BOUNDED"
        no_verdict += run["status"] in NON_VERDICTS
        # A check on the data, not on the policy: a proof at the chosen depth
        # while unwind 16 finds a counterexample would be a pir bug (every
        # unwind's verdict must stand on its own).
        contradictions += str(run["status"]).startswith("PROVED") and r["final"] == "FAILED"
        if r["final"] == "FAILED":
            n_failed += 1
            # time to the first counterexample: try u, else escalate to 16
            ttfc += float(run["seconds"]) + (0.0 if run["status"] == "FAILED"
                                             else float(r["runs"][UNWINDS[-1]]["seconds"]))
    n = max(1, len(rows))
    return {"agreement": round(agree / n, 4), "seconds": round(seconds, 3),
            "time_to_first_cex": round(ttfc, 3), "failed_functions": n_failed,
            "bounded": bounded, "no_verdict": no_verdict, "contradictions": contradictions}


def snap(y: float) -> int:
    """Predicted log2 unwind -> the smallest listed unwind >= 2**y."""
    want = 2 ** max(0.0, y)
    for u in UNWINDS:
        if u >= want - 1e-9:
            return u
    return UNWINDS[-1]


def knn_bound(train: list[dict[str, Any]], k: int = 9) -> Any:
    """k nearest training functions (standardised features): the largest
    label among them (a larger bound is the safe side)."""
    n = max(1, len(train))
    dims = len(FUNCTION_FEATURES)
    mu = [sum(r["x"][i] for r in train) / n for i in range(dims)]
    sd = [max(1e-6, math.sqrt(sum((r["x"][i] - mu[i]) ** 2 for r in train) / n)) for i in range(dims)]
    pts = [([(v - mu[i]) / sd[i] for i, v in enumerate(r["x"])], r["label"]) for r in train]

    def choose(r: dict[str, Any]) -> int:
        z = [(v - mu[i]) / sd[i] for i, v in enumerate(r["x"])]
        near = sorted(range(len(pts)), key=lambda j: (sum((a - b) ** 2 for a, b in zip(z, pts[j][0])), j))[:k]
        return max(pts[j][1] for j in near)
    return choose


def measure_bound(rows: list[dict[str, Any]], n_trees: int = 60,
                  repeat: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    train = [r for r in rows if not held_out(r["key"])]
    test = [r for r in rows if held_out(r["key"])]
    res: dict[str, Any] = {"functions": len(rows), "train": len(train), "test": len(test)}
    if len(train) < 10 or len(test) < 5:
        res["note"] = "not enough bound runs to measure"
        res["enabled"] = False
        return res
    model = GBDT(n_trees=n_trees).fit([r["x"] for r in train], [math.log2(r["label"]) for r in train])
    res["policies"] = {
        f"fixed-{DEFAULT_UNWIND}": _policy(test, lambda r: DEFAULT_UNWIND),
        f"fixed-{UNWINDS[-1]}": _policy(test, lambda r: UNWINDS[-1]),
        "gbdt": _policy(test, lambda r: snap(model.predict(r["x"]))),
        "knn": _policy(test, knn_bound(train)),
        "oracle": _policy(test, lambda r: r["label"]),
    }
    if repeat:
        # Run-to-run noise: the same held-out functions timed twice at unwind 8.
        rep = {(r["key"], r["function"]): r for r in repeat}
        common = [r for r in test if (r["key"], r["function"]) in rep]
        a = _policy(common, lambda r: DEFAULT_UNWIND)["seconds"]
        b = _policy([rep[(r["key"], r["function"])] for r in common], lambda r: DEFAULT_UNWIND)["seconds"]
        res["noise"] = {"functions": len(common), "run1_s": a, "run2_s": b,
                        "rel": round(abs(a - b) / max(a, 1e-9), 4)}
    b, g = res["policies"][f"fixed-{DEFAULT_UNWIND}"], res["policies"]["gbdt"]
    margin = max(0.05, res.get("noise", {}).get("rel", 0.0))
    # Never trade verdicts for time: same or better agreement, no more BOUNDED
    # or no-verdict outcomes, AND less time by more than the noise.
    res["enabled"] = (g["agreement"] >= b["agreement"] and g["bounded"] <= b["bounded"] and
                      g["no_verdict"] <= b["no_verdict"] and g["seconds"] < (1 - margin) * b["seconds"])
    res["model"] = model.to_json()
    return res


def build_model(solver: dict[str, Any] | None, bound: dict[str, Any] | None,
                replay: dict[str, Any] | None = None) -> dict[str, Any]:
    """The model file. With a replay measurement (sched.run_all + decide) the
    solver part is enabled only when the replay chose a model; its trees are
    exported (the C++ loader evaluates GBDT / AFT trees alike)."""
    m: dict[str, Any] = {"schema": 1, "kind": "prism-gbdt",
                         "query_features": QUERY_FEATURES, "function_features": FUNCTION_FEATURES,
                         "note": "trained by tools/prism_ai/predict.py; enabled only when the held-out "
                                 "measurement beats the baseline (roadmap 9.7)"}
    enabled = False
    metrics: dict[str, Any] = {}
    if solver:
        metrics["solver"] = {k: v for k, v in solver.items() if k != "models"}
        if replay is None and solver.get("enabled") and solver.get("models"):
            m["solvers"] = solver["models"]
            enabled = True
    if replay is not None:
        metrics["replay"] = {k: v for k, v in replay.items() if not k.startswith("_")}
        chosen = replay.get("chosen")
        if chosen:
            mdl = replay["_models"][chosen.split("-")[0]]
            m["solvers"] = {name: g.to_json() for name, g in mdl.models.items()}
            m["head_start"] = not chosen.endswith("-order")
            enabled = True
    if bound:
        metrics["bound"] = {k: v for k, v in bound.items() if k != "model"}
        if bound.get("enabled") and bound.get("model"):
            m["bound"] = bound["model"]
            enabled = True
    m["enabled"] = enabled
    m["metrics"] = metrics
    return m


def markdown(model: dict[str, Any]) -> str:
    out = ["| part | policy | held-out result |", "|---|---|---|"]
    met = model.get("metrics", {})
    s = met.get("solver", {})
    for name, p in (s.get("policies") or {}).items():
        out.append(f"| solver choice ({s.get('test')} VCs) | {name} | {p['seconds']} s, "
                   f"picks the fastest {p['accuracy'] * 100:.1f}% |")
    b = met.get("bound", {})
    for name, p in (b.get("policies") or {}).items():
        out.append(f"| unwind ({b.get('test')} functions) | {name} | agreement {p['agreement'] * 100:.1f}%, "
                   f"{p['seconds']} s, first cex {p['time_to_first_cex']} s, BOUNDED {p.get('bounded', '')}, "
                   f"no verdict {p.get('no_verdict', '')} |")
    if "noise" in b:
        out.append(f"| unwind noise | fixed-{DEFAULT_UNWIND} timed twice ({b['noise']['functions']} functions) | "
                   f"{b['noise']['run1_s']} s vs {b['noise']['run2_s']} s |")
    out.append("")
    rp = met.get("replay")
    if rp:
        out.append(sched.markdown(rp))
        out.append(f"replay decision: {rp.get('chosen') or 'none'} ({rp.get('why', '')})")
        out.append("")
    out.append(f"solver model enabled: {bool(model.get('solvers'))}; bound model enabled: "
               f"{b.get('enabled', False)}; model file enabled: {model.get('enabled')}")
    return "\n".join(out) + "\n"


INC_PART = 12000  # characters per string literal (MSVC caps one literal at 16 KB)


def emit_inc(model: dict[str, Any]) -> str:
    """src/prism/solver/predict_default.inc: the model the C++ engine uses
    when no model file exists (compact JSON, split into literals)."""
    txt = json.dumps(model, separators=(",", ":"), sort_keys=True)
    assert ")JSON\"" not in txt
    parts = [txt[i:i + INC_PART] for i in range(0, len(txt), INC_PART)]
    body = ",\n".join(f'    R"JSON({p})JSON"' for p in parts)
    return ("// Generated by tools/prism_ai/predict.py --emit-inc; do not edit.\n"
            f"// {'enabled' if model.get('enabled') else 'DISABLED'} model, "
            f"{len(model.get('solvers', {}))} solver targets; metrics inside.\n"
            f"static const char* const kDefaultModelParts[] = {{\n{body}}};\n")


def parse_inc(text: str) -> dict[str, Any]:
    """The model JSON embedded in a predict_default.inc (tests)."""
    import re
    return dict(json.loads("".join(re.findall(r'R"JSON\((.*?)\)JSON"', text, flags=re.S))))


REPLAY_KS = [1, 2, 3, 5]   # cores: sequential, and the portfolio with k members at once
DECIDE_KS = [2, 3, 5]      # max_parallel is max(2, hardware threads)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--solve-log", type=Path, action="append", default=[],
                    help="solve_runs.jsonl or <cache>/solve_log.jsonl (repeatable)")
    ap.add_argument("--bound-log", type=Path, action="append", default=[], help="bound_runs.jsonl (repeatable)")
    ap.add_argument("--repeat-solve-log", type=Path, action="append", default=[],
                    help="the held-out files collected a second time (run-to-run noise)")
    ap.add_argument("--repeat-bound-log", type=Path, action="append", default=[])
    ap.add_argument("--out", type=Path, required=True, help="model JSON (predict_model.json)")
    ap.add_argument("--trees", type=int, default=60)
    ap.add_argument("--markdown", type=Path, help="write the measurement table here")
    ap.add_argument("--end-to-end", type=Path, action="append", default=[],
                    help="e2e.json of tools/prism_ai/sched_e2e.py (repeatable): recorded in the metrics")
    ap.add_argument("--emit-inc", type=Path,
                    help="also write the model as src/prism/solver/predict_default.inc (the built-in model)")
    a = ap.parse_args(argv)
    srows = [r for p in a.solve_log for r in solver_rows(load_jsonl(p))]
    brows = [r for p in a.bound_log for r in bound_rows(load_jsonl(p))]
    solver = measure_solver(srows, a.trees) if srows else None
    replay = None
    rrows = sched.load(a.solve_log) if a.solve_log else []
    # The replay needs every member timed alone (the collection format).
    if rrows and sum(len(r["runs"]) >= 2 for r in rrows) >= 30:
        rep = sched.load(a.repeat_solve_log) if a.repeat_solve_log else None
        replay = sched.run_all(rrows, REPLAY_KS, n_trees=a.trees, repeat=rep)
        replay["chosen"], replay["why"] = sched.decide(replay, DECIDE_KS)
    brep = [r for p in a.repeat_bound_log for r in bound_rows(load_jsonl(p))]
    bound = measure_bound(brows, a.trees, repeat=brep or None) if brows else None
    model = build_model(solver, bound, replay)
    if a.end_to_end:
        model["metrics"]["end_to_end"] = {
            f"run{i + 1}": json.loads(p.read_text(encoding="utf-8"))["summary"] for i, p in enumerate(a.end_to_end)}
    a.out.write_text(json.dumps(model, indent=1), encoding="utf-8")
    if a.emit_inc:
        a.emit_inc.write_text(emit_inc(model), encoding="utf-8")
    md = markdown(model)
    if a.markdown:
        a.markdown.write_text(md, encoding="utf-8")
    print(md, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
