"""End-to-end A/B of the learned scheduler on held-out VCs (roadmap 3.1 / 9.7).

The replay in sched.py assumes no contention and no process start-up; this
runs the real portfolio instead. For every held-out source file (the split of
sched.py) the pir VCs are written with ``prism --pir-vcs`` (up to 6 per
function, unwind 8) and each VC is answered by ``prism --solve-smt2`` four
times, interleaved so that a change in machine load hits every pass alike:

  rules    PRISM_SOLVER_PREDICT=0, a fresh history per VC (no history)
  model    PRISM_SOLVER_MODEL=<GBDT trained on the training files, enabled>
  history  PRISM_SOLVER_PREDICT=0, one history across the run
  rules2   rules again: the run-to-run noise

Solver wall time (process start excluded) is summed; a timeout or a stall
(no answer within 120 s) counts as the timeout. Answers are compared across
passes. Output: <out>/e2e.json; ``predict.py --end-to-end`` adds its summary
to the model's metrics.

    python tools/prism_ai/sched_e2e.py --prism build/prism --solve-log DIR/solve_runs.jsonl --out e2e
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.prism_ai import predict, sched  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
PASSES = ["rules", "model", "history", "rules2"]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--prism", required=True)
    ap.add_argument("--solve-log", type=Path, action="append", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--trees", type=int, default=60)
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    train, test = sched.split(sched.load(a.solve_log))
    g = sched.GbdtModel(train, censored=False, n_trees=a.trees)
    model = {"schema": 1, "kind": "prism-gbdt", "enabled": True, "query_features": predict.QUERY_FEATURES,
             "function_features": predict.FUNCTION_FEATURES,
             "solvers": {m: x.to_json() for m, x in g.models.items()}}
    mp = a.out / "model.json"
    mp.write_text(json.dumps(model), encoding="utf-8")
    files = sorted({r["key"] for r in test})
    vcs: list[str] = []
    for f in files:
        d = a.out / "vcs" / f.replace("/", "__")
        r = subprocess.run([a.prism, "--pir-vcs", str(REPO / f), "--out", str(d), "--unwind", "8"],
                           capture_output=True, text=True, timeout=600)
        try:
            j = json.loads(r.stdout)
        except json.JSONDecodeError:
            continue
        for fn in j.get("functions", []):
            vcs += [vc["path"] for vc in fn.get("vcs", [])[:6]]
    res: dict[str, list[dict[str, Any]]] = {p: [] for p in PASSES}
    for i, vc in enumerate(vcs):
        for p in PASSES:
            env = dict(os.environ)
            env.pop("PRISM_SOLVER_MODEL", None)
            if p == "model":
                env["PRISM_SOLVER_MODEL"] = str(mp)
            else:
                env["PRISM_SOLVER_PREDICT"] = "0"
            hist = a.out / "hist-shared" if p == "history" else a.out / "hist" / p / str(i)
            try:
                r = subprocess.run([a.prism, "--solve-smt2", vc, "--timeout", str(a.timeout),
                                    "--solver-cache", str(hist)], capture_output=True, text=True,
                                   timeout=120, env=env)
                j = json.loads(r.stdout)
            except subprocess.TimeoutExpired:
                j = {"kind": "stall"}
            except json.JSONDecodeError:
                j = {"kind": "error"}
            res[p].append({"kind": j.get("kind"), "wall_s": j.get("wall_s", a.timeout),
                           "winner": j.get("winner", "")})
    summary: dict[str, Any] = {}
    for p in PASSES:
        ok = [x["kind"] in ("sat", "unsat") for x in res[p]]
        walls = [x["wall_s"] if o else a.timeout for x, o in zip(res[p], ok)]
        wins: dict[str, int] = {}
        for x in res[p]:
            wins[x["winner"]] = wins.get(x["winner"], 0) + 1
        s = sorted(walls)
        summary[p] = {"total_s": round(sum(walls), 3), "timeouts": ok.count(False),
                      "stalls": sum(x["kind"] == "stall" for x in res[p]),
                      "median_s": round(s[len(s) // 2], 4) if s else 0.0, "winners": wins}
    summary["disagreements"] = sum(
        1 for i in range(len(vcs)) if len({res[p][i]["kind"] for p in PASSES} & {"sat", "unsat"}) > 1)
    summary["vcs"] = len(vcs)
    summary["files"] = len(files)
    (a.out / "e2e.json").write_text(json.dumps({"summary": summary, "res": res, "vcs": vcs}, indent=1),
                                    encoding="utf-8")
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
