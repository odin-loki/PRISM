#!/usr/bin/env python3
"""Fetch and verify PRISM's pinned dependencies (roadmap Part 1.1).

Reads third_party/MANIFEST.toml. Fails closed: any hash, commit or version
mismatch exits non-zero and leaves nothing installed.

  python scripts/fetch_deps.py --list
  python scripts/fetch_deps.py --linked              # in-tree linked libs match the manifest
  python scripts/fetch_deps.py --linked --refetch    # ...and match the pinned upstream archive
  python scripts/fetch_deps.py --tool cadical        # fetch, verify, build into ~/.prism/tools/
  python scripts/fetch_deps.py --tool esbmc --no-build

External tools land in <tools-dir>/<name>/<commit>/ with bin/ (executables),
src/ (the verified source) and PRISM-TOOL.json. <tools-dir> is $PRISM_TOOLS_DIR
or ~/.prism/tools. PRISM looks there (pinned commit only) before PATH and
records the commit as tool_sha in every finding from that tool.

Archive format: the uncompressed tar of `git archive --format=tar
--prefix=<name>-<commit>/ <commit>` after `git fetch --depth 1 <url> <commit>`.
Only the standard library and the git CLI are used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "third_party" / "MANIFEST.toml"
STAMP = "PRISM-TOOL.json"
_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
KINDS = ("linked", "external", "system")
# Files that may appear in a linked tree without being part of it.
_TREE_IGNORE_SUFFIX = (".gguf", ".pyc")
_TREE_IGNORE_DIRS = {"__pycache__", ".git"}


class FetchError(RuntimeError):
    """Any failure that must stop the fetch (fail closed)."""


class HashMismatch(FetchError):
    pass


# ------------------------------------------------------------------ manifest

def load_manifest(path: Path = MANIFEST) -> dict[str, Any]:
    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    comps = data.get("component") or []
    seen: set[str] = set()
    for c in comps:
        validate_component(c)
        if c["name"] in seen:
            raise FetchError(f"manifest: duplicate component {c['name']!r}")
        seen.add(c["name"])
    return data


def validate_component(c: dict[str, Any]) -> None:
    name = c.get("name")
    if not name or not isinstance(name, str):
        raise FetchError("manifest: component without a name")
    kind = c.get("kind")
    if kind not in KINDS:
        raise FetchError(f"manifest: {name}: kind must be one of {KINDS}, got {kind!r}")
    for key in ("url", "spdx", "version"):
        if not c.get(key):
            raise FetchError(f"manifest: {name}: missing {key}")
    if kind == "system":
        return
    if not _HEX40.match(str(c.get("commit", ""))):
        raise FetchError(f"manifest: {name}: commit must be 40 lowercase hex")
    if not _HEX64.match(str(c.get("archive_sha256", ""))):
        raise FetchError(f"manifest: {name}: archive_sha256 must be 64 lowercase hex")
    if kind == "linked" and not c.get("path"):
        raise FetchError(f"manifest: {name}: linked component needs path")


def components(data: dict[str, Any], kind: str | None = None) -> list[dict[str, Any]]:
    comps = list(data.get("component") or [])
    return [c for c in comps if kind is None or c["kind"] == kind]


def find_component(data: dict[str, Any], name: str) -> dict[str, Any]:
    for c in components(data):
        if c["name"] == name or name in (c.get("stages") or []):
            return c
    raise FetchError(f"{name!r} is not in {MANIFEST.relative_to(ROOT)}")


def tools_dir() -> Path:
    env = os.environ.get("PRISM_TOOLS_DIR")
    return Path(env).expanduser() if env else Path.home() / ".prism" / "tools"


# ------------------------------------------------------------------- hashing

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify_sha256(path: Path, expected: str, what: str) -> None:
    got = sha256_file(path)
    if got != expected:
        raise HashMismatch(
            f"{what}: archive sha256 mismatch\n  expected {expected}\n  got      {got}\n"
            "refusing to use it (fail closed). If upstream really changed, re-pin "
            "third_party/MANIFEST.toml in a reviewed commit."
        )


def _tree_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in _TREE_IGNORE_DIRS)
        for fn in filenames:
            if fn.endswith(_TREE_IGNORE_SUFFIX):
                continue
            out.append(Path(dirpath) / fn)
    return sorted(out, key=lambda p: p.relative_to(root).as_posix())


def tree_digest(root: Path) -> str:
    """SHA-256 over sorted '<relpath>\\0<sha256>\\n' lines of every file under root."""
    h = hashlib.sha256()
    for p in _tree_files(root):
        rel = p.relative_to(root).as_posix()
        h.update(f"{rel}\0{sha256_file(p)}\n".encode())
    return h.hexdigest()


# ----------------------------------------------------------------------- git

def _git(args: list[str], cwd: Path, **kw: Any) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=cwd, check=False, capture_output=True, **kw)


def fetch_archive(comp: dict[str, Any], dest_tar: Path) -> None:
    """git fetch the pinned commit and write its git-archive tar to dest_tar.

    Raises FetchError on any failure and HashMismatch when the tar does not
    hash to archive_sha256. dest_tar is removed on failure.
    """
    name, url, commit = comp["name"], comp["url"], comp["commit"]
    with tempfile.TemporaryDirectory(prefix="prism-fetch-") as td:
        repo = Path(td)
        r = _git(["init", "-q", "--bare", "."], repo)
        if r.returncode:
            raise FetchError(f"{name}: git init failed: {r.stderr.decode(errors='replace')}")
        r = _git(["fetch", "-q", "--depth", "1", url, commit], repo)
        if r.returncode:
            raise FetchError(f"{name}: git fetch {url} {commit} failed: "
                             f"{r.stderr.decode(errors='replace').strip()}")
        r = _git(["rev-parse", "FETCH_HEAD^{commit}"], repo, text=True)
        if r.returncode or r.stdout.strip() != commit:
            raise FetchError(f"{name}: fetched {r.stdout.strip()!r}, pinned {commit}")
        with open(dest_tar, "wb") as fh:
            r = subprocess.run(
                ["git", "archive", "--format=tar", f"--prefix={name}-{commit}/", commit],
                cwd=repo, stdout=fh, stderr=subprocess.PIPE, check=False,
            )
        if r.returncode:
            dest_tar.unlink(missing_ok=True)
            raise FetchError(f"{name}: git archive failed: {r.stderr.decode(errors='replace')}")
    try:
        verify_sha256(dest_tar, comp["archive_sha256"], name)
    except HashMismatch:
        dest_tar.unlink(missing_ok=True)
        raise


def extract(tar_path: Path, dest: Path) -> Path:
    """Extract a verified archive; returns the single top-level directory."""
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tar_path) as tf:
        tf.extractall(dest, filter="data")
    tops = [p for p in dest.iterdir() if p.is_dir()]
    if len(tops) != 1:
        raise FetchError(f"{tar_path.name}: expected one top-level directory, got {len(tops)}")
    return tops[0]


# ------------------------------------------------------------ linked checks

def tree_version(comp: dict[str, Any], root: Path = ROOT) -> str | None:
    rel = comp.get("tree_version_file")
    rx = comp.get("tree_version_regex")
    if not rel or not rx:
        return None
    text = (root / comp["path"] / rel).read_text(encoding="utf-8", errors="replace")
    m = re.search(rx, text, re.S)
    if not m:
        return ""
    return ".".join(g for g in m.groups() if g is not None)


def check_linked(comp: dict[str, Any], root: Path = ROOT) -> list[str]:
    """Offline checks: path exists, version marker and tree digest match."""
    errs: list[str] = []
    d = root / comp["path"]
    if not d.is_dir():
        return [f"{comp['name']}: {comp['path']} missing"]
    want = comp.get("tree_version")
    if want is not None:
        got = tree_version(comp, root)
        if got != want:
            errs.append(f"{comp['name']}: in-tree version {got!r} != manifest {want!r}")
    digest = comp.get("tree_sha256")
    if digest:
        got_d = tree_digest(d)
        if got_d != digest:
            errs.append(f"{comp['name']}: tree_sha256 mismatch (in-tree source was modified)\n"
                        f"  expected {digest}\n  got      {got_d}")
    else:
        errs.append(f"{comp['name']}: no tree_sha256 in manifest")
    return errs


def diff_against_upstream(comp: dict[str, Any], upstream: Path, root: Path = ROOT) -> list[str]:
    """'D path' (upstream only) / 'M path' (content differs) / 'A path' (in-tree only)."""
    ours = root / comp["path"]
    up = upstream / comp["upstream_subdir"] if comp.get("upstream_subdir") else upstream
    only = comp.get("upstream_files")

    def rels(base: Path) -> dict[str, Path]:
        m = {p.relative_to(base).as_posix(): p for p in _tree_files(base)}
        if only:
            m = {k: v for k, v in m.items() if k in only}
        return m

    a, b = rels(ours), rels(up)
    out = [f"D {k}" for k in sorted(b) if k not in a]
    out += [f"A {k}" for k in sorted(a) if k not in b]
    out += [f"M {k}" for k in sorted(a) if k in b and sha256_file(a[k]) != sha256_file(b[k])]
    return sorted(out, key=lambda s: (s[2:], s[0]))


def run_linked(data: dict[str, Any], refetch: bool) -> int:
    bad = 0
    for comp in components(data, "linked"):
        errs = check_linked(comp)
        if refetch and not errs:
            with tempfile.TemporaryDirectory(prefix="prism-linked-") as td:
                tar = Path(td) / f"{comp['name']}.tar"
                try:
                    fetch_archive(comp, tar)
                    top = extract(tar, Path(td) / "x")
                except FetchError as exc:
                    errs.append(str(exc))
                else:
                    drift = diff_against_upstream(comp, top)
                    want = sorted(comp.get("drift") or [], key=lambda s: (s[2:], s[0]))
                    if drift != want:
                        errs.append(f"{comp['name']}: in-tree copy differs from pinned upstream "
                                    f"beyond the recorded drift:\n  recorded {want}\n  actual   {drift}")
        status = "OK" if not errs else "FAIL"
        print(f"{status:4} linked {comp['name']} {comp['version']} {comp['commit'][:12]}"
              + (" (archive re-fetched, sha256 verified)" if refetch and not errs else ""))
        for e in errs:
            print("     " + e.replace("\n", "\n     "))
        bad += bool(errs)
    return 1 if bad else 0


# ---------------------------------------------------------------- recipes

def _run(cmd: list[str], cwd: Path) -> None:
    print("  $ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, check=False)
    if r.returncode:
        raise FetchError(f"build step failed ({r.returncode}): {' '.join(cmd)}")


def _jobs() -> str:
    return str(max(1, (os.cpu_count() or 2)))


def _recipe_configure_make(src: Path) -> None:
    _run(["sh", "./configure"], src)
    _run(["make", "-j", _jobs()], src)


def _recipe_cake_lpr(src: Path) -> None:
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise FetchError("cake_lpr: no C compiler (cc/gcc/clang) on PATH")
    # The Makefile's default target: the CakeML-compiled checker (cake_lpr.S,
    # shipped verified) linked with its C FFI shim.
    _run([cc, "-O2", "basis_ffi.c", "cake_lpr.S", "-o", "cake_lpr", "-std=c99"], src)


RECIPES: dict[str, Callable[[Path], None]] = {
    "configure-make": _recipe_configure_make,
    "cake_lpr": _recipe_cake_lpr,
}


# -------------------------------------------------------------- --tool NAME

def install_dir(comp: dict[str, Any], base: Path | None = None) -> Path:
    return (base or tools_dir()) / comp["name"] / comp["commit"]


def fetch_tool(comp: dict[str, Any], base: Path | None = None, build: bool = True) -> Path:
    """Fetch + verify + (optionally) build one external tool. Returns its dir.

    Staged in a temporary sibling and renamed into place only after every
    check and build step passed, so a failure never leaves a half install.
    """
    if comp["kind"] == "system":
        raise FetchError(f"{comp['name']} is a system tool: {comp.get('install', 'install it on PATH')}")
    if comp["kind"] != "external":
        raise FetchError(f"{comp['name']} is {comp['kind']}; use --linked")
    final = install_dir(comp, base)
    if (final / STAMP).is_file():
        stamp = json.loads((final / STAMP).read_text(encoding="utf-8"))
        if stamp.get("archive_sha256") == comp["archive_sha256"] and (
                stamp.get("built") or not build or not comp.get("recipe")):
            print(f"{comp['name']}: already installed at {final}")
            return final
    final.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{comp['commit'][:12]}-", dir=final.parent))
    try:
        tar = stage / "source.tar"
        print(f"{comp['name']}: fetching {comp['url']} @ {comp['commit']}", flush=True)
        fetch_archive(comp, tar)
        print(f"{comp['name']}: sha256 OK {comp['archive_sha256']}")
        top = extract(tar, stage / "x")
        tar.unlink()
        src = stage / "src"
        top.rename(src)
        (stage / "x").rmdir()
        built = False
        bins: list[str] = []
        recipe = comp.get("recipe")
        if build and recipe:
            fn = RECIPES.get(recipe)
            if fn is None:
                raise FetchError(f"{comp['name']}: unknown recipe {recipe!r}")
            fn(src)
            (stage / "bin").mkdir()
            for rel in comp.get("recipe_out") or []:
                exe = src / rel
                if not exe.is_file():
                    raise FetchError(f"{comp['name']}: build did not produce {rel}")
                dst = stage / "bin" / exe.name
                shutil.copy2(exe, dst)
                bins.append(exe.name)
            built = True
        stamp = {
            "name": comp["name"], "version": comp["version"], "url": comp["url"],
            "commit": comp["commit"], "archive_sha256": comp["archive_sha256"],
            "spdx": comp["spdx"], "built": built, "bins": bins,
        }
        (stage / STAMP).write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
        if final.exists():
            shutil.rmtree(final)
        stage.rename(final)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    if built:
        for b in bins:
            print(f"{comp['name']}: installed {final / 'bin' / b}")
    else:
        hint = comp.get("build_hint") or "see the upstream build instructions"
        print(f"{comp['name']}: verified source at {final / 'src'}\n"
              f"  no automatic build recipe. Build it yourself: {hint}\n"
              f"  <prefix> = {final}  (PRISM looks for {final / 'bin'}/{'|'.join(comp.get('bins') or [comp['name']])})")
    return final


# ------------------------------------------------------------------------ cli

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", type=Path, default=MANIFEST)
    ap.add_argument("--tools-dir", type=Path, default=None,
                    help="install root (default $PRISM_TOOLS_DIR or ~/.prism/tools)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--list", action="store_true", help="print the manifest")
    g.add_argument("--linked", action="store_true", help="verify in-tree linked libraries")
    g.add_argument("--tool", metavar="NAME", action="append",
                   help="fetch/verify/build an external tool (repeatable)")
    g.add_argument("--tree-digest", metavar="DIR", type=Path,
                   help="print the tree_sha256 of a directory")
    ap.add_argument("--refetch", action="store_true",
                    help="with --linked: re-fetch each pinned archive and diff it against the tree")
    ap.add_argument("--no-build", action="store_true", help="with --tool: fetch and verify only")
    args = ap.parse_args(argv)

    if args.tree_digest:
        print(tree_digest(args.tree_digest))
        return 0
    try:
        data = load_manifest(args.manifest)
    except (OSError, tomllib.TOMLDecodeError, FetchError) as exc:
        print(f"fetch_deps: bad manifest: {exc}", file=sys.stderr)
        return 2
    if args.list:
        for c in components(data):
            print(f"{c['kind']:8} {c['name']:14} {c['version']:10} {c.get('commit', '-')[:12]:12} {c['spdx']}")
        return 0
    if args.linked:
        return run_linked(data, args.refetch)
    rc = 0
    for name in args.tool:
        try:
            fetch_tool(find_component(data, name), args.tools_dir, build=not args.no_build)
        except HashMismatch as exc:
            print(f"fetch_deps: {exc}", file=sys.stderr)
            return 3
        except FetchError as exc:
            print(f"fetch_deps: {exc}", file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
