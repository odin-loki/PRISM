"""Differential testing: two SCALAR functions, same byte inputs, disagree = FAILED."""

from __future__ import annotations

from typing import Any

from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile

from prism import laws
from prism.contracts import parse_comments
from prism.fuzz import C_TYPE_SIZE, param_nbytes
from prism.models import Finding, FunctionInfo


def run_diff(functions: list[FunctionInfo], root: Path) -> list[Finding]:
    """Pair `*_a`/`*_b` (names or files) or `// diff: othername`; run the same bytes."""
    root = Path(root)
    pairs = _pairs(functions)
    if not pairs:
        return []
    out: list[Finding] = []
    for a, b in pairs:
        out.append(_diff_pair(a, b, root))
    return out


def _stem(fn: FunctionInfo) -> str:
    return Path(fn.file).stem if fn.file else ""


def _pairs(functions: list[FunctionInfo]) -> list[tuple[FunctionInfo, FunctionInfo]]:
    by_name: dict[str, list[FunctionInfo]] = {}
    for fn in functions:
        by_name.setdefault(fn.name, []).append(fn)
    used: set[int] = set()
    pairs: list[tuple[FunctionInfo, FunctionInfo]] = []

    def mark(a: FunctionInfo, b: FunctionInfo) -> None:
        used.add(id(a))
        used.add(id(b))
        pairs.append((a, b))

    # Function names foo_a / foo_b
    names = {fn.name: fn for fn in functions}
    for fn in functions:
        if id(fn) in used:
            continue
        if fn.name.endswith("_a"):
            other = names.get(fn.name[:-2] + "_b")
            if other is not None and id(other) not in used:
                mark(fn, other)

    # Header `// diff: othername`
    for fn in functions:
        if id(fn) in used:
            continue
        other_name = parse_comments(fn).get("diff")
        if not other_name:
            continue
        other = names.get(str(other_name))
        if other is not None and id(other) not in used:
            mark(fn, other)

    # Same function name in files *_a.c / *_b.c
    for group in by_name.values():
        a_fns = [f for f in group if id(f) not in used and _stem(f).endswith("_a")]
        b_fns = [f for f in group if id(f) not in used and _stem(f).endswith("_b")]
        for fa, fb in zip(a_fns, b_fns):
            mark(fa, fb)

    return pairs


def _diff_pair(a: FunctionInfo, b: FunctionInfo, root: Path) -> Finding:
    base: dict[str, Any] = dict(
        stage="diff", file=a.file, function=f"{a.name}/{b.name}",
        line=a.line, strength=laws.STRENGTH_FINDS,
    )
    if a.kind != "SCALAR" or b.kind != "SCALAR":
        return Finding(
            **base, status=laws.NEEDS_HARNESS, cls="",
            message=(
                f"differential testing needs two SCALAR functions, got {a.kind}/{b.kind}; "
                "POINTER/OTHER would invent a buffer or object"
            ),
        )
    if [(t, n) for t, n in a.params] != [(t, n) for t, n in b.params]:
        return Finding(
            **base, status=laws.ERROR, cls="",
            message="parameter lists differ; same bytes would not mean the same arguments",
        )

    src = _emit_program(a, b)
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return Finding(**base, status=laws.NOTRUN, cls="", message="no gcc/clang on PATH",
                       extra={"install": "install gcc or clang"})

    nbytes = param_nbytes(a.params)
    with tempfile.TemporaryDirectory(prefix="prism_diff_") as td:
        td_path = Path(td)
        harness = td_path / "diff.c"
        exe = td_path / "diff.exe"
        harness.write_text(src, encoding="utf-8")
        ok, err = _compile(cc, harness, exe)
        if not ok:
            return Finding(**base, status=laws.ERROR, cls="", message=f"diff compile: {err[:400]}")
        timed_out = False
        for data in _inputs(nbytes):
            st, detail = _run(exe, data)
            if st == "disagree":
                return Finding(
                    **base, status=laws.FAILED, cls="FUNC-CONTRACT",
                    message=f"{a.name} and {b.name} disagree",
                    evidence=detail[:800],
                    counterexample=data.hex(),
                    extra={"a": a.file, "b": b.file},
                )
            if st == "crash":
                return Finding(
                    **base, status=laws.CRASH, cls="FUZZ-CRASH",
                    message=f"diff harness crashed on {data[:16].hex()}",
                    evidence=detail[:800],
                    counterexample=data.hex(),
                )
            if st == "timeout":
                timed_out = True
        if timed_out:
            return Finding(
                **base, status=laws.TIMEOUT, cls="",
                message="diff harness timed out (not agreement, not a proof)",
                extra={"a": a.file, "b": b.file},
            )
    return Finding(
        **base, status=laws.CLEAN, cls="",
        message="no disagreement on sampled inputs (not a proof)",
        extra={"a": a.file, "b": b.file},
    )


def _emit_program(a: FunctionInfo, b: FunctionInfo) -> str:
    args = ", ".join(f"{t} {n}" if t else f"int {n}" for t, n in a.params)
    call = ", ".join(n for _, n in a.params)
    reads = []
    decls = []
    off = 0
    for typ, name in a.params:
        key = " ".join(typ.split()) or "int"
        sz = C_TYPE_SIZE.get(key, 4)
        decls.append(f"    {key} {name};")
        reads.append(f"    memcpy(&{name}, buf + {off}, {sz});")
        off += sz
    nbytes = off or 1
    ret_a = a.return_type.replace("*", "").strip() or "int"
    ret_b = b.return_type.replace("*", "").strip() or "int"
    return f'''#include <stdint.h>
#include <stdio.h>
#include <string.h>

static {ret_a} impl_a({args}) {{
{a.body}
}}
static {ret_b} impl_b({args}) {{
{b.body}
}}

int main(void) {{
    unsigned char buf[{nbytes}];
    if (fread(buf, 1, {nbytes}, stdin) != {nbytes}) return 0;
{chr(10).join(decls)}
{chr(10).join(reads)}
    {ret_a} ra = impl_a({call});
    {ret_b} rb = impl_b({call});
    if (ra != rb) {{
        fprintf(stderr, "DIFF %d %d\\n", (int)ra, (int)rb);
        return 2;
    }}
    return 0;
}}
'''


def _compile(cc: str, src: Path, exe: Path) -> tuple[bool, str]:
    cmd = [cc, "-O0", "-g", "-std=c11", str(src), "-o", str(exe)]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return False, "compile timeout"
    if p.returncode != 0:
        return False, (p.stderr or p.stdout)[-1500:]
    return True, ""


def _run(exe: Path, data: bytes, timeout: float = 1.0) -> tuple[str, str]:
    try:
        p = subprocess.run([str(exe)], input=data, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    err = (p.stderr or b"").decode("utf-8", "replace")
    if p.returncode == 2 or err.startswith("DIFF"):
        return "disagree", err[-800:]
    if p.returncode < 0:
        return "crash", f"signal {-p.returncode}"
    return "ok", err[-200:]


def _inputs(nbytes: int) -> list[bytes]:
    interesting = [
        0, 1, -1, 2, 3, 42, 99, 100, 127, 128, 255,
        0x7FFFFFFF, -0x80000000, 123456, -99,
    ]
    out: list[bytes] = []
    for v in interesting:
        raw = struct.pack("<I", v & 0xFFFFFFFF) if nbytes >= 4 else bytes([v & 0xFF])
        out.append(raw[:nbytes].ljust(nbytes, b"\x00"))
    out.append(os.urandom(nbytes))
    return out
