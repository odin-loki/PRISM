"""Differential oracle: the old bmc encoder vs the new pir stage (roadmap 2.8).

Runs the C++ binary once over a tree with ``--stage inventory,classify,bmc,pir``
(or reads an existing report.json) and prints the per-function agreement
matrix plus every hard conflict: one engine says PROVED* and the other FAILED.
A wrong PROVED in either engine is a soundness bug (docs/PIR.md).

    python tools/pir_vs_bmc.py [TREE] [--bin build/prism] [--report report.json]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVED = {"PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING"}
ROWS = ["PROVED*", "BOUNDED", "FAILED", "NEEDS-HARNESS", "ERROR", "UNKNOWN", "absent"]


def bucket(status: str | None) -> str:
    if status is None:
        return "absent"
    if status in PROVED:
        return "PROVED*"
    if status in {"BOUNDED", "FAILED", "NEEDS-HARNESS", "ERROR"}:
        return status
    return "UNKNOWN"  # UNKNOWN / TIMEOUT / NOFUNC / NOTRUN


def per_function(report: dict, stage: str) -> dict[tuple[str, str], dict]:
    out: dict[tuple[str, str], dict] = {}
    for st in report.get("stages", []):
        if st.get("name") != stage:
            continue
        for f in st.get("findings", []):
            fn = f.get("function")
            if not fn:
                continue
            key = (f.get("file", ""), fn)
            # first finding per function is the verdict (both stages emit one)
            out.setdefault(key, f)
    return out


def run(tree: Path, exe: Path, out: Path, jobs: int) -> dict:
    cmd = [str(exe), str(tree), "--no-llm", "--stage", "inventory,classify,bmc,pir",
           "--out", str(out), "--jobs", str(jobs)]
    subprocess.run(cmd, cwd=ROOT, check=False, capture_output=True, text=True)
    return json.loads((out / "report.json").read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tree", nargs="?", default=str(ROOT / "testdata"))
    ap.add_argument("--bin", default=os.environ.get("PRISM_BIN", str(ROOT / "build" / "prism")))
    ap.add_argument("--report", help="reuse an existing report.json")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--json", help="write the matrix and conflicts as JSON")
    args = ap.parse_args(argv)
    if args.report:
        report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    else:
        with tempfile.TemporaryDirectory(prefix="prism_pir_vs_bmc_") as td:
            report = run(Path(args.tree), Path(args.bin), Path(td) / "out", args.jobs)
    bmc = per_function(report, "bmc")
    pir = per_function(report, "pir")
    keys = sorted(set(bmc) | set(pir))
    matrix: Counter[tuple[str, str]] = Counter()
    conflicts = []
    for k in keys:
        b = bmc.get(k)
        p = pir.get(k)
        rb = bucket(b["status"] if b else None)
        rp = bucket(p["status"] if p else None)
        matrix[(rb, rp)] += 1
        if {rb, rp} == {"PROVED*", "FAILED"}:
            conflicts.append({
                "file": k[0], "function": k[1],
                "bmc": b["status"] if b else None, "bmc_msg": (b or {}).get("message", ""),
                "bmc_cex": (b or {}).get("counterexample", ""),
                "pir": p["status"] if p else None, "pir_msg": (p or {}).get("message", ""),
                "pir_cex": (p or {}).get("counterexample", ""),
            })
    width = max(len(r) for r in ROWS) + 2
    print(f"functions: {len(keys)} (bmc {len(bmc)}, pir {len(pir)})")
    print("rows = bmc, columns = pir")
    print(" " * width + "".join(f"{c:>{width}}" for c in ROWS))
    for r in ROWS:
        print(f"{r:<{width}}" + "".join(f"{matrix.get((r, c), 0):>{width}}" for c in ROWS))
    agree = sum(n for (r, c), n in matrix.items() if r == c and r != "absent")
    both = sum(n for (r, c), n in matrix.items() if "absent" not in (r, c))
    print(f"same bucket where both report: {agree}/{both}")
    print(f"hard conflicts (PROVED* vs FAILED): {len(conflicts)}")
    for c in conflicts:
        print(f"  {c['file']}::{c['function']}: bmc={c['bmc']} [{c['bmc_msg']}] {c['bmc_cex']}"
              f" | pir={c['pir']} [{c['pir_msg']}] {c['pir_cex']}")
    if args.json:
        Path(args.json).write_text(json.dumps({
            "matrix": {f"{r}|{c}": n for (r, c), n in sorted(matrix.items())},
            "conflicts": conflicts,
        }, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
