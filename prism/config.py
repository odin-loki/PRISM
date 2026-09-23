from __future__ import annotations

from collections.abc import Callable, Iterable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import TypeVar
import os
import shutil
import sys

_T = TypeVar("_T")
_R = TypeVar("_R")

# Adapter binaries: search (1) Config.tools / --tool, (2) a built executable
# under third_party/<vendor>/ if already present, (3) PATH. Missing is NOTRUN
# with install pointing at the vendored tree in third_party/SOURCES.md.
# Do not compile these trees from the adapter.
VENDOR_DIR = {
    "esbmc": "esbmc",
    "dafny": "dafny",
    "cppcheck": "cppcheck",
    "klee": "klee",
    "afl-fuzz": "AFLplusplus",
    "frama-c": "Frama-C",
    "infer": "infer",
    "codeql": "codeql",
    "cbmc": "cbmc",
    "strix": "strix",
    "semgrep": "semgrep",
    "spatch": "coccinelle",
    "fuse": "FuSeBMC",
    "fuzz4all": "Fuzz4All",
}

# Known *output* dirs only. "src/" is the vendored source tree, not a proof.
_BUILD_SUBDIRS = (
    "",
    "bin",
    "build",
    "build/bin",
    "build/src",
    "Release",
    "Debug",
    "build/Release",
    "build/Debug",
)

_SKIP_VENDOR_PARTS = {
    "scripts", "tests", "docs", "examples", "regression", "website",
    "ql", "misc", "javascript", "change-notes", ".github",
}

# Only descend into these names during the shallow glob (never src/).
_WALK_ALLOW = {
    "bin", "build", "release", "debug", "out", "dist", "install",
}

_SOURCE_SUFFIX = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".py", ".md", ".txt",
    ".json", ".o", ".obj", ".a", ".lib", ".so", ".dll", ".sh", ".bat",
}


def repo_root() -> Path:
    here = Path(__file__).resolve().parents[1]
    if (here / "third_party" / "SOURCES.md").is_file():
        return here
    cwd = Path.cwd()
    for p in (cwd, *cwd.parents):
        if (p / "third_party" / "SOURCES.md").is_file():
            return p
    return here


def adapter_install(stage: str) -> str:
    vendor = VENDOR_DIR.get(stage)
    if vendor:
        return f"build from vendored third_party/{vendor} (see third_party/SOURCES.md)"
    return f"{stage} is not vendored (see third_party/SOURCES.md)"


def _is_built_exe(path: Path) -> bool:
    try:
        if not path.is_file():
            return False
    except OSError:
        return False
    if path.suffix.lower() in _SOURCE_SUFFIX:
        return False
    parts = {p.lower() for p in path.parts}
    if parts & _SKIP_VENDOR_PARTS:
        return False
    if sys.platform == "win32":
        return path.suffix.lower() == ".exe"
    try:
        return os.access(path, os.X_OK)
    except OSError:
        return False


def _shallow_glob_exe(root: Path, names: tuple[str, ...], max_depth: int = 3) -> str | None:
    """Walk a few levels under a vendored tree for a built binary. Never compile."""
    want = {str(n).lower() for n in names}
    want |= {n if n.endswith(".exe") else n + ".exe" for n in want}
    skip = _SKIP_VENDOR_PARTS | {
        ".git", "node_modules", "__pycache__", "src", "include", "lib",
    }
    try:
        if not root.is_dir():
            return None
    except OSError:
        return None
    stack: list[tuple[Path, int]] = [(root, 0)]
    seen: set[str] = set()
    visited = 0
    while stack:
        d, depth = stack.pop()
        try:
            entries = list(d.iterdir())
        except OSError:
            continue
        for ent in entries:
            visited += 1
            if visited > 4000:
                return None
            try:
                name = ent.name
            except OSError:
                continue
            low = name.lower()
            if low.startswith(".") or low in skip:
                continue
            key = str(ent)
            if key in seen:
                continue
            seen.add(key)
            try:
                is_dir = ent.is_dir()
            except OSError:
                continue
            if is_dir:
                # Do not walk source trees. release/out is a build layout.
                if low not in _WALK_ALLOW:
                    continue
                if depth + 1 <= max_depth:
                    stack.append((ent, depth + 1))
                continue
            if low in want and _is_built_exe(ent):
                return str(ent)
    return None


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
    """Return a pre-built binary under third_party/<vendor>/, or None.

    Never compiles. Known output dirs first, then a shallow glob
    (esbmc, cbmc, klee, cppcheck, infer, dafny, strix, semgrep, frama-c, afl-fuzz).
    Law 9: a repo root inside ``exclude`` (the scanned tree) is refused — a
    hostile tree could plant third_party/SOURCES.md and a fake esbmc.
    """
    vendor = VENDOR_DIR.get(stage)
    if not vendor:
        return None
    base_root = repo_root()
    if exclude is not None and path_within(base_root, exclude):
        return None
    root = base_root / "third_party" / vendor
    try:
        if not root.is_dir():
            return None
    except OSError:
        return None
    names_t = tuple(names)
    seen: set[str] = set()
    for sub in _BUILD_SUBDIRS:
        base = root / sub if sub else root
        for n in names_t:
            cands = [base / n]
            if not str(n).lower().endswith(".exe"):
                cands.append(base / f"{n}.exe")
            for cand in cands:
                key = str(cand)
                if key in seen:
                    continue
                seen.add(key)
                if _is_built_exe(cand):
                    return str(cand)
    return _shallow_glob_exe(root, names_t, max_depth=3)


def resolve_adapter(cfg: Config, stage: str, names: tuple[str, ...] | list[str]) -> str | None:
    """(1) config/explicit, (2) vendored built exe, (3) PATH."""
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
