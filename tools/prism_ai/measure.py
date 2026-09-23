"""Per-feature measurements for docs/AI.md (roadmap 9.7).

    python tools/prism_ai/measure.py triage OUT/            # clusters vs exact-key dedup
    python tools/prism_ai/measure.py ask OUT/ tests/data/ask_questions.jsonl --prism build/prism
    python tools/prism_ai/measure.py regress OUT/           # manifest of `prism regress --run`
    python tools/prism_ai/measure.py draft OUT/             # draft_*.json of `prism draft`

Each prints one JSON object. Every metric compares against a no-AI baseline
where one exists; the numbers go into docs/AI.md.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from itertools import combinations
from pathlib import Path
from typing import Any

TRIAGE_STATUSES = {"FAILED", "CRASH", "SANFAIL", "ERROR", "TIMEOUT", "UNKNOWN", "NEEDS-HARNESS", "BOUNDED",
                   "HYPOTHESIS"}


def findings(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    out = {}
    for st in report.get("stages", []):
        for i, f in enumerate(st.get("findings", [])):
            out[f"{st['name']}#{i}"] = f
    return out


def pairwise(groups: list[list[str]], label: dict[str, str]) -> dict[str, float]:
    """Pairwise precision / recall / F1 of a clustering against labels."""
    same_cluster = set()
    for g in groups:
        for a, b in combinations(sorted(g), 2):
            same_cluster.add((a, b))
    by_label: dict[str, list[str]] = {}
    for k, v in label.items():
        by_label.setdefault(v, []).append(k)
    same_label = set()
    for g in by_label.values():
        for a, b in combinations(sorted(g), 2):
            same_label.add((a, b))
    tp = len(same_cluster & same_label)
    p = tp / len(same_cluster) if same_cluster else 1.0
    r = tp / len(same_label) if same_label else 1.0
    f1 = 2 * p * r / (p + r) if p + r else 0.0
    return {"precision": round(p, 4), "recall": round(r, 4), "f1": round(f1, 4), "groups": len(groups)}


def measure_triage(out: Path) -> dict[str, Any]:
    report = json.loads((out / "report.json").read_text(encoding="utf-8"))
    tri = json.loads((out / "triage.json").read_text(encoding="utf-8"))
    fs = {k: f for k, f in findings(report).items() if f.get("status") in TRIAGE_STATUSES}
    # Proxy root cause: the function (file + name); findings without one keep
    # their own (file, line). Documented as a proxy in docs/AI.md.
    label = {k: f"{f.get('file')}::{f.get('function') or ('L' + str(f.get('line')))}" for k, f in fs.items()}
    base: dict[tuple[Any, ...], list[str]] = {}
    for k, f in fs.items():
        base.setdefault((f.get("file"), f.get("line"), f.get("cls"), f.get("message")), []).append(k)
    exact: dict[tuple[Any, ...], list[str]] = {}
    for k, f in fs.items():
        exact.setdefault((f.get("file"), f.get("line"), f.get("cls")), []).append(k)
    clusters = [c["members"] for c in tri.get("clusters", [])]
    return {"findings": len(fs), "labels": len(set(label.values())),
            "baseline_identical": pairwise(list(base.values()), label),
            "baseline_file_line_cls": pairwise(list(exact.values()), label),
            "triage": pairwise(clusters, label), "embedder": tri.get("embedder")}


def measure_ask(out: Path, questions: Path, prism: str) -> dict[str, Any]:
    rows = [json.loads(line) for line in questions.read_text(encoding="utf-8").splitlines() if line.strip()]
    ok = 0
    fields_ok = 0
    fields = 0
    misses = []
    for r in rows:
        p = subprocess.run([prism, "ask", r["question"], "--report", str(out / "report.json"), "--json", "--no-llm"],
                           capture_output=True, text=True, timeout=120)
        got = json.loads(p.stdout)["query"]
        exp = r["expect"]
        good = True
        for k, v in exp.items():
            fields += 1
            g = got.get(k)
            same = sorted(g) == sorted(v) if isinstance(v, list) and isinstance(g, list) else g == v
            fields_ok += same
            good = good and same
        ok += good
        if not good:
            misses.append({"question": r["question"], "got": {k: got.get(k) for k in exp}, "expect": exp})
    return {"questions": len(rows), "exact": ok, "exact_share": round(ok / max(1, len(rows)), 4),
            "field_accuracy": round(fields_ok / max(1, fields), 4), "misses": misses}


def measure_regress(out: Path) -> dict[str, Any]:
    m = json.loads((out / "regression_tests" / "manifest.json").read_text(encoding="utf-8"))
    by: dict[str, int] = {}
    for t in m["tests"]:
        by[t["status"]] = by.get(t["status"], 0) + 1
    tests = sum(v for k, v in by.items() if k not in ("unsupported", "duplicate"))
    return {"findings": len(m["tests"]), "by_status": by, "tests": tests,
            "reproduce_share": round(by.get("reproduces", 0) / max(1, tests), 4)}


def measure_draft(out: Path) -> dict[str, Any]:
    res = {}
    for p in sorted(out.glob("draft_*.json")):
        d = json.loads(p.read_text(encoding="utf-8"))
        claims = sum(len(s["claims"]) for s in d["sections"])
        linked = sum(1 for s in d["sections"] for c in s["claims"] if c["links"])
        res[p.stem] = {"claims": claims, "linked": linked, "rejected": len(d["rejected"]),
                       "theorems_indexed": d.get("theorems_indexed"), "author": d.get("author")}
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("what", choices=["triage", "ask", "regress", "draft"])
    ap.add_argument("out", type=Path)
    ap.add_argument("questions", type=Path, nargs="?")
    ap.add_argument("--prism", default="build/prism")
    a = ap.parse_args(argv)
    if a.what == "triage":
        r = measure_triage(a.out)
    elif a.what == "ask":
        if not a.questions:
            ap.error("ask needs a questions file")
        r = measure_ask(a.out, a.questions, a.prism)
    elif a.what == "regress":
        r = measure_regress(a.out)
    else:
        r = measure_draft(a.out)
    json.dump(r, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
