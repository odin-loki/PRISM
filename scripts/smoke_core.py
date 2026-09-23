#!/usr/bin/env python3
"""Smoke the C++ engine (PRISM_BIN, default build/prism) on core plants."""
import os
import subprocess
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRISM = Path(os.environ.get("PRISM_BIN") or ROOT / "build" / "prism")
OUT = Path(os.environ.get("PRISM_SMOKE_OUT") or Path(tempfile.gettempdir()) / "prism-smoke")
OUT.mkdir(parents=True, exist_ok=True)

JOBS = [
    ("abs", "testdata/abs_ok.c", "inventory,classify,lints,bmc,concolic,fuzz,rapid,unify"),
    ("div", "testdata/div_param.c", "inventory,classify,bmc,fuzz,unify"),
    ("ovf", "testdata/add_overflow.c", "inventory,classify,bmc,fuzz,unify"),
    ("oob", "testdata/oob_write.c", "inventory,classify,bmc,fuzz,unify"),
    ("thr", "testdata/iso_thread_race.c", "inventory,classify,lints,thread,bmc"),
    ("join", "testdata/thrd_join_api.c", "inventory,classify,lints"),
]

def main() -> None:
    for name, src, stages in JOBS:
        dest = OUT / name
        print(f"======== {name} ========", flush=True)
        subprocess.run(
            [str(PRISM), str(ROOT / src), "--no-llm", "--jobs", "4",
             "--out", str(dest), "--stage", stages],
            check=False,
        )
        md = dest / "report.md"
        if not md.exists():
            print("missing report")
            continue
        for line in md.read_text(encoding="utf-8").splitlines():
            if any(k in line for k in (
                "PROVED", "FAILED", "CRASH", "NEEDS-HARNESS", "RACE",
                "BOUNDED", "API-THRD", "confidence", "**bmc**", "**fuzz**",
                "**thread**", "**lints**",
            )):
                print(line)
    print("SMOKE_CORE_DONE", flush=True)

if __name__ == "__main__":
    main()
