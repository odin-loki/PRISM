"""What is in scope: one skip-directory list for every stage.

Vendor, build and cache directories are not the user's code. Every stage
that walks the tree (C sources, polyglot, secrets) skips the same names,
and the inventory stage records each skipped directory that holds source
files as UNKNOWN, so nothing is skipped silently (Law 7).

Same table and walk as the C++ engine (src/prism/scope.cpp,
include/prism/scope.hpp); tests/test_polyglot.py locks them together.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import os

SKIP_DIRS: frozenset[str] = frozenset({
    ".git", "prism-out", "third_party", "build", "node_modules", "__pycache__",
    ".venv", "venv", "target", ".tox", ".mypy_cache", ".ruff_cache", ".pytest_cache",
})

# prism-out-gui, build-release, build_wsl, ... are output trees too.
SKIP_PREFIXES: tuple[str, ...] = ("prism-out", "build")


def skip_dir(name: str) -> bool:
    """True for a directory name no stage descends into."""
    return name in SKIP_DIRS or name.startswith(SKIP_PREFIXES)


def skipped_path(path: Path, root: Path) -> bool:
    """True when a directory between `root` and `path` is skipped.

    Only the parts below `root` count: a scan root that itself lives under
    a `build/` directory still scans its own files.
    """
    try:
        rel = Path(path).relative_to(root)
    except ValueError:
        return False
    return any(skip_dir(part) for part in rel.parts[:-1])


def skipped_dirs(root: Path, is_source: Callable[[Path], bool]) -> list[tuple[str, int]]:
    """(dir relative to root, source-file count) for each top-most skipped
    directory that holds at least one source file. Sorted by dir."""
    out: list[tuple[str, int]] = []
    if not root.is_dir():
        return out
    for dirpath, dirnames, _files in os.walk(root):
        keep = []
        for d in sorted(dirnames):
            full = Path(dirpath) / d
            if full.is_symlink():
                continue
            if skip_dir(d):
                n = _count_sources(full, is_source)
                if n:
                    out.append((full.relative_to(root).as_posix(), n))
            else:
                keep.append(d)
        dirnames[:] = keep
    return sorted(out)


def _count_sources(d: Path, is_source: Callable[[Path], bool]) -> int:
    n = 0
    for dirpath, dirnames, filenames in os.walk(d):
        dirnames[:] = [x for x in dirnames if not (Path(dirpath) / x).is_symlink()]
        for name in filenames:
            if is_source(Path(dirpath) / name):
                n += 1
    return n


def skipped_message(rel_dir: str, n: int) -> str:
    return f"skipped {rel_dir}/ ({n} source files): vendor/build directory"
