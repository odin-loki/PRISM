#!/usr/bin/env python3
"""Size bound sweep for the libc model contract harnesses (roadmap 8.2).

tests/conformance/libc-models/*_contracts.c prove each model's contract for
objects of up to N bytes (harness.h, default N = 4). This tool re-runs every
`_true` harness alone, with N = 4, 8, 16, ... (`#define N` before
harness.h, `--unwind 2N + 2` so every model loop can close), and reports per
function the largest N that is still PROVED within the conformance timeout
(180 s per PRISM run, as in tools/conformance.py).

A PROVED here is a proof for objects up to that N only (a size-bounded
result, docs/PIR.md "Library models verified by PRISM"), never a proof for
all sizes (Law 2). It is reported as such.

    PRISM_BIN=build/prism python tools/libc_model_bounds.py [--sizes 4,8,16] [-j 2] [--filter RE]
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SUITE = REPO / "tests" / "conformance" / "libc-models"
FUNC = re.compile(r"^int (\w+)\(")


def split_harness(text: str) -> tuple[str, dict[str, str]]:
    """(prelude, {function: its comment and body}) of a harness file."""
    lines = text.splitlines(keepends=True)
    starts = [i for i, ln in enumerate(lines) if FUNC.match(ln)]
    if not starts:
        return text, {}

    def comment_start(i: int) -> int:
        j = i
        while j > 0 and (lines[j - 1].startswith(("/*", " *", "//")) or lines[j - 1].strip() == "*/"):
            j -= 1
        return j

    prelude = "".join(lines[: comment_start(starts[0])])
    # one-line static helpers between harnesses (string_contracts.c `sign`)
    prelude += "".join(ln for ln in lines[starts[0]:] if ln.startswith("static ") and ln.rstrip().endswith("}"))
    funcs: dict[str, str] = {}
    for i in starts:
        name = FUNC.match(lines[i]).group(1)  # type: ignore[union-attr]
        j = i
        one_liner = lines[i].count("{") and lines[i].count("{") == lines[i].count("}")
        while not one_liner and j + 1 < len(lines) and not lines[j].startswith("}"):
            j += 1
        funcs[name] = "".join(lines[comment_start(i): j + 1])
    return prelude, funcs


def absolutize(prelude: str) -> str:
    def fix(m: re.Match[str]) -> str:
        return f'#include "{(SUITE / m.group(1)).resolve()}"'
    return re.sub(r'#include "([^"]+)"', fix, prelude)


def run_one(prism: str, src: Path, fn: str, n: int, timeout: float) -> dict[str, object]:
    out = src.parent / f"out_{fn}_{n}"
    argv = [prism, str(src), "--no-llm", "--stage", "inventory,classify,pir", "--out", str(out),
            "--unwind", str(2 * n + 2), "--solver-cache", str(src.parent / f"cache_{fn}_{n}")]
    t0 = time.monotonic()
    try:
        subprocess.run(argv, capture_output=True, text=True, timeout=timeout, cwd=REPO)
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "seconds": timeout}
    secs = round(time.monotonic() - t0, 1)
    rep = out / "report.json"
    if not rep.exists():
        return {"status": "ERROR", "seconds": secs}
    data = json.loads(rep.read_text(encoding="utf-8"))
    for st in data.get("stages", []):
        if st.get("name") != "pir":
            continue
        rows = [f for f in st.get("findings", []) if f.get("function") == fn]
        if rows:
            order = ["FAILED", "ERROR", "NEEDS-HARNESS", "UNENCODED", "BOUNDED", "PROVED"]
            rows.sort(key=lambda f: order.index(f.get("status")) if f.get("status") in order else 0)
            return {"status": rows[0].get("status"), "seconds": secs, "message": str(rows[0].get("message", ""))[:160]}
    return {"status": "MISSING", "seconds": secs}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prism", default=os.environ.get("PRISM_BIN", str(REPO / "build" / "prism")))
    ap.add_argument("--sizes", default="4,8,16,32,64")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--jobs", "-j", type=int, default=2)
    ap.add_argument("--filter", help="regex on function name")
    ap.add_argument("--json", type=Path, help="write the per-function results here")
    args = ap.parse_args(argv)
    sizes = [int(x) for x in args.sizes.split(",")]
    jobs: list[tuple[str, str, str]] = []  # (file, function, source text without N)
    results: dict[str, dict[str, object]] = {}
    for f in sorted(SUITE.glob("*_contracts.c")):
        prelude, funcs = split_harness(f.read_text(encoding="utf-8"))
        for fn, body in funcs.items():
            if not fn.endswith("_true") or (args.filter and not re.search(args.filter, fn)):
                continue
            if not re.search(r"\bN\b|MAKE_STR|MAKE_BYTES", body):
                # no object of size N: the harness has no size bound to raise
                print(f"{f.name:22} {fn:24} no N in the harness (fixed or symbolic object sizes)", flush=True)
                results[fn] = {"file": f.name, "largest_proved_N": None, "runs": {}}
                continue
            jobs.append((f.name, fn, absolutize(prelude) + "\n" + body))
    with tempfile.TemporaryDirectory(prefix="prism-libc-bounds-") as tmp:

        def sweep(job: tuple[str, str, str]) -> tuple[str, str, dict[int, dict[str, object]]]:
            fname, fn, text = job
            per: dict[int, dict[str, object]] = {}
            for n in sizes:
                d = Path(tmp) / f"{fn}_{n}"
                d.mkdir()
                src = d / f"{fn}.c"
                src.write_text(f"#define N {n}\n" + text, encoding="utf-8")
                per[n] = run_one(args.prism, src, fn, n, args.timeout)
                if per[n]["status"] != "PROVED":
                    break
            return fname, fn, per

        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for fname, fn, per in ex.map(sweep, jobs):
                proved = [n for n, r in per.items() if r["status"] == "PROVED"]
                best = max(proved) if proved else 0
                results[fn] = {"file": fname, "largest_proved_N": best,
                               "runs": {str(n): r for n, r in per.items()}}
                trail = ", ".join(f"N={n}: {r['status']} {r['seconds']}s" for n, r in per.items())
                print(f"{fname:22} {fn:24} largest N proved: {best or '-':>3}   ({trail})", flush=True)
    if args.json:
        args.json.write_text(json.dumps(results, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
