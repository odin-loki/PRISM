"""Print a seeded random sample of finding pairs that triage joins but the
exact (file, line, class) key does not, for manual review (docs/AI.md
"Triage": the reviewed precision of the merges triage adds).

    python tools/prism_ai/sample_pairs.py OUT/ [--n 40] [--seed 1]
"""

from __future__ import annotations

import argparse
import json
import random
from itertools import combinations
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--n", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args(argv)
    rep = json.loads((a.out / "report.json").read_text(encoding="utf-8"))
    tri = json.loads((a.out / "triage.json").read_text(encoding="utf-8"))
    fs = {f"{s['name']}#{i}": f for s in rep["stages"] for i, f in enumerate(s["findings"])}
    pairs = []
    for c in tri["clusters"]:
        for x, y in combinations(c["members"], 2):
            fx, fy = fs[x], fs[y]
            if (fx.get("file"), fx.get("line"), fx.get("cls")) != (fy.get("file"), fy.get("line"), fy.get("cls")):
                pairs.append((x, y))
    rnd = random.Random(a.seed)
    # one pair per cluster at most, so a few huge clusters do not dominate
    by_cluster: dict[str, tuple[str, str]] = {}
    rnd.shuffle(pairs)
    member_of = {m: c["id"] for c in tri["clusters"] for m in c["members"]}
    for x, y in pairs:
        by_cluster.setdefault(member_of[x], (x, y))
    sample = list(by_cluster.values())
    rnd.shuffle(sample)
    print(f"{len(pairs)} added pairs in {len(by_cluster)} clusters; sample {min(a.n, len(sample))}")
    for x, y in sample[: a.n]:
        for k in (x, y):
            f = fs[k]
            print(f"  {k:14} {f['status']:13} {f.get('file')}:{f.get('line')} {f.get('function') or '-'} "
                  f"{f.get('cls')} | {f.get('message', '')[:90]}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
