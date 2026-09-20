#!/usr/bin/env python3
"""Compile bmc.cpp with the /var/tmp/prism-wsl compile_commands.json command."""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

BUILD = Path("/var/tmp/prism-wsl")
FALLBACK = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis") / "build_wsl"
OUT_O = Path("/var/tmp/prism-wsl/CMakeFiles/prism_core.dir/src/prism/bmc.cpp.o")


def main() -> int:
    cc_path = BUILD / "compile_commands.json"
    if not cc_path.is_file():
        cc_path = FALLBACK / "compile_commands.json"
    if not cc_path.is_file():
        print("compile_commands.json missing", file=sys.stderr)
        return 1
    cc = json.loads(cc_path.read_text(encoding="utf-8"))
    OUT_O.parent.mkdir(parents=True, exist_ok=True)
    for e in cc:
        if e["file"].replace("\\", "/").endswith("bmc.cpp"):
            cmd = e["command"]
            if " -o " in cmd:
                cmd = re.sub(r" -o \S+", " -o " + str(OUT_O), cmd, count=1)
            print(cmd[:180], "...", flush=True)
            r = subprocess.run(["bash", "-lc", cmd], cwd=e.get("directory", str(BUILD)))
            return r.returncode
    print("bmc.cpp compile command missing", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
