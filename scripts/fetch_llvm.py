#!/usr/bin/env python3
"""Fetch the pinned LLVM (third_party/llvm/PIN.toml), verify sha256, extract, delete tarball.

usage: fetch_llvm.py [--dest DIR] [--platform P] [--keep-full]
Prints the bin dir; pass it to CMake as -DPRISM_LLVM_PINNED_BIN=... or set PRISM_LLVM_BIN.
"""
import argparse, hashlib, os, platform, shutil, sys, tarfile, tomllib, urllib.request
from fnmatch import fnmatch
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def host():
    m = platform.machine().lower()
    arch = "aarch64" if m in ("arm64", "aarch64") else "x86_64"
    s = sys.platform
    if s.startswith("linux"): return f"linux-{arch}"
    if s == "darwin": return "macos-arm64"
    if s.startswith("win"): return "windows-" + ("arm64" if arch == "aarch64" else "x86_64")
    raise SystemExit(f"no pinned LLVM for {s}/{m}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default=os.environ.get("PRISM_TOOLS_HOME", str(Path.home() / ".cache/prism/tools")))
    ap.add_argument("--platform", default=None)
    ap.add_argument("--keep-full", action="store_true", help="extract everything, not just PIN members")
    a = ap.parse_args()
    pin = tomllib.loads((ROOT / "third_party/llvm/PIN.toml").read_text())
    plat = a.platform or host()
    asset = next((x for x in pin["asset"] if x["platform"] == plat), None)
    if not asset: raise SystemExit(f"no asset for {plat}")
    out = Path(a.dest) / f"llvm-{pin['version']}"
    stamp = out / ".sha256"
    if stamp.exists() and stamp.read_text().strip() == asset["sha256"]:
        print(out / "bin"); return
    Path(a.dest).mkdir(parents=True, exist_ok=True)
    tgz = Path(a.dest) / asset["name"]
    if shutil.disk_usage(a.dest).free < asset["size"] * 5:
        raise SystemExit("not enough disk for download + extract")
    print(f"fetching {asset['name']}", file=sys.stderr)
    urllib.request.urlretrieve(pin["base_url"] + asset["name"].replace("+", "%2B"), tgz)
    h = hashlib.sha256()
    with open(tgz, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    if h.hexdigest() != asset["sha256"]:
        tgz.unlink(); raise SystemExit(f"sha256 mismatch: {h.hexdigest()}")
    out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tgz) as t:
        for m in t:
            parts = m.name.split("/", 1)
            if len(parts) < 2: continue
            rel = parts[1]
            if not a.keep_full and not any(rel == p or rel.startswith(p + "/") or fnmatch(rel, p) or
                                           fnmatch(rel, p + ".exe") for p in pin["members"]):
                continue
            m.name = rel
            t.extract(m, out, filter="data")
    tgz.unlink()
    stamp.write_text(asset["sha256"])
    print(out / "bin")

if __name__ == "__main__":
    main()
