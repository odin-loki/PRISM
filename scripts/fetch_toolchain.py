#!/usr/bin/env python3
"""Fetch a pinned toolchain artifact from third_party/TOOLCHAIN_PINS.toml and verify SHA-256.

usage: fetch_toolchain.py NAME [--platform P] [--dest DIR]
Refuses (exit 2) any download whose SHA-256 differs from the pin. Extracts into
DEST/NAME/VERSION (default ~/.prism/toolchain) and prints that directory.
"""
import argparse, hashlib, os, pathlib, platform, shutil, sys, tarfile, tempfile, tomllib, urllib.request, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

def host_platform():
    s, m = platform.system(), platform.machine().lower()
    arch = "x86_64" if m in ("x86_64", "amd64") else m
    return {"Linux": f"linux-{arch}", "Windows": f"windows-{arch}", "Darwin": "macos-universal"}.get(s, s)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name"); ap.add_argument("--platform", default=None)
    ap.add_argument("--dest", default=str(pathlib.Path.home() / ".prism" / "toolchain"))
    a = ap.parse_args()
    pins = tomllib.loads((ROOT / "third_party" / "TOOLCHAIN_PINS.toml").read_text())
    pin = next((p for p in pins["pin"] if p["name"] == a.name), None)
    if not pin or "artifacts" not in pin:
        sys.exit(f"no fetchable pin named {a.name}")
    arts = pin["artifacts"]
    plat = a.platform or ("source" if "source" in arts else host_platform())
    if plat not in arts:
        sys.exit(f"{a.name} {pin['version']}: no artifact for {plat} (have {', '.join(arts)})")
    art = arts[plat]
    out = pathlib.Path(a.dest) / a.name / pin["version"]
    if (out / ".verified").exists():
        print(out); return
    with tempfile.TemporaryDirectory() as td:
        f = pathlib.Path(td) / art["url"].rsplit("/", 1)[1]
        h = hashlib.sha256()
        with urllib.request.urlopen(art["url"]) as r, open(f, "wb") as w:
            while chunk := r.read(1 << 20):
                h.update(chunk); w.write(chunk)
        if h.hexdigest() != art["sha256"]:
            print(f"SHA-256 mismatch for {art['url']}: got {h.hexdigest()}, pinned {art['sha256']}", file=sys.stderr)
            sys.exit(2)
        shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
        if f.suffix == ".zip":
            zipfile.ZipFile(f).extractall(out)
        else:
            with tarfile.open(f) as t:
                t.extractall(out, filter="data")
        (out / ".verified").write_text(art["sha256"] + "\n")
    print(out)

if __name__ == "__main__":
    main()
