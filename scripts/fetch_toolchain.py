#!/usr/bin/env python3
"""Fetch a pinned toolchain artifact from third_party/TOOLCHAIN_PINS.toml and verify SHA-256.

usage: fetch_toolchain.py NAME [--platform P] [--dest DIR]
Also fetches the lint tools (kind = "lint": ruff, shellcheck, mypy, pyflakes, yamllint,
typescript, eslint; clang-tidy comes from the pinned LLVM archive).
Refuses (exit 2) any download whose SHA-256 differs from the pin. Extracts into
DEST/NAME/VERSION (default ~/.prism/toolchain) and prints that directory.
"""
import argparse, hashlib, os, pathlib, platform, shutil, sys, tarfile, tempfile, tomllib, urllib.request, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

def host_platform():
    s, m = platform.system(), platform.machine().lower()
    arch = "x86_64" if m in ("x86_64", "amd64") else m
    return {"Linux": f"linux-{arch}", "Windows": f"windows-{arch}", "Darwin": "macos-universal"}.get(s, s)

def _download(url, dest):
    h = hashlib.sha256()
    with urllib.request.urlopen(url) as r, open(dest, "wb") as w:
        while chunk := r.read(1 << 20):
            h.update(chunk); w.write(chunk)
    return h.hexdigest()

def _deb_data(deb):
    """(name, bytes) of the data.tar.* member of a .deb (ar archive)."""
    b = deb.read_bytes()
    if not b.startswith(b"!<arch>\n"):
        sys.exit(f"{deb.name}: not a .deb (ar) archive")
    i = 8
    while i + 60 <= len(b):
        name = b[i:i + 16].decode().strip().rstrip("/")
        size = int(b[i + 48:i + 58].decode().strip())
        if name.startswith("data.tar"):
            return name, b[i + 60:i + 60 + size]
        i += 60 + size + (size & 1)
    sys.exit(f"{deb.name}: no data.tar member")

def _untar(name, data, root, td):
    """Extract regular files and directories (headers, crt objects); links are
    skipped (PIR never links, and Windows needs privileges for symlinks)."""
    import io
    if name.endswith(".zst"):
        try:
            from compression import zstd  # Python 3.14+
            data = zstd.decompress(data)
        except ImportError:
            try:
                import zstandard  # pip install zstandard
                data = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)).read()
            except ImportError:
                import subprocess
                data = subprocess.run(["zstd", "-dc"], input=data, capture_output=True, check=True).stdout
        mode = "r:"
    else:
        mode = "r:*"
    with tarfile.open(fileobj=io.BytesIO(data), mode=mode) as t:
        members = [m for m in t.getmembers() if m.isfile() or m.isdir()]
        t.extractall(root, members=members, filter="data")

def fetch_debs(pin, dest):
    out = dest / pin["name"] / pin["version"]
    if (out / ".verified").exists():
        return out
    with tempfile.TemporaryDirectory() as td:
        shutil.rmtree(out, ignore_errors=True); (out / "root").mkdir(parents=True)
        for d in pin["debs"]:
            f = pathlib.Path(td) / d["url"].rsplit("/", 1)[1]
            got = _download(d["url"], f)
            if got != d["sha256"]:
                print(f"SHA-256 mismatch for {d['url']}: got {got}, pinned {d['sha256']}", file=sys.stderr)
                shutil.rmtree(out, ignore_errors=True)
                sys.exit(2)
            _untar(*_deb_data(f), out / "root", td)
        (out / ".verified").write_text("\n".join(d["sha256"] for d in pin["debs"]) + "\n")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name"); ap.add_argument("--platform", default=None)
    ap.add_argument("--dest", default=str(pathlib.Path.home() / ".prism" / "toolchain"))
    a = ap.parse_args()
    pins = tomllib.loads((ROOT / "third_party" / "TOOLCHAIN_PINS.toml").read_text())
    pin = next((p for p in pins["pin"] if p["name"] == a.name), None)
    if pin and "from_pin" in pin:
        # Ships inside another pinned archive (clang-tidy in LLVM): fetch that one.
        sys.exit(f"{a.name} {pin['version']} comes from {pin['from_pin']} ({pin.get('bin', '')}); fetch that pin")
    if pin and "debs" in pin:
        print(fetch_debs(pin, pathlib.Path(a.dest))); return
    if not pin or "artifacts" not in pin:
        sys.exit(f"no fetchable pin named {a.name}")
    arts = pin["artifacts"]
    default = next((k for k in ("source", "python", "node") if k in arts), None)
    plat = a.platform or default or host_platform()
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
        if pin.get("install") in ("pip", "npm"):
            # Wheels / npm tarballs stay packed: install with
            #   pip install --no-deps OUT/<file>   or   npm install OUT/<file>
            shutil.copy2(f, out / f.name)
        elif f.suffix == ".zip":
            zipfile.ZipFile(f).extractall(out)
        else:
            with tarfile.open(f) as t:
                t.extractall(out, filter="data")
        (out / ".verified").write_text(art["sha256"] + "\n")
    print(out)

if __name__ == "__main__":
    main()
