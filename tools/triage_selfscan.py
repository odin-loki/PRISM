#!/usr/bin/env python3
"""Triage PRISM self-scan report.sarif: real / false_alarm / out_of_scope.

    python tools/triage_selfscan.py OUT/
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

DEFECTS = frozenset({"FAILED", "CRASH", "SANFAIL"})
CORPUS = ("testdata/", "testdata_tp/", "tests/")
SRC = ("src/prism/", "src/gui/")


def norm(uri: str) -> str:
    p = uri.replace("\\", "/")
    return p[2:] if p.startswith("./") else p


def path_of(r: dict) -> str:
    locs = r.get("locations") or []
    if not locs:
        return ""
    return norm(locs[0].get("physicalLocation", {}).get("artifactLocation", {}).get("uri", ""))


def starts(p: str, prefixes: tuple[str, ...]) -> bool:
    return any(p.startswith(x) for x in prefixes)


def classify(path: str, status: str, stage: str) -> str:
    if starts(path, ("third_party/",)) or "/third_party/" in path:
        return "out_of_scope"
    if starts(path, CORPUS):
        return "false_alarm"
    if starts(path, (".github/",)):
        return "false_alarm"
    if starts(path, SRC):
        if status in ("CRASH", "ERROR") and stage == "sanitize":
            return "real"
        return "false_alarm"
    return "real"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", type=Path, help="directory containing report.sarif")
    args = ap.parse_args()
    doc = json.loads((args.out / "report.sarif").read_text(encoding="utf-8"))
    results = doc["runs"][0]["results"]

    failed = [r for r in results if r.get("properties", {}).get("status") == "FAILED"]
    n_third = sum(1 for r in failed if starts(path_of(r), ("third_party/",)))
    n_corpus = sum(1 for r in failed if starts(path_of(r), CORPUS))
    n_src = sum(1 for r in failed if starts(path_of(r), SRC))
    n_gh = sum(1 for r in failed if starts(path_of(r), (".github/",)))
    print(f"total FAILED: {len(failed)}")
    print(f"third_party (out of scope): {n_third}")
    print(f"testdata/tests (false alarm corpus): {n_corpus}")
    print(f"src/prism|gui (manual review): {n_src}")
    print(f".github (false alarm LANG-LINT): {n_gh}")

    tally = {"real": 0, "false_alarm": 0, "out_of_scope": 0}
    for r in results:
        props = r.get("properties", {})
        if props.get("status") not in DEFECTS:
            continue
        tally[classify(path_of(r), props.get("status", ""), props.get("stage", ""))] += 1
    print(f"summary: {tally['real']} / {tally['false_alarm']} / {tally['out_of_scope']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
