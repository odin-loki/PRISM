#!/usr/bin/env python3
"""Smoke in-tree PRISM (vendored Z3) on core plants."""
import subprocess
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
PRISM = Path("/var/tmp/prism-wsl/prism")
OUT = Path("/var/tmp/prism-smoke")
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
