"""Concurrency lint: shared global writes without mutex protection.

Missing pthread knowledge is a finding, not a fake TSan proof.
If no thread API appears anywhere, return [].
"""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import re

from helix import laws
from helix.cparse import strip_comments_keep_lines
from helix.models import Finding, FunctionInfo

_THREAD_API = re.compile(
    r"\b(?:pthread_create|std::jthread|std::thread|CreateThread|thrd_create)\b"
)
_MUTEX = re.compile(r"\b(?:mtx_lock|mtx_timedlock|pthread_mutex)\b")
_GLOBAL_DECL = re.compile(
    r"^(?:\s*(?:static|extern|const|volatile|unsigned|signed|short|long)\s+)*"
    r"(?:(?:struct|enum|union)\s+\w+\s+)?"
    r"(?:\w+\s+)+(?P<name>[A-Za-z_]\w*)\s*(?:=\s*[^;]+)?;",
    re.M,
)


def _file_globals(text: str) -> set[str]:
    depth = 0
    globals_: set[str] = set()
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            depth += stripped.count("{") - stripped.count("}")
            continue
        if depth == 0:
            m = _GLOBAL_DECL.match(line)
            if m:
                globals_.add(m.group("name"))
        depth += stripped.count("{") - stripped.count("}")
    return globals_


def _writes_global(body: str, name: str) -> bool:
    pat = re.compile(rf"\b{re.escape(name)}\s*(?:[\[\(]|(?:[+\-*/%&|^]?=))")
    return bool(pat.search(body))


def _has_mutex(body: str) -> bool:
    return bool(_MUTEX.search(body))


def _read_tu_text(functions: list[FunctionInfo]) -> str:
    if not functions:
        return ""
    names = []
    for fn in functions:
        p = Path(fn.file)
        names.extend([
            p,
            Path.cwd() / p,
            Path(__file__).resolve().parents[1] / p,
            Path(__file__).resolve().parents[1] / "testdata" / p.name,
        ])
    for p in names:
        if p.is_file():
            try:
                return strip_comments_keep_lines(
                    p.read_text(encoding="utf-8", errors="replace")
                )
            except OSError:
                continue
    return "\n".join(fn.body for fn in functions)


def _tu_has_thread_api(functions: list[FunctionInfo], file_text: str) -> bool:
    if _THREAD_API.search(file_text):
        return True
    return any(_THREAD_API.search(fn.body) for fn in functions)


def run_thread(functions: list[FunctionInfo]) -> list[Finding]:
    if not functions:
        return []

    by_file: dict[str, list[FunctionInfo]] = defaultdict(list)
    for fn in functions:
        by_file[fn.file].append(fn)

    out: list[Finding] = []
    for rel, fns in by_file.items():
        file_text = _read_tu_text(fns)
        if not _tu_has_thread_api(fns, file_text):
            continue

        globals_ = _file_globals(file_text)
        if not globals_:
            continue

        writers: dict[str, list[FunctionInfo]] = defaultdict(list)
        for fn in fns:
            for g in globals_:
                if _writes_global(fn.body, g):
                    writers[g].append(fn)

        for g, who in writers.items():
            unsync = [fn for fn in who if not _has_mutex(fn.body)]
            if len(unsync) < 2:
                continue
            anchor = unsync[0]
            out.append(
                Finding(
                    stage="thread",
                    status=laws.FAILED,
                    file=rel,
                    function=anchor.name,
                    line=anchor.line,
                    cls="RACE-SHARED",
                    message=(
                        f"global '{g}' written from {len(unsync)} functions "
                        "without mtx_lock/pthread_mutex"
                    ),
                    strength=laws.STRENGTH_FINDS,
                    evidence=g,
                    extra={"global": g, "writers": [fn.name for fn in unsync]},
                )
            )
    return out
