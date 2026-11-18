#!/usr/bin/env python3
"""leandb_fetch_local.py: fetch the pinned Lean data sources from their original hosts and build PRISM's local shards.

    scripts/leandb_fetch_local.py [--yes] [--sources a,b | --all] [--dest DIR] [--out DIR] [--prism PATH]

What it does
  1. Reads data/leandb/PIN.json. By default it selects the sources the pin marks redistributable=false (the ones that are
     NOT committed: unclear licence). --sources a,b selects named sources; --all selects every pinned source, so you can
     rebuild everything from upstream and compare your shards with the committed ones (--compare).
  2. Prints each selected source's name, upstream URL and licence exactly as recorded in the pin, and says that you are
     responsible for complying with each dataset's terms. It asks to continue unless --yes is given.
  3. Downloads exactly the revisions in the pin (Hugging Face dataset revision, Zenodo record, git commit) from the
     original hosts and checks every file's SHA-256 against the pin. A mismatch fails loudly (exit 2) and removes the file.
  4. Normalises the raw files into one SQLite file with tools/leandb/build_lean_db.py (needs pyarrow and ijson for the
     parquet / LeanDojo sources), then runs `prism leandb build-local` (or `import --all` with --all) to write the shards,
     and `prism leandb verify`.
Robust: resumable (HTTP Range into .part files), retries with exponential backoff, configurable destination (default
~/.prism/leandb/downloads, outside the repository), a free-space guard, progress bars with `rich` when it is installed
and plain progress lines otherwise.

Download logic ported from OS-Proving tools/lean_db/fetch_lean_db.py (same author; tested there on the real sources).
It never alters licence data: the licence text shown and stored is the pin's.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
UA = {"User-Agent": "prism-leandb-fetch/1.0"}
EXIT_MISMATCH = 2

try:  # progress bars when rich is available and we are on a terminal
    from rich.progress import BarColumn, DownloadColumn, Progress, TextColumn, TimeRemainingColumn, TransferSpeedColumn

    HAVE_RICH = sys.stderr.isatty()
except Exception:  # pragma: no cover
    HAVE_RICH = False


class Bar:
    """One download progress display: rich bar or plain lines."""

    _rich = None

    def __init__(self, label, total):
        self.label, self.total, self.done, self.last = label, total or 0, 0, 0.0
        if HAVE_RICH:
            if Bar._rich is None:
                Bar._rich = Progress(TextColumn("{task.description}"), BarColumn(), DownloadColumn(), TransferSpeedColumn(),
                                     TimeRemainingColumn(), transient=True)
                Bar._rich.start()
            self.tid = Bar._rich.add_task(label, total=total or None)

    def advance(self, n):
        self.done += n
        if HAVE_RICH:
            Bar._rich.advance(self.tid, n)
        elif time.time() - self.last > 5:
            self.last = time.time()
            pct = f" {100 * self.done / self.total:.0f}%" if self.total else ""
            print(f"  {self.label}: {self.done / 1e6:.1f} MB{pct}", file=sys.stderr, flush=True)

    def close(self):
        if HAVE_RICH:
            Bar._rich.remove_task(self.tid)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def file_url(src, rel, f):
    """The upstream URL of one pinned file."""
    if f.get("url"):
        return f["url"]
    if src["kind"] == "hf":
        return f"https://huggingface.co/datasets/{src['repo']}/resolve/{src['revision']}/{rel}"
    if src["kind"] == "zenodo":
        return f"https://zenodo.org/records/{src['record']}/files/{rel}?download=1"
    raise ValueError(f"no URL for {src['name']}/{rel}")


def download(url, out, size, label, attempts=8):
    """Range-resumable download to `out` through `out.part`; retries with backoff. Returns nothing; raises on failure."""
    os.makedirs(os.path.dirname(out), exist_ok=True)
    part = out + ".part"
    delay = 2
    for attempt in range(1, attempts + 1):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        if size and have > size:
            os.remove(part)
            have = 0
        bar = Bar(label, size)
        bar.advance(have)
        try:
            if size and have == size:
                break
            req = urllib.request.Request(url, headers=dict(UA, **({"Range": f"bytes={have}-"} if have else {})))
            with urllib.request.urlopen(req, timeout=120) as r:
                restart = have and getattr(r, "status", 200) != 206 and not url.startswith("file:")
                if restart:  # the server ignored the range: start this file again
                    have = 0
                mode = "wb" if (restart or url.startswith("file:")) else ("ab" if have else "wb")
                with open(part, mode) as f:
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
                        bar.advance(len(b))
            if size and os.path.getsize(part) != size:
                raise IOError(f"{label}: got {os.path.getsize(part)} bytes, expected {size}")
            break
        except urllib.error.HTTPError as e:
            if e.code == 416 and os.path.exists(part):  # already complete
                break
            err = e
        except (urllib.error.URLError, IOError, OSError, TimeoutError) as e:
            err = e
        finally:
            bar.close()
        if attempt == attempts:
            raise IOError(f"{label}: giving up after {attempts} attempts: {err}")
        print(f"  {label}: {err}; retry {attempt}/{attempts - 1} in {delay} s", file=sys.stderr)
        time.sleep(delay)
        delay = min(delay * 2, 300)
    os.replace(part, out)


def fail_mismatch(msg):
    print(msg, file=sys.stderr)
    sys.exit(EXIT_MISMATCH)


def fetch_file(src, f, dest, force=False):
    name = src["name"]
    rel = f["path"][len(name) + 1:] if f["path"].startswith(name + "/") else f["path"]
    out = os.path.join(dest, name, rel)
    want = f["sha256"]
    if os.path.exists(out) and os.path.getsize(out) == f["bytes"] and sha256_file(out) == want:
        print(f"  {name}/{rel}: already present, hash ok")
        return
    download(file_url(src, rel, f), out, f["bytes"], f"{name}/{rel}")
    got = sha256_file(out)
    if got != want:
        os.remove(out)
        fail_mismatch(f"SHA-256 MISMATCH for {name}/{rel}: pinned {want}, downloaded {got}. The file was removed. "
                      f"The upstream data changed or the download is corrupt; nothing was imported.")
    print(f"  {name}/{rel}: ok ({f['bytes'] / 1e6:.1f} MB, sha256 matches the pin)")


def fetch_git(src, f, dest):
    commit = f["sha256"].split(":", 1)[1] if f["sha256"].startswith("git:") else src["revision"]
    d = os.path.join(dest, src["name"])
    if not os.path.isdir(os.path.join(d, ".git")):
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)
        subprocess.run(["git", "-C", d, "remote", "add", "origin", src["url"]], check=True)
    head = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != commit:
        for attempt in range(1, 6):
            r = subprocess.run(["git", "-C", d, "fetch", "--depth", "1", "-q", "origin", commit], env=dict(os.environ, GIT_TERMINAL_PROMPT="0"))
            if r.returncode == 0:
                break
            if attempt == 5:
                sys.exit(f"git fetch of {src['name']} {commit} failed")
            time.sleep(2 ** attempt)
        subprocess.run(["git", "-C", d, "checkout", "-q", "--force", commit], check=True)
    head = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != commit:
        fail_mismatch(f"COMMIT MISMATCH for {src['name']}: pinned {commit}, got {head}")
    print(f"  {src['name']}: git commit {commit[:12]} ok")


def tsv_escape(s):
    return str(s).replace("\t", " ").replace("\n", " ")


def prepare_normaliser(pin, selected, dest):
    """A scratch tree in OS-Proving's layout (tools/lean_db, data/lean-db) built from the pin, so the normaliser runs unmodified."""
    scratch = os.path.join(dest, "_normalise")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, "tools", "lean_db"))
    os.makedirs(os.path.join(scratch, "data", "lean-db"))
    for fn in ("build_lean_db.py", "leanparse.py"):
        shutil.copy(os.path.join(REPO, "tools", "leandb", fn), os.path.join(scratch, "tools", "lean_db", fn))
    srcs = []
    for s in pin["sources"]:
        e = {"name": s["name"], "kind": s["kind"], "use": s["use"], "license": s["license"]}
        for k in ("url", "repo", "record", "note"):
            if k in s:
                e[k] = s[k]
        srcs.append(e)
    json.dump({"sources": srcs}, open(os.path.join(scratch, "tools", "lean_db", "sources.json"), "w"), indent=1)
    with open(os.path.join(scratch, "data", "lean-db", "manifest.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("name\tkind\tuse\tlicense\tsource_url\trevision\tpath\tbytes\tsha256\tretrieved\n")
        for s in selected:
            for fl in s["files"]:
                f.write("\t".join(tsv_escape(x) for x in (s["name"], s["kind"], s["use"], s["license"], s.get("url", ""), s["revision"], fl["path"], fl["bytes"],
                                                           fl["sha256"], "pinned")) + "\n")
    return scratch


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pin", default=os.path.join(REPO, "data", "leandb", "PIN.json"))
    ap.add_argument("--dest", default=os.environ.get("PRISM_LEANDB_DOWNLOADS", os.path.expanduser("~/.prism/leandb/downloads")),
                    help="download directory (default outside the repository)")
    ap.add_argument("--sources", default="", help="comma separated source names")
    ap.add_argument("--all", action="store_true", help="every pinned source, including the committed ones")
    ap.add_argument("--out", default="", help="shard output directory (default data/leandb/local, or <dest>/rebuilt with --all)")
    ap.add_argument("--compare", default="", help="with --all: compare the rebuilt committed shards with this directory (data/leandb/shards)")
    ap.add_argument("--prism", default=os.environ.get("PRISM_BIN", "prism"), help="the prism binary")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--min-free-gb", type=float, default=5.0, help="free space to keep after the download")
    ap.add_argument("--no-import", action="store_true", help="download and verify only")
    ap.add_argument("--procs", type=int, default=4)
    a = ap.parse_args()

    pin = json.load(open(a.pin, encoding="utf-8"))
    by = {s["name"]: s for s in pin["sources"]}
    if a.all:
        sel = list(pin["sources"])
    elif a.sources:
        miss = [n for n in a.sources.split(",") if n not in by]
        if miss:
            sys.exit(f"not in the pin: {', '.join(miss)}")
        sel = [by[n] for n in a.sources.split(",")]
    else:
        sel = [s for s in pin["sources"] if not s.get("redistributable")]
    if not sel:
        sys.exit("no sources selected")

    print("Selected sources (licence as recorded in the pin):\n")
    for s in sel:
        print(f"  {s['name']}\n    upstream: {s.get('url', '')}  revision/commit/record: {s['revision']}\n    licence:  {s['license']}\n"
              f"    use: {s['use']}; committed in PRISM git: {'yes' if s.get('redistributable') else 'no (local shards only)'}\n    evidence: {s.get('evidence', '')}\n")
    print("You are responsible for complying with each dataset's licence and terms of use. PRISM ships none of this data for the sources marked 'no'; "
          "this script fetches it from the original hosts for you, and the shards it builds stay on your machine.\n")
    if not a.yes:
        if not sys.stdin.isatty() or input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
            sys.exit("cancelled (use --yes for non-interactive runs)")

    need = sum(f["bytes"] for s in sel for f in s["files"])
    os.makedirs(a.dest, exist_ok=True)
    free = shutil.disk_usage(a.dest).free
    if free < need * 1.2 + a.min_free_gb * 1e9:
        sys.exit(f"not enough space on {a.dest}: need about {need * 1.2 / 1e9:.1f} GB plus {a.min_free_gb} GB spare, {free / 1e9:.1f} GB free")

    for s in sel:
        print(f"{s['name']} [{s['kind']}]")
        for f in s["files"]:
            if s["kind"] == "git":
                fetch_git(s, f, a.dest)
            else:
                fetch_file(s, f, a.dest)
    if a.no_import:
        print("downloaded and verified; --no-import: stopping")
        return

    scratch = prepare_normaliser(pin, sel, a.dest)
    db = os.path.join(a.dest, "lean_db.sqlite")
    cmd = [sys.executable, os.path.join(scratch, "tools", "lean_db", "build_lean_db.py"), "--root", a.dest, "--out", db, "--procs", str(a.procs),
           "--only", ",".join(s["name"] for s in sel)]
    print("normalising:", " ".join(cmd))
    if os.path.exists(db):
        os.remove(db)
    rc = subprocess.run(cmd).returncode
    # The normaliser writes lean_db.sqlite.new and renames it over lean_db.sqlite as its very last step, so a database
    # that exists is a finished one. On WSL DrvFS (/mnt/c, /mnt/d) the stat right after that rename can fail and make
    # the normaliser exit 1 after the file is in place; tolerate exactly that case.
    if rc != 0:
        time.sleep(1)
        if not (os.path.exists(db) and os.path.getsize(db) > 0 and not os.path.exists(db + ".new")):
            sys.exit(f"the normaliser failed (exit {rc})")
        print(f"note: the normaliser exited {rc} after writing {db} (rename-then-stat race on WSL drive mounts); the database is complete")

    out =a.out or (os.path.join(a.dest, "rebuilt") if a.all else os.path.join(REPO, "data", "leandb", "local"))
    if a.all:
        sh, lo = os.path.join(out, "shards"), os.path.join(out, "local")
        subprocess.run([a.prism, "leandb", "import", db, "--pin", a.pin, "--committed", sh, "--local", lo], check=True)
        for d in (sh, lo):
            subprocess.run([a.prism, "leandb", "verify", "--db", d, "--pin", a.pin], check=True)
        if a.compare:
            theirs = {x["first_key"]: x["sha256"] for x in json.load(open(os.path.join(a.compare, "manifest.json")))["shards"]}
            mine = json.load(open(os.path.join(sh, "manifest.json")))["shards"]
            # shards never span two (use, table, source) groups, so a rebuild of some sources is compared shard by shard
            # (matched by first key); with --all every committed shard is expected to be found
            same = [x for x in mine if theirs.get(x["first_key"]) == x["sha256"]]
            missing = [x for x in mine if x["first_key"] not in theirs]
            differ = [x for x in mine if x["first_key"] in theirs and theirs[x["first_key"]] != x["sha256"]]
            print(f"reproducibility check against {a.compare}: {len(same)} shards byte-identical, {len(differ)} different, "
                  f"{len(missing)} not in the committed set")
            sys.exit(0 if not differ and not missing and same else 1)
    else:
        subprocess.run([a.prism, "leandb", "build-local", db, "--pin", a.pin, "--out", out], check=True)
        subprocess.run([a.prism, "leandb", "verify", "--db", out, "--pin", a.pin], check=True)
    print("done; `prism leandb search` uses the local shards in data/leandb/local together with the committed ones")


if __name__ == "__main__":
    main()
