#!/usr/bin/env python3
"""PRISM PLAN.md done smoke (WSL)."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
OUT = Path("/var/tmp/prism-plan-done")


def main() -> int:
    sys.path.insert(0, str(ROOT))
    OUT.mkdir(parents=True, exist_ok=True)
    ok = True
    for src_name, want in (
        ("abs_ok.c", "PROVED-UNBOUNDED"),
        ("oob_write.c", "CRASH"),
    ):
        dest = OUT / src_name.replace(".c", "")
        dest.mkdir(parents=True, exist_ok=True)
        print(f"======== {src_name} ========", flush=True)
        r = subprocess.run(
            [
                sys.executable,
                "-m",
                "prism",
                str(ROOT / "testdata" / src_name),
                "--out",
                str(dest),
                "--jobs",
                "4",
                "--no-llm",
                "--skip",
                "llm,repair,execute",
                "--fuzz-budget",
                "2.0",
                "--fuzz-iters",
                "64",
            ],
            cwd=str(ROOT),
            check=False,
        )
        print("exit", r.returncode, flush=True)
        report = json.loads((dest / "report.json").read_text(encoding="utf-8"))
        print("confidence", report.get("confidence"), flush=True)
        found = []
        esbmc = []
        for s in report.get("stages") or []:
            for f in s.get("findings") or []:
                st = f.get("status") or ""
                found.append((s.get("name"), st, (f.get("message") or "")[:80]))
                if s.get("name") == "esbmc":
                    esbmc.append(st)
        for name, st, msg in found[:20]:
            print(f"  {name}: {st} {msg}", flush=True)
        blob = " ".join(st for _, st, _ in found)
        if want not in blob:
            print("FAIL missing", want, flush=True)
            ok = False
        else:
            print("OK", want, flush=True)
        if esbmc and any(s in {"CLEAN", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"} for s in esbmc):
            print("FAIL ESBMC proof", esbmc, flush=True)
            ok = False
        elif esbmc:
            print("OK esbmc", esbmc, flush=True)
    print("PRISM_PLAN_DONE", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
