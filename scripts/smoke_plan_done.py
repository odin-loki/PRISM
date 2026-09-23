#!/usr/bin/env python3
"""PLAN.md 'done' smoke: prove a SCALAR, crash a plant, name missing ESBMC."""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRISM = Path(os.environ.get("PRISM_BIN") or ROOT / "build" / "prism")
OUT = Path(os.environ.get("PRISM_SMOKE_OUT") or Path(tempfile.gettempdir()) / "prism-plan-done")


def load_report(dest: Path) -> dict:
    p = dest / "report.json"
    return json.loads(p.read_text(encoding="utf-8"))


def statuses(report: dict, stage: str) -> list[str]:
    out = []
    for s in report.get("stages") or []:
        if s.get("name") != stage:
            continue
        for f in s.get("findings") or []:
            out.append(f.get("status") or "")
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    abs_out = OUT / "abs_ok"
    oob_out = OUT / "oob_write"
    for dest, src in (
        (abs_out, ROOT / "testdata" / "abs_ok.c"),
        (oob_out, ROOT / "testdata" / "oob_write.c"),
    ):
        dest.mkdir(parents=True, exist_ok=True)
        print(f"======== {src.name} ========", flush=True)
        r = subprocess.run(
            [
                str(PRISM),
                str(src),
                "--no-llm",
                "--jobs",
                "4",
                "--out",
                str(dest),
                "--skip",
                "llm,optional,repair,execute",
            ],
            check=False,
        )
        print("exit", r.returncode, flush=True)
        report = load_report(dest)
        print("confidence", report.get("confidence"), flush=True)
        for s in report.get("stages") or []:
            finds = s.get("findings") or []
            if not finds:
                print(f"  {s.get('name')}: (no findings)", flush=True)
                continue
            for f in finds[:6]:
                print(
                    f"  {s.get('name')}: {f.get('status')} {f.get('cls','')} {f.get('message','')[:90]}",
                    flush=True,
                )
            if len(finds) > 6:
                print(f"  ... {len(finds)-6} more", flush=True)

    abs_rep = load_report(abs_out)
    oob_rep = load_report(oob_out)
    bmc = statuses(abs_rep, "bmc")
    esbmc = statuses(abs_rep, "esbmc")
    fuzz = statuses(oob_rep, "fuzz") + statuses(oob_rep, "fuse") + statuses(oob_rep, "concolic")
    ok = True
    if "PROVED-UNBOUNDED" not in bmc:
        print("FAIL: abs_ok missing PROVED-UNBOUNDED", bmc, flush=True)
        ok = False
    else:
        print("OK: abs_ok PROVED-UNBOUNDED", flush=True)
    if esbmc and any(s in {"CLEAN", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"} for s in esbmc):
        print("FAIL: missing ESBMC treated as proof", esbmc, flush=True)
        ok = False
    elif esbmc and all(s == "NOTRUN" for s in esbmc):
        print("OK: ESBMC NOTRUN", flush=True)
    elif not esbmc:
        print("NOTE: no esbmc findings (stage skipped or empty)", flush=True)
    if "CRASH" not in fuzz:
        print("FAIL: oob_write no CRASH", fuzz, flush=True)
        ok = False
    else:
        print("OK: oob_write CRASH", flush=True)
    print("PLAN_DONE_SMOKE", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
