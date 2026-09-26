#!/usr/bin/env python3
"""Build a self-contained SV-COMP tool archive (roadmap 6.3).

    python tools/svcomp/package_archive.py --prism build/prism --out prism-svcomp.tar.gz

The tarball unpacks to ``prism/`` with the binary, wrapper scripts, licence and
a MANIFEST.json (versions and file list). Clang/opt are not bundled; see
tools/svcomp/README.md.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PREFIX = "prism"


def _prism_version(prism: Path) -> str | None:
    try:
        r = subprocess.run([str(prism), "--version"], capture_output=True, text=True, timeout=15, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    line = (r.stdout or "").strip()
    if line.startswith("prism "):
        return line.split()[1]
    return None


def _git_head() -> str | None:
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True, check=False)
    except OSError:
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def package(prism: Path, out: Path) -> dict:
    if not prism.is_file():
        raise SystemExit(f"prism binary not found: {prism}")
    if not os.access(prism, os.X_OK):
        raise SystemExit(f"prism binary is not executable: {prism}")

    static = [
        (HERE / "prism_svcomp.py", "prism_svcomp.py"),
        (HERE / "witness.py", "witness.py"),
        (HERE / "prism.py", "prism.py"),
        (HERE / "README.md", "README.md"),
        (HERE / "fm-tools.yml", "fm-tools.yml"),
        (REPO / "LICENSE", "LICENSE"),
    ]
    for src, _ in static:
        if not src.is_file():
            raise SystemExit(f"missing {src}")

    manifest: dict = {
        "format": "prism-svcomp-archive",
        "format_version": 1,
        "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "prism_version": _prism_version(prism),
        "git_commit": _git_head(),
        "files": [],
    }

    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prism-svcomp-pack-") as tmp:
        root = Path(tmp) / PREFIX
        root.mkdir()
        shutil.copy2(prism, root / "prism")
        os.chmod(root / "prism", 0o755)
        manifest["files"].append("prism")
        for src, name in static:
            shutil.copy2(src, root / name)
            manifest["files"].append(name)
        manifest["files"].sort()
        (root / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        manifest["files"].append("MANIFEST.json")

        with tarfile.open(out, "w:gz") as tar:
            for path in sorted(root.iterdir()):
                tar.add(path, arcname=f"{PREFIX}/{path.name}")
    return manifest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prism", type=Path, required=True, help="path to the PRISM C++ binary")
    ap.add_argument("--out", type=Path, default=Path("prism-svcomp.tar.gz"))
    args = ap.parse_args(argv)
    manifest = package(args.prism.resolve(), args.out.resolve())
    print(f"wrote {args.out} ({len(manifest['files'])} files, prism {manifest.get('prism_version')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
