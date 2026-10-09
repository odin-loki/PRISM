#!/usr/bin/env python3
"""Copy Z3 codegen outputs from a reference build into cmake/z3-generated/."""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "cmake" / "z3-generated"
BUILD = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/prism-z3-test")
PIN = "ff553588f5d86e06f51f48a8bc0de7824a10c5f1603027fa486494fb24b7a527"

EXTRA = (
    "api_commands.cpp",
    "api_log_macros.cpp",
    "api_log_macros.h",
    "install_tactic.cpp",
    "mem_initializer.cpp",
    "gparams_register_modules.cpp",
    "database.h",
)


def main() -> int:
    z3_src = ROOT / "third_party" / "z3" / "src"
    z3_build = BUILD / "third_party" / "z3" / "src"
    if not z3_build.is_dir():
        print(f"build tree missing: {z3_build}", file=sys.stderr)
        return 1
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "src").mkdir(parents=True)
    copied = 0
    for pyg in z3_src.rglob("*.pyg"):
        rel = pyg.relative_to(z3_src)
        built = z3_build / rel.with_suffix(".hpp")
        if not built.is_file():
            print(f"warning: no build output for {pyg}", file=sys.stderr)
            continue
        dst = OUT / "src" / rel.with_suffix(".hpp")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(built, dst)
        copied += 1
    for name in EXTRA:
        for src in z3_build.rglob(name):
            rel = src.relative_to(z3_build)
            dst = OUT / "src" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
    (OUT / "tree_sha256").write_text(PIN + "\n", encoding="utf-8")
    print(f"copied {copied} files into {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
