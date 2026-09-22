"""Stage PRISM sources safely: skip >10MB and paths longer than 240 chars."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 10 * 1024 * 1024
MAX_REL = 240  # leave headroom under Windows MAX_PATH once .git/objects prefixes


def ignored_by_git(rel: str) -> bool:
    r = subprocess.run(
        ["git", "check-ignore", "-q", rel],
        cwd=ROOT,
        capture_output=True,
    )
    return r.returncode == 0


def collect() -> list[str]:
    out: list[str] = []
    skipped_big = skipped_long = 0
    for p in ROOT.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(ROOT).as_posix()
        if rel.startswith(".git/") or "/.git/" in rel:
            continue
        if ignored_by_git(rel):
            continue
        try:
            sz = p.stat().st_size
        except OSError:
            continue
        if sz > LIMIT:
            skipped_big += 1
            continue
        if len(rel) > MAX_REL:
            skipped_long += 1
            continue
        out.append(rel)
    print(f"collect {len(out)} files; skip big={skipped_big} long={skipped_long}", flush=True)
    return out


def add_batch(paths: list[str]) -> None:
    # git add via stdin null-separated to avoid cmdline limits
    payload = "\0".join(paths) + ("\0" if paths else "")
    r = subprocess.run(
        ["git", "add", "-A", "--", *paths[:0]],  # placeholder unused
        cwd=ROOT,
        capture_output=True,
    )
    # Prefer: git add --pathspec-from-file=- --pathspec-file-nul
    r = subprocess.run(
        ["git", "add", "--pathspec-from-file=-", "--pathspec-file-nul"],
        cwd=ROOT,
        input=payload.encode("utf-8"),
        capture_output=True,
    )
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode("utf-8", errors="replace"))
        # fallback: smaller batches with git add --
        for i in range(0, len(paths), 200):
            batch = paths[i : i + 200]
            rr = subprocess.run(["git", "add", "--", *batch], cwd=ROOT, capture_output=True)
            if rr.returncode != 0:
                # one-by-one for failures
                for p in batch:
                    subprocess.run(["git", "add", "--", p], cwd=ROOT, capture_output=True)
    print("add done", flush=True)


def main() -> int:
    # Always stage top-level project pieces first (non-vendor)
    core = [
        ".gitignore",
        "CMakeLists.txt",
        "README.md",
        "requirements.txt",
        "docs",
        "prism",
        "include",
        "src",
        "scripts",
        "tests",
        "testdata",
        "tools",
        "third_party/SOURCES.md",
        "third_party/vendor.log",
        "third_party/doctest",
        "third_party/nlohmann",
        "third_party/xsimd",
        "third_party/pcre2",
        "third_party/z3",
        "third_party/rapidcheck",
        "third_party/llama.cpp",
        "third_party/AFLplusplus",
        "third_party/Frama-C",
        "third_party/FuSeBMC",
        "third_party/Fuzz4All",
        "third_party/cbmc",
        "third_party/coccinelle",
        "third_party/codeql",
        "third_party/cppcheck",
        "third_party/dafny",
        "third_party/esbmc",
        "third_party/infer",
        "third_party/klee",
        "third_party/semgrep",
        "third_party/strix",
    ]
    for item in core:
        p = ROOT / item
        if not p.exists():
            print("missing", item)
            continue
        print("git add", item, flush=True)
        r = subprocess.run(["git", "add", "--", item], cwd=ROOT, capture_output=True)
        if r.returncode != 0:
            err = r.stderr.decode("utf-8", errors="replace")
            print("WARN add failed:", item, err[:500], flush=True)
            # fall back to file-by-file under that tree
            if p.is_dir():
                files = []
                for f in p.rglob("*"):
                    if not f.is_file():
                        continue
                    rel = f.relative_to(ROOT).as_posix()
                    if ignored_by_git(rel) or len(rel) > MAX_REL:
                        continue
                    try:
                        if f.stat().st_size > LIMIT:
                            continue
                    except OSError:
                        continue
                    files.append(rel)
                for i in range(0, len(files), 100):
                    batch = files[i : i + 100]
                    subprocess.run(["git", "add", "--", *batch], cwd=ROOT, capture_output=True)
                    print(f"  batched {i+len(batch)}/{len(files)}", flush=True)

    staged = subprocess.check_output(["git", "diff", "--cached", "--name-only"], cwd=ROOT)
    n = len([ln for ln in staged.decode().splitlines() if ln.strip()])
    print("staged count", n, flush=True)
    return 0 if n else 1


if __name__ == "__main__":
    raise SystemExit(main())
