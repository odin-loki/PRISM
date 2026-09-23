#!/usr/bin/env python3
"""Portfolio vs Z3 alone on the pir VCs of the conformance suite (roadmap 3.1).

The roadmap 3.1 exit criterion: "the portfolio beats Z3 alone on total time
over the conformance suite's VCs". This tool measures exactly that:

1. `prism --pir-vcs FILE --out DIR` writes every verification condition of
   every encodable function of each conformance task (tests/conformance) as
   an SMT-LIB2 file (the same VCs the pir stage solves, docs/PIR.md);
2. each VC is answered by `prism --solve-smt2 VC` three times, back to back
   (so a change in machine load hits the three alike; running the passes one
   after another on a shared machine measured the load, not the solvers):
     z3         Z3 alone in-process (`--z3-only`),
     portfolio  the portfolio with no solve-time history (a fresh one per VC),
     history    the portfolio scheduling from the history it has built on the
                VCs before this one in the same run (what the pir stage does);
   the query cache is off in all three (timing, not answers);
3. the solver's own wall time (`wall_s`, process start-up excluded) is summed
   per pass; a timeout counts as the full timeout. Answers are compared: a
   VC where two passes give different definitive answers is a disagreement
   and is listed (it would be a solver bug).

Output: <out>/solver_bench.json and <out>/solver_bench.md. C++ engine only.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import conformance  # noqa: E402

PASSES = ("z3", "portfolio", "history")


def find_prism(arg: str | None) -> Path:
    for c in [arg, os.environ.get("PRISM_BIN"), str(REPO / "build" / "prism")]:
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            return Path(c).resolve()
    raise SystemExit("C++ prism binary not found: pass --prism or set PRISM_BIN")


def dump_vcs(prism: Path, src: Path, out: Path, unwind: int) -> dict[str, Any]:
    r = subprocess.run([str(prism), "--pir-vcs", str(src), "--out", str(out), "--unwind", str(unwind)],
                       capture_output=True, text=True, timeout=600, cwd=REPO)
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return {"file": str(src), "error": (r.stderr or r.stdout)[-400:]}


def solve(prism: Path, vc: str, mode: str, timeout: float, history: Path) -> dict[str, Any]:
    argv = [str(prism), "--solve-smt2", vc, "--timeout", str(timeout), "--solver-cache", str(history)]
    if mode == "z3":
        argv.append("--z3-only")
    t0 = time.monotonic()
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout * 3 + 30, cwd=REPO)
        res = json.loads(r.stdout)
    except (subprocess.TimeoutExpired, json.JSONDecodeError) as ex:
        res = {"kind": "error", "note": f"driver: {type(ex).__name__}"}
    res["process_s"] = round(time.monotonic() - t0, 4)
    return res


def charged(res: dict[str, Any], timeout: float) -> float:
    if res.get("kind") in ("sat", "unsat"):
        return float(res.get("wall_s", 0.0))
    return timeout


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prism", help="C++ prism binary (default: $PRISM_BIN, else build/prism)")
    ap.add_argument("--suite", action="append", type=Path,
                    help="task roots (default: tests/conformance/prism and tests/conformance/sv-comp)")
    ap.add_argument("--filter", help="regex on task ident")
    ap.add_argument("--unwind", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds per VC and pass")
    ap.add_argument("--jobs", "-j", type=int, default=1,
                    help="VCs solved at once (default 1: members of one query then own the cores)")
    ap.add_argument("--out", type=Path, default=Path("solver-bench-out"))
    ap.add_argument("--passes", default=",".join(PASSES),
                    help="comma list of passes to run (z3, portfolio, history)")
    args = ap.parse_args(argv)

    prism = find_prism(args.prism)
    roots = args.suite or [conformance.SUITE / "prism", conformance.SUITE / "sv-comp"]
    tasks = conformance.discover([r.resolve() for r in roots])
    if args.filter:
        rx = re.compile(args.filter)
        tasks = [t for t in tasks if rx.search(t.ident)]
    args.out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="prism-solver-bench-"))
    try:
        vcs: list[dict[str, Any]] = []
        errors: list[str] = []
        unencoded = 0
        for t in tasks:
            d = dump_vcs(prism, t.source, work / "vcs" / t.ident.replace("/", "__"), args.unwind)
            if "error" in d:
                errors.append(f"{t.ident}: {d['error']}")
                continue
            for fn in d.get("functions", []):
                if "status" in fn:
                    unencoded += 1
                for vc in fn.get("vcs", []):
                    vcs.append({"task": t.ident, "function": fn["function"], **vc})
        print(f"{len(tasks)} tasks, {len(vcs)} VCs ({unencoded} functions not encoded)", file=sys.stderr)
        passes = [p for p in PASSES if p in args.passes.split(",")]
        results: dict[str, list[dict[str, Any]]] = {p: [] for p in passes}

        def one(i: int) -> dict[str, dict[str, Any]]:
            out = {}
            for p in passes:
                hist = {"z3": work / "hist-z3", "portfolio": work / "hist-none" / str(i),
                        "history": work / "hist-online"}[p]
                out[p] = solve(prism, vcs[i]["path"], "z3" if p == "z3" else "portfolio", args.timeout, hist)
            return out

        with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
            for res in ex.map(one, range(len(vcs))):
                for p in passes:
                    results[p].append(res[p])
        for p in passes:
            print(f"pass {p}: {sum(charged(r, args.timeout) for r in results[p]):.2f} s", file=sys.stderr)

        rows = []
        disagreements = []
        for i, vc in enumerate(vcs):
            row = {k: vc[k] for k in ("task", "function", "kind", "prop", "line")}
            kinds = {}
            for p in passes:
                r = results[p][i]
                row[p] = {"kind": r.get("kind"), "wall_s": r.get("wall_s"), "winner": r.get("winner", ""),
                          "process_s": r.get("process_s")}
                kinds[p] = r.get("kind")
            definitive = {k for k in kinds.values() if k in ("sat", "unsat")}
            if len(definitive) > 1:
                disagreements.append(row)
            rows.append(row)

        summary: dict[str, Any] = {"tasks": len(tasks), "vcs": len(vcs), "unencoded_functions": unencoded,
                                   "timeout_s": args.timeout, "unwind": args.unwind, "errors": errors,
                                   "disagreements": len(disagreements), "passes": {}}
        for p in passes:
            rs = results[p]
            wins: dict[str, int] = {}
            for r in rs:
                if r.get("winner"):
                    wins[r["winner"]] = wins.get(r["winner"], 0) + 1
            summary["passes"][p] = {
                "total_s": round(sum(charged(r, args.timeout) for r in rs), 3),
                "process_total_s": round(sum(r.get("process_s", 0.0) for r in rs), 3),
                "solved": sum(1 for r in rs if r.get("kind") in ("sat", "unsat")),
                "sat": sum(1 for r in rs if r.get("kind") == "sat"),
                "unsat": sum(1 for r in rs if r.get("kind") == "unsat"),
                "timeouts": sum(1 for r in rs if r.get("kind") == "timeout"),
                "other": sum(1 for r in rs if r.get("kind") not in ("sat", "unsat", "timeout")),
                "max_s": round(max((charged(r, args.timeout) for r in rs), default=0.0), 3),
                "winners": wins,
            }
        slow = sorted(rows, key=lambda r: -max(r[p]["wall_s"] or 0.0 for p in passes))[:15]
        summary["slowest"] = slow
        (args.out / "solver_bench.json").write_text(
            json.dumps({"summary": summary, "rows": rows, "disagreements": disagreements}, indent=1),
            encoding="utf-8")
        md = ["# pir VCs: portfolio vs Z3 alone", "",
              f"- {summary['tasks']} conformance tasks, **{summary['vcs']} VCs** "
              f"({unencoded} functions not encoded), unwind {args.unwind}, timeout {args.timeout:g} s per VC",
              f"- disagreements between passes (must be 0): **{summary['disagreements']}**", "",
              "| pass | total solver s | total process s | solved | sat | unsat | timeouts | other | max s | winners |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
        for p in passes:
            s = summary["passes"][p]
            w = ", ".join(f"{k} {v}" for k, v in sorted(s["winners"].items(), key=lambda kv: -kv[1]))
            md.append(f"| {p} | {s['total_s']} | {s['process_total_s']} | {s['solved']} | {s['sat']} "
                      f"| {s['unsat']} | {s['timeouts']} | {s['other']} | {s['max_s']} | {w} |")
        md += ["", "Slowest VCs (solver seconds per pass):", "",
               "| task | function | VC | " + " | ".join(passes) + " |", "|---|---|---|" + "---:|" * len(passes)]
        for r in slow:
            md.append(f"| {r['task']} | {r['function']} | {r['prop']}@{r['line']} | "
                      + " | ".join(f"{r[p]['wall_s']} {r[p]['kind']} {r[p]['winner']}".strip() for p in passes)
                      + " |")
        if errors:
            md += ["", "Front-end errors:", ""] + [f"- {e}" for e in errors]
        (args.out / "solver_bench.md").write_text("\n".join(md) + "\n", encoding="utf-8")
        print("\n".join(md))
        return 1 if disagreements else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
