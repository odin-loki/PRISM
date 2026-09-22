"""Conservative intra-function source→sink taint on C text.

CodeQL-shaped lint, not a proof. POINTER functions are in scope.
No flow → skip the function; never emit CLEAN-as-proof.
"""

from __future__ import annotations

import re

from prism import laws
from prism.models import Finding, FunctionInfo

_SOURCES = frozenset({
    "getenv", "fgets", "gets", "scanf", "recv", "read", "fread", "recvfrom",
})
_SINKS = frozenset({
    "system", "popen", "execl", "execv", "strcpy", "sprintf", "memcpy",
    "printf", "fprintf", "strcat", "strncat",
})

_ASSIGN = re.compile(
    r"(?P<lhs>[A-Za-z_]\w*(?:\s*\[[^\]]*\])?)\s*=\s*(?P<rhs>[^;]+)",
)
_SIMPLE_COPY = re.compile(
    r"(?P<dst>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)*(?P<src>[A-Za-z_]\w*)\s*(?:;|$)",
)
_IDENT = re.compile(r"\b([A-Za-z_]\w*)\b")
_STRING = re.compile(r'"([^"\\]|\\.)*"')


def _split_args(argtext: str) -> list[str]:
    args: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in argtext:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def _sink_arg_indices(name: str) -> tuple[int, ...]:
    if name in {"system", "popen", "execl", "execv", "printf"}:
        return (0,)
    if name in {"sprintf", "fprintf"}:
        return (1,)
    if name in {"strcpy", "memcpy", "strcat", "strncat"}:
        return (0, 1)
    return (0,)


def _arg_is_tainted(arg: str, tainted: set[str]) -> bool:
    stripped = _STRING.sub("", arg)
    ids = {m.group(1) for m in _IDENT.finditer(stripped)}
    return bool(ids & tainted)


def _mark_source_taint(stmt: str, tainted: set[str]) -> None:
    for m in _ASSIGN.finditer(stmt):
        lhs = m.group("lhs").split("[", 1)[0].strip()
        rhs = m.group("rhs")
        if any(re.search(rf"\b{re.escape(src)}\s*\(", rhs) for src in _SOURCES):
            tainted.add(lhs)

    for src in _SOURCES:
        for m in re.finditer(rf"\b{re.escape(src)}\s*\((?P<args>[^)]*)\)", stmt):
            args = _split_args(m.group("args"))
            if not args:
                continue
            if src == "read" and len(args) >= 2:
                tainted.add(args[1].strip().lstrip("&").split("[", 1)[0])
            elif src == "recv" and len(args) >= 2:
                tainted.add(args[1].strip().lstrip("&").split("[", 1)[0])
            elif src == "recvfrom" and len(args) >= 2:
                tainted.add(args[1].strip().lstrip("&").split("[", 1)[0])
            elif src == "fread":
                tainted.add(args[0].strip().lstrip("&").split("[", 1)[0])
            elif src in {"fgets", "gets"}:
                tainted.add(args[0].strip().lstrip("&").split("[", 1)[0])
            elif src == "scanf":
                for arg in args[1:]:
                    tainted.add(arg.strip().lstrip("&*").split("[", 1)[0])


def _mark_copy_taint(stmt: str, tainted: set[str]) -> None:
    m = _SIMPLE_COPY.search(stmt)
    if not m:
        return
    src, dst = m.group("src"), m.group("dst")
    if src in tainted:
        tainted.add(dst)


def _check_sinks(
    stmt: str,
    fn: FunctionInfo,
    line: int,
    tainted: set[str],
) -> Finding | None:
    for m in re.finditer(
        r"\b(?P<name>system|popen|execl|execv|strcpy|sprintf|memcpy|strcat|strncat)\s*\((?P<args>[^)]*)\)",
        stmt,
    ):
        name = m.group("name")
        args = _split_args(m.group("args"))
        for idx in _sink_arg_indices(name):
            if idx >= len(args):
                continue
            if _arg_is_tainted(args[idx], tainted):
                return Finding(
                    stage="taint",
                    status=laws.FAILED,
                    file=fn.file,
                    function=fn.name,
                    line=line,
                    cls="TAINT-SINK",
                    message=f"tainted data reaches {name}()",
                    strength=laws.STRENGTH_FINDS,
                    evidence=stmt.strip(),
                )
    return None


def _iter_statements(body: str, base_line: int) -> list[tuple[int, str]]:
    lines = body.splitlines()
    out: list[tuple[int, str]] = []
    buf: list[str] = []
    start = 0
    for i, raw in enumerate(lines):
        if not buf:
            start = i
        buf.append(raw)
        if ";" in raw or raw.strip().endswith("}"):
            stmt = " ".join(buf).strip()
            if stmt:
                out.append((base_line + start + 1, stmt))
            buf = []
    tail = " ".join(buf).strip()
    if tail:
        out.append((base_line + start + 1, tail))
    return out


def _analyze_function(fn: FunctionInfo) -> Finding | None:
    tainted: set[str] = set()
    for _, name in fn.params:
        if name in {"argv", "envp"}:
            tainted.add(name)

    for line_no, stmt in _iter_statements(fn.body, fn.line):
        _mark_source_taint(stmt, tainted)
        _mark_copy_taint(stmt, tainted)
        hit = _check_sinks(stmt, fn, line_no, tainted)
        if hit:
            return hit
    return None


def run_taint(functions: list[FunctionInfo]) -> list[Finding]:
    out: list[Finding] = []
    for fn in functions:
        hit = _analyze_function(fn)
        if hit:
            out.append(hit)
    return out
