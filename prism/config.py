from __future__ import annotations

from collections.abc import Callable, Iterable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, TypeVar
import functools
import hashlib
import os
import re
import shutil
import sys
import threading
import tomllib

_T = TypeVar("_T")
_R = TypeVar("_R")

# Adapter binaries: search (1) Config.tools / --tool, (2) the pinned build
# ~/.prism/tools/<component>/<commit>/bin/ made by scripts/fetch_deps.py
# (commit from third_party/MANIFEST.toml), (3) PATH. Missing is NOTRUN with an
# install hint naming the fetch_deps command. PRISM never compiles a tool from
# an adapter. Stage -> manifest component (same table in src/prism/config.cpp):
VENDOR_DIR = {
    "esbmc": "esbmc",
    "dafny": "dafny",
    "cppcheck": "cppcheck",
    "klee": "klee",
    "afl-fuzz": "aflplusplus",
    "frama-c": "frama-c",
    "infer": "infer",
    "cbmc": "cbmc",
    "strix": "strix",
    "semgrep": "semgrep",
    "spatch": "coccinelle",
    "cadical": "cadical",
    "kissat": "kissat",
    "cake_lpr": "cake_lpr",
}

_HEX40 = re.compile(r"^[0-9a-f]{40}$")


def repo_root() -> Path:
    """The PRISM checkout holding third_party/MANIFEST.toml (package dir first)."""
    here = Path(__file__).resolve().parents[1]
    if (here / "third_party" / "MANIFEST.toml").is_file():
        return here
    cwd = Path.cwd()
    for p in (cwd, *cwd.parents):
        if (p / "third_party" / "MANIFEST.toml").is_file():
            return p
    return here


def tools_home() -> Path:
    """Install root of scripts/fetch_deps.py: $PRISM_TOOLS_DIR or ~/.prism/tools."""
    env = os.environ.get("PRISM_TOOLS_DIR")
    if env:
        return Path(env).expanduser()
    return Path.home() / ".prism" / "tools"


def load_manifest(root: Path | None = None) -> dict[str, dict[str, Any]]:
    """name -> component row of third_party/MANIFEST.toml ({} when unreadable)."""
    path = (root or repo_root()) / "third_party" / "MANIFEST.toml"
    try:
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
    except (OSError, ValueError):  # TOMLDecodeError, and UnicodeDecodeError on non-UTF-8 bytes
        return {}
    comps = data.get("component")
    if not isinstance(comps, list):
        return {}
    return {str(c.get("name")): c for c in comps if isinstance(c, dict) and c.get("name")}


def pinned_commit(component: str, root: Path | None = None) -> str | None:
    """The manifest commit for an external component, or None."""
    row = load_manifest(root).get(component) or {}
    commit = str(row.get("commit") or "")
    if row.get("kind") != "external" or not _HEX40.match(commit):
        return None
    return commit


def adapter_install(stage: str) -> str:
    comp = VENDOR_DIR.get(stage)
    if comp:
        return (f"python scripts/fetch_deps.py --tool {comp} "
                f"(pinned in third_party/MANIFEST.toml)")
    return f"{stage} is a system tool, not pinned by fetch_deps (see third_party/MANIFEST.toml)"


def _is_built_exe(path: Path) -> bool:
    try:
        if not path.is_file():
            return False
    except OSError:
        return False
    if sys.platform == "win32":
        return path.suffix.lower() == ".exe"
    try:
        return os.access(path, os.X_OK)
    except OSError:
        return False


def path_within(p: Path, root: Path) -> bool:
    """True when p is root or lies below it (resolved)."""
    try:
        Path(p).resolve().relative_to(Path(root).resolve())
        return True
    except (ValueError, OSError):
        return False


def find_vendored_exe(
    stage: str, names: tuple[str, ...] | list[str], *, exclude: Path | None = None,
) -> str | None:
    """Return the fetch_deps build of a pinned tool, or None.

    Looks only in <tools_home>/<component>/<commit>/bin/ (then that dir's
    root) for the commit pinned in third_party/MANIFEST.toml: a build of any
    other commit is not the tool the manifest names. Never compiles.
    Law 9: when the manifest or the tools dir lies inside ``exclude`` (the
    scanned tree) nothing is taken from it; a hostile tree could plant both.
    """
    comp = VENDOR_DIR.get(stage)
    if not comp:
        return None
    base_root = repo_root()
    home = tools_home()
    if exclude is not None and (path_within(base_root, exclude) or path_within(home, exclude)):
        return None
    commit = pinned_commit(comp, base_root)
    if not commit:
        return None
    root = home / comp / commit
    for sub in ("bin", ""):
        base = root / sub if sub else root
        for n in names:
            cands = [base / n]
            if sys.platform == "win32" and not str(n).lower().endswith(".exe"):
                cands.append(base / f"{n}.exe")
            for cand in cands:
                if _is_built_exe(cand):
                    return str(cand)
    return None


_IDENTITY_CACHE: dict[tuple[str, int, int], str] = {}
_IDENTITY_LOCK = threading.Lock()


def tool_identity(exe: str | Path) -> str:
    """Which exact build produced a finding (roadmap 1.1: tool SHA in findings).

    The manifest commit when ``exe`` lives under <tools_home>/<name>/<commit>/,
    else ``path:<abs>;sha256:<hash of the file>``. Cached per (path, size, mtime).
    Same format as src/prism/config.cpp tool_identity.
    """
    p = Path(exe)
    try:
        ap = p.resolve()
    except OSError:
        ap = p.absolute()
    try:
        rel = ap.relative_to(tools_home().resolve())
        parts = rel.parts
        if len(parts) >= 3 and _HEX40.match(parts[1]):
            return parts[1]
    except (ValueError, OSError):
        pass
    try:
        st = ap.stat()
    except OSError:
        return f"path:{ap};sha256:unreadable"
    key = (str(ap), st.st_size, st.st_mtime_ns)
    with _IDENTITY_LOCK:
        hit = _IDENTITY_CACHE.get(key)
    if hit:
        return hit
    h = hashlib.sha256()
    try:
        with open(ap, "rb") as fh:
            for block in iter(lambda: fh.read(1 << 20), b""):
                h.update(block)
        ident = f"path:{ap};sha256:{h.hexdigest()}"
    except OSError:
        ident = f"path:{ap};sha256:unreadable"
    with _IDENTITY_LOCK:
        _IDENTITY_CACHE[key] = ident
    return ident


def stamp_tool_sha(findings: Iterable[Any], exe: str | Path | None) -> None:
    """Set extra["tool_sha"] on every finding an external tool produced."""
    if not exe:
        return
    ident = tool_identity(exe)
    for f in findings:
        extra = getattr(f, "extra", None)
        if extra is None:
            f.extra = extra = {}
        extra.setdefault("tool_sha", ident)


def stamps_tool(stage: str, names: tuple[str, ...]) -> Callable[[Callable[..., list[_R]]], Callable[..., list[_R]]]:
    """Decorator for run_<tool>(paths, cfg): stamp tool_sha when the tool resolved."""
    def deco(fn: Callable[..., list[_R]]) -> Callable[..., list[_R]]:
        @functools.wraps(fn)
        def wrapper(paths: Any, cfg: Config, *a: Any, **kw: Any) -> list[_R]:
            out = fn(paths, cfg, *a, **kw)
            stamp_tool_sha(out, resolve_adapter(cfg, stage, names))
            return out
        return wrapper
    return deco


def resolve_adapter(cfg: Config, stage: str, names: tuple[str, ...] | list[str]) -> str | None:
    """(1) config/explicit, (2) pinned fetch_deps build (~/.prism/tools), (3) PATH."""
    names_t = tuple(names)
    for key in (stage, *names_t):
        expl = (cfg.tools or {}).get(key)
        if not expl:
            continue
        p = Path(expl).expanduser()
        try:
            if p.is_file():
                return str(p)
        except OSError:
            continue
    exclude = None if getattr(cfg, "allow_exec", False) else Path(cfg.root)
    hit = find_vendored_exe(stage, names_t, exclude=exclude)
    if hit:
        return hit
    for n in names_t:
        found = shutil.which(n)
        if found:
            return found
    return None


def ordered_map(fn: Callable[[_T], _R], items: Iterable[_T], jobs: int | None) -> list[_R]:
    """``[fn(x) for x in items]``, run on up to ``jobs`` threads.

    For per-file subprocess work (compile, run, lint). Results come back in
    input order, so callers build findings exactly as the serial loop did;
    the first exception (in input order) propagates. jobs <= 1 or a single
    item runs inline, with no pool.
    """
    seq = list(items)
    try:
        n = int(jobs or 1)
    except (TypeError, ValueError):
        n = 1
    n = min(max(1, n), len(seq))
    if n <= 1:
        return [fn(x) for x in seq]
    with ThreadPoolExecutor(max_workers=n) as pool:
        return list(pool.map(fn, seq))


def _default_pbsd() -> Path | None:
    """PRISM_PBSD only. No guessed location: the ParanoidBSD tree is used
    when named explicitly (PRISM_PBSD or --pbsd PATH)."""
    env = os.environ.get("PRISM_PBSD")
    return Path(env) if env else None


def _default_gguf() -> Path:
    env = os.environ.get("PRISM_GGUF")
    if env:
        return Path(env)
    blob = Path.home() / ".ollama" / "models" / "blobs" / (
        "sha256-dec52a44569a2a25341c4e4d3fee25846eed4f6f0b936278e3a3c900bb99d37c"
    )
    return blob


@dataclass
class Config:
    root: Path = Path(".")
    out: Path = Path("prism-out")
    # ParanoidBSD tree (--pbsd PATH or PRISM_PBSD); None = not configured.
    # Importing its tools/verify modules executes external code: Law 9.
    pbsd_root: Path | None = field(default_factory=_default_pbsd)
    model: str = os.environ.get("PRISM_MODEL", "qwen3.5:9b")
    ollama_host: str = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")
    gguf: Path = field(default_factory=_default_gguf)
    llama_server: str = os.environ.get("PRISM_LLAMA_SERVER", "http://127.0.0.1:8080")
    jobs: int = max(1, (os.cpu_count() or 4) // 2)
    timeout: float = 30.0
    unwind: int = 8
    fuzz_budget: float = 8.0
    fuzz_iters: int = 2048
    llm: bool = True
    repair_rounds: int = 3
    stages: list[str] | None = None
    skip: list[str] = field(default_factory=list)
    resume: bool = False
    tools: dict[str, str] = field(default_factory=dict)
    # Law 9: running code from the scanned tree (compiled harnesses,
    # sanitizer builds, perl -c, cargo clippy, eslint) or from the LLM is
    # opt-in (--allow-exec). Default: those steps are NOTRUN.
    allow_exec: bool = False

    def want(self, name: str) -> bool:
        if name in self.skip:
            return False
        if self.stages is None:
            return True
        return name in self.stages

    def which(self, *names: str) -> str | None:
        """PATH lookup for host compilers. Adapter binaries use which_adapter."""
        for n in names:
            p = shutil.which(n)
            if p:
                return p
        return None

    def which_adapter(self, stage: str, *names: str) -> str | None:
        return resolve_adapter(self, stage, names)
