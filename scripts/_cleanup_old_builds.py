"""Remove old PRISM build trees and leftover smoke/agent dumps. Source stays."""
from __future__ import annotations

import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DIRS = [
    ROOT / "build",
    ROOT / "build_cxx",
    ROOT / "build_wsl",
    ROOT / "native" / "build",
    ROOT / "%TEMP%",
    ROOT / "__pycache__",
    ROOT / "prism" / "__pycache__",
    ROOT / "tests" / "__pycache__",
    ROOT / "scripts" / "__pycache__",
    ROOT / "prism-out",
    ROOT / "prism-out-abs",
    ROOT / "prism-out-smoke",
    ROOT / "prism-out-abs",
    ROOT / "prism-out-abs-ok",
    ROOT / "prism-out-acsl",
    ROOT / "prism-out-div",
    ROOT / "prism-out-fuzz",
    ROOT / "prism-out-oob",
    ROOT / "prism-out-plan-done-win",
    ROOT / "prism-out-quick",
    ROOT / "prism-out-smoke-abs",
    ROOT / "prism-out-smoke-div",
    ROOT / "prism-out-wp",
]

FILES = [
    ROOT / "err.txt",
    ROOT / "err2.txt",
    ROOT / "err3.txt",
    ROOT / "err4.txt",
    ROOT / "err5.txt",
    ROOT / "err6.txt",
    ROOT / "err7.txt",
    ROOT / "err8.txt",
    ROOT / "_tmp_leftover26_report.txt",
    ROOT / "_tmp_prior_honesty.txt",
    ROOT / "scripts" / "leftover27_scan.py",
    ROOT / "scripts" / "leftover27_scan2.py",
    ROOT / "scripts" / "write_leftover27.py",
]


def rm(p: Path) -> None:
    if not p.exists():
        print("skip missing", p.name)
        return
    if p.is_dir():
        shutil.rmtree(p, ignore_errors=True)
        print("rmdir", p.relative_to(ROOT))
    else:
        p.unlink(missing_ok=True)
        print("rm", p.relative_to(ROOT))


def main() -> None:
    for p in DIRS + FILES:
        rm(p)
    # leftover agent dump if cleanup left a stub
    leftover_self = Path(__file__)
    leftover_self.unlink(missing_ok=True)
    print("cleanup done")


if __name__ == "__main__":
    main()
