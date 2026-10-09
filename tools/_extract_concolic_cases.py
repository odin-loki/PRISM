#!/usr/bin/env python3
"""One-off helper: print concolic row snippets from the deleted Python test.

Uses git history when tests/test_concolic.py is not in the working tree.
Prefer tools/gen_test_concolic_cpp.py to regenerate the full doctest file.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = ROOT / "tests" / "test_concolic.py"
REV = "190103945^:tests/test_concolic.py"


def main() -> None:
    if PY.is_file():
        src = PY.read_text(encoding="utf-8")
    else:
        r = subprocess.run(
            ["git", "show", REV],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        if r.returncode != 0:
            print(f"cannot read {REV}", file=sys.stderr)
            sys.exit(2)
        src = r.stdout
    # Legacy one-off: only useful with hand-edited tail; run gen_test_concolic_cpp.py instead.
    print("Use: python3 tools/gen_test_concolic_cpp.py", file=sys.stderr)
    _ = src


if __name__ == "__main__":
    main()
