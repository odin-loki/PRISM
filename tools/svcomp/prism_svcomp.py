#!/usr/bin/env python3
"""PRISM as an SV-COMP verifier: run PRISM on one task, map report.json to an
SV-COMP answer, replay the counterexample and write a witness (a violation
witness for ``false``, a correctness witness for ``true``).

Roadmap 6.3. This is the executable the BenchExec tool-info module
(``tools/svcomp/prism.py``) runs:

    prism_svcomp.py [--allow-exec] [--prism BIN] [--data-model ILP32|LP64]
                    --prop PROPERTY.prp TASK.c

It runs exactly

    prism TASK.c --no-llm --stage inventory,classify,bmc,pir --out DIR

and prints one line ``PRISM-SVCOMP-RESULT: <answer>`` where answer is
``true``, ``false(no-overflow)``, ``false(unreach-call)``,
``false(valid-deref)``, ``false(valid-free)``, ``unknown`` or ``error``,
followed by ``PRISM-SVCOMP-REASON: ...``.

The mapping keeps PRISM's laws (docs/VERDICTS.md):

- ``true`` only from ``PROVED``, ``PROVED-UNBOUNDED`` or ``PROVED-CERTIFIED``
  of ``main`` by a verdict stage that covers the property (``bmc``/``pir``
  for no-overflow and unreach-call; nothing covers the whole of
  valid-memsafety, so it is never ``true``), and only when no verdict stage
  reported a ``FAILED`` of the property's class. ``PROVED-ASSUMING`` and
  ``BOUNDED`` are never ``true`` (Law 2).
- ``false(...)`` only from a ``FAILED`` of the property's class whose
  counterexample **replays**: the program is compiled with the matching
  sanitizer and run (in bubblewrap when available) on the counterexample's
  nondet values, and the sanitizer (or ``reach_error``) must fire. Replay
  executes task code, so it needs ``--allow-exec`` (Law 9); without it every
  refutation is ``unknown``.
- Everything else is ``unknown``: ``NEEDS-HARNESS``, ``BOUNDED``,
  ``UNKNOWN``, ``TIMEOUT``, ``ERROR``, a refutation that does not replay, a
  refutation of a program with ``__VERIFIER_nondet_*`` inputs whose stage did
  not report the nondet values (``bmc`` and ``pir`` report them for main in
  ``extra["nondet"]``, and each call's source position in
  ``extra["nondet_loc"]``), a stage that
  disagrees with another, and every unsupported property.

Standard library only (it ships in an SV-COMP archive next to witness.py).
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


def _load_witness_module() -> Any:
    # Loaded by path, not through sys.path: this directory also holds the
    # tool-info module ``prism.py``, which must never shadow the ``prism``
    # package (the Python engine) in the importing process.
    name = "prism_svcomp_witness"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parent / "witness.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


W = _load_witness_module()

WRAPPER_VERSION = "0.1.0"
STAGES = "inventory,classify,bmc,pir"
VERDICT_STAGES = ("bmc", "pir")
PROOF_TRUE = {"PROVED", "PROVED-UNBOUNDED", "PROVED-CERTIFIED"}  # never PROVED-ASSUMING / BOUNDED
RESULT_PREFIX = "PRISM-SVCOMP-RESULT: "
REASON_PREFIX = "PRISM-SVCOMP-REASON: "

# property -> which stages' proofs of main cover it, which finding classes refute it
PROPERTIES: dict[str, dict[str, Any]] = {
    # A signed left shift whose result is not representable is an overflow
    # under the SV-COMP rules ("the resulting type of an operation is a
    # signed-integer type but the resulting value is not in the range ...",
    # C11 6.5.7p4); a negative or too large shift count, or a negative left
    # operand, is not. `refute_props` names the INT-SHIFT-UB checks that can
    # be such an overflow (pir: shift-base, which also covers a negative
    # base; bmc: shift31); the replay under -fsanitize=shift-base then has to
    # show "left shift of N by M places cannot be represented".
    "no-overflow": {"prove": {"bmc", "pir"}, "classes": {"INT-SIGNED-OVF", "INT-SHIFT-UB"},
                    "refute_props": {"INT-SHIFT-UB": {"shift-base", "shift31"}}},
    # Both verdict stages encode a reach_error() / __VERIFIER_error() call as
    # a FUNC-CONTRACT property "reach_error" (pir translate.cpp, bmc
    # model_call) and a canonical __VERIFIER_assert as "assert"; a proof of
    # main by either covers unreach-call.
    "unreach-call": {"prove": {"bmc", "pir"}, "classes": {"FUNC-CONTRACT"}, "props": {"reach_error", "assert"}},
    # valid-memtrack (leaks) and valid-free are not encoded by any verdict
    # stage: a proof never covers the whole property, so no `true`.
    "valid-memsafety": {"prove": set(), "classes": {"MEM-OOB-READ", "MEM-OOB-WRITE", "PTR-NULL-DEREF"}},
}
SPEC_PATTERNS = [
    ("no-overflow", re.compile(r"LTL\(\s*G\s*!\s*overflow\s*\)")),
    ("unreach-call", re.compile(r"LTL\(\s*G\s*!\s*call\(\s*reach_error\(\)\s*\)\s*\)")),
    ("valid-memsafety", re.compile(r"LTL\(\s*G\s*valid-(free|deref|memtrack)\s*\)")),
]
# Types whose width differs between ILP32 and LP64: PRISM's encoders are LP64.
WIDTH_TYPES = ["long", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t"]
NONDET_CALL = re.compile(r"\b__VERIFIER_nondet_(\w+)\s*\(")


# --------------------------------------------------------------------------- property / task


def parse_property(text: str) -> str:
    """The property name of an SV-COMP .prp file, or ``unsupported``."""
    found = {name for name, rx in SPEC_PATTERNS if rx.search(text)}
    if len(found) == 1:
        return found.pop()
    return "unsupported"


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return re.sub(r'"(\\.|[^"\\])*"', '""', src)


def _function_bodies(src: str) -> list[str]:
    """Bodies of top-level function definitions (``) {`` at brace depth 0)."""
    bodies: list[str] = []
    depth, start = 0, -1
    for i, ch in enumerate(src):
        if ch == "{":
            if depth == 0 and re.search(r"\)\s*$", src[max(0, i - 200):i]):
                start = i
            depth += 1
        elif ch == "}":
            depth = max(0, depth - 1)
            if depth == 0 and start >= 0:
                bodies.append(src[start:i + 1])
                start = -1
    return bodies


def _word_re(words: list[str]) -> re.Pattern[str]:
    return re.compile(r"\b(" + "|".join(re.escape(w) for w in words) + r")\b")


def width_dependent_code(source: str) -> bool:
    """Does a function body use a type whose width differs between ILP32 and
    LP64 (``long``, ``size_t``, ``sizeof``, ... or a typedef / struct built
    from one)? Unused declarations from preprocessed headers do not count."""
    src = _strip_comments(source)
    # glibc's assert() expands to `(void) sizeof (cond)`: its value is discarded.
    src = re.sub(r"\(\s*void\s*\)\s*sizeof\b", "(void)", src)
    names: set[str] = set()
    typedefs = re.findall(r"\btypedef\b([^;]*?)\b(\w+)\s*(?:\[[^\]]*\])?\s*;", src, flags=re.S)
    aggregates = re.findall(r"\b(?:struct|union)\s+(\w+)\s*\{([^{}]*)\}", src, flags=re.S)
    changed = True
    while changed:
        changed = False
        pat = _word_re([*WIDTH_TYPES, *sorted(names)])
        for body, name in typedefs:
            if name not in names and pat.search(body):
                names.add(name)
                changed = True
        for tag, body in aggregates:
            if tag not in names and pat.search(body):
                names.add(tag)
                changed = True
    pat = _word_re([*WIDTH_TYPES, "sizeof", *sorted(names)])
    return any(pat.search(b) for b in _function_bodies(src))


# --------------------------------------------------------------------------- decision


@dataclass
class Decision:
    answer: str  # true | false(<prop>) | unknown | error
    reason: str
    finding: dict[str, Any] | None = None
    replay: dict[str, Any] = field(default_factory=dict)


def main_findings(report: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    """Findings about ``main`` per verdict stage (plus stage crash rows)."""
    per: dict[str, list[dict[str, Any]]] = {}
    for st in report.get("stages", []):
        name = st.get("name")
        if name not in VERDICT_STAGES:
            continue
        rows = [f for f in st.get("findings", []) if f.get("function") == "main"]
        if st.get("status") == "failed":
            rows.append({"status": "STAGE-FAILED", "cls": "", "message": str(st.get("detail", ""))[:300]})
        per[name] = rows
    return per


def finding_prop(f: dict[str, Any]) -> str:
    """The engine's name of the violated check: ``extra["prop"]`` (pir), else
    the message prefix (bmc: ``shift31: INT-SHIFT-UB``).

    >>> finding_prop({"message": "shift31: INT-SHIFT-UB"}), finding_prop({"extra": {"prop": "shift-base"}})
    ('shift31', 'shift-base')
    """
    extra = f.get("extra") or {}
    if extra.get("prop"):
        return str(extra["prop"])
    head, sep, _ = str(f.get("message", "")).partition(":")
    return head.strip() if sep else ""


def refutes(f: dict[str, Any], prop: str) -> bool:
    spec = PROPERTIES[prop]
    if f.get("status") != "FAILED" or f.get("cls") not in spec["classes"]:
        return False
    if "props" in spec:
        return finding_prop(f) in spec["props"]
    only = spec.get("refute_props", {}).get(f.get("cls"))
    if only is not None:
        return finding_prop(f) in only
    return True


def decide(report: dict[str, Any], prop: str, replay_fn: Any) -> Decision:
    """Map a PRISM report.json to an SV-COMP answer for ``prop``.

    ``replay_fn(finding) -> dict`` replays one refutation; its ``replay`` key
    must be ``replayed`` for the refutation to count.
    """
    if prop not in PROPERTIES:
        return Decision("unknown", f"property {prop} is not supported")
    per = main_findings(report)
    if not any(per.values()):
        return Decision("unknown", "no verdict stage reported main")
    refutations = [(st, f) for st, rows in per.items() for f in rows if refutes(f, prop)]
    # Replay first a refutation that also gives the nondet calls' source
    # positions (its witness can place every function_return waypoint), pir's
    # before bmc's (pir's trace stops at the violated check itself).
    refutations.sort(key=lambda sf: ("nondet_loc" not in (sf[1].get("extra") or {}), sf[0] != "pir"))
    proofs = [(st, f) for st, rows in per.items() for f in rows
              if f.get("status") in PROOF_TRUE and st in PROPERTIES[prop]["prove"]]
    last_replay: dict[str, Any] = {}
    for st, f in refutations:
        rp = replay_fn(f)
        last_replay = rp
        if rp.get("replay") == "replayed":
            answer = f"false({rp.get('subproperty') or prop})"
            why = f"{st}: FAILED {f.get('cls')} ({f.get('message', '')}); counterexample replayed: {rp.get('detail', '')}"
            if proofs:
                why += f"; DISAGREEMENT: {', '.join(s for s, _ in proofs)} claimed a proof"
            return Decision(answer, why, dict(f, stage=st), rp)
    if refutations:
        st, f = refutations[0]
        return Decision("unknown", f"{st}: FAILED {f.get('cls')} but the counterexample did not replay "
                        f"({last_replay.get('replay')}: {last_replay.get('why', last_replay.get('detail', ''))})",
                        dict(f, stage=st), last_replay)
    if proofs:
        st, f = proofs[0]
        return Decision("true", f"{st}: {f.get('status')} of main ({f.get('message', '')})", dict(f, stage=st))
    summary = "; ".join(f"{st}: {', '.join(sorted({str(f.get('status')) for f in rows})) or 'none'}"
                        for st, rows in per.items())
    return Decision("unknown", f"no proof covering {prop} and no replayed refutation ({summary})")


# --------------------------------------------------------------------------- replay


def which_cc() -> str | None:
    for cc in ("clang", "gcc", "cc"):
        if shutil.which(cc):
            return shutil.which(cc)
    return None


def bwrap_prefix(work: Path) -> list[str]:
    """Read-only root, private /tmp (with the replay directory bound back
    read-only), no network, no shared namespaces; [] when bwrap is unusable."""
    bw = shutil.which("bwrap")
    if not bw:
        return []
    w = str(work.resolve())
    cmd = [bw, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
           "--ro-bind", w, w, "--unshare-all", "--die-with-parent", "--"]
    try:
        r = subprocess.run(cmd + ["true"], capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return cmd if r.returncode == 0 else []


def _defines(source: str, name: str) -> bool:
    return re.search(r"\b" + re.escape(name) + r"\s*\([^;{)]*\)\s*\{", source) is not None


NONDET_TYPES = {
    "int": "int", "uint": "unsigned int", "unsigned_int": "unsigned int", "u32": "unsigned int",
    "long": "long", "ulong": "unsigned long", "longlong": "long long", "ulonglong": "unsigned long long",
    "short": "short", "ushort": "unsigned short", "char": "char", "uchar": "unsigned char",
    "bool": "_Bool", "_Bool": "_Bool", "float": "float", "double": "double", "size_t": "unsigned long",
    "loff_t": "long long", "u8": "unsigned char", "u16": "unsigned short", "sector_t": "unsigned long",
}


def stub_source(source: str, nondet_values: list[int | float]) -> str:
    """Definitions for the __VERIFIER_* functions the task uses but does not
    define; nondet functions return the counterexample's values in order."""
    vals = ", ".join(repr(float(v)) for v in nondet_values) or "0.0"
    out = ["#include <stdio.h>", "#include <stdlib.h>", "#include <unistd.h>",
           f"static const double prism_nondet[] = {{{vals}}};",
           f"static unsigned prism_next, prism_count = {len(nondet_values)};",
           "static double prism_take(void) { if (prism_next < prism_count) return prism_nondet[prism_next++];",
           '  fprintf(stderr, "PRISM-NONDET-EXHAUSTED\\n"); return 0; }']
    for suffix in sorted(set(NONDET_CALL.findall(source))):
        name = f"__VERIFIER_nondet_{suffix}"
        if _defines(source, name):
            continue
        ctype = NONDET_TYPES.get(suffix)
        if ctype is None:
            raise ValueError(f"unsupported nondet type {name}")
        out.append(f"{ctype} {name}(void) {{ return ({ctype})prism_take(); }}")
    if "__VERIFIER_assume" in source and not _defines(source, "__VERIFIER_assume"):
        out.append('void __VERIFIER_assume(int c) { if (!c) { fprintf(stderr, "PRISM-ASSUME-FAILED\\n"); _exit(0); } }')
    if "reach_error" in source and not _defines(source, "reach_error"):
        out.append('void reach_error(void) { fprintf(stderr, "PRISM-REACH-ERROR reach_error\\n"); abort(); }')
    return "\n".join(out) + "\n"


def _rlimits(timeout: float) -> Any:
    def apply() -> None:
        import resource
        cpu = int(timeout) + 1
        resource.setrlimit(resource.RLIMIT_CPU, (cpu, cpu))
        resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 24, 1 << 24))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    return apply


UBSAN_RE = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): runtime error: (?P<msg>.*)$", re.M)
ASAN_RE = re.compile(r"ERROR: AddressSanitizer: (?P<kind>[\w-]+)")
ASAN_FRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in \S+ (?P<file>[^\s:]+):(?P<line>\d+):(?P<col>\d+)")
OVERFLOW_MSG = re.compile(r"^(signed integer overflow|negation of .* cannot be represented|division of .* cannot be represented"
                          r"|left shift of \d+ by \d+ places cannot be represented)")
ASAN_DEREF = {"heap-buffer-overflow", "stack-buffer-overflow", "global-buffer-overflow", "SEGV",
              "heap-use-after-free", "stack-use-after-return", "stack-use-after-scope", "stack-buffer-underflow"}
ASAN_FREE = {"attempting", "bad-free", "double-free"}


def nondet_trace(f: dict[str, Any]) -> list[tuple[str, int | float]] | None:
    """The ``__VERIFIER_nondet_*`` values a refutation of main reads, in call
    order, from the engine's ``extra["nondet"]`` ("fn=value, ...", reported by
    ``bmc`` and ``pir``); None when the engine did not report them (then the
    refutation cannot be replayed). Only a finding of main is a whole-program
    trace.

    >>> nondet_trace({"function": "main", "extra": {"nondet": "__VERIFIER_nondet_int=-5, __VERIFIER_nondet_float=0.5"}})
    [('__VERIFIER_nondet_int', -5), ('__VERIFIER_nondet_float', 0.5)]
    """
    extra = f.get("extra") or {}
    if f.get("function") != "main" or "nondet" not in extra:
        return None
    out: list[tuple[str, int | float]] = []
    for part in str(extra["nondet"]).split(","):
        part = part.strip()
        if not part:
            continue
        name, _, val = part.partition("=")
        val = val.strip()
        try:
            out.append((name.strip(), int(val, 0)))
        except ValueError:
            try:
                out.append((name.strip(), float(val)))
            except ValueError:
                return None
    return out


def nondet_locations(f: dict[str, Any], count: int) -> list[tuple[int, int] | None] | None:
    """Source (line, column) of each call in the nondet trace, from
    ``extra["nondet_loc"]`` ("line:col, ...", ``pir`` reports it from debug
    info; 0:0 means unknown, given as None). None when absent or when it does
    not match the trace entry for entry.

    >>> nondet_locations({"extra": {"nondet_loc": "6:13, 0:0"}}, 2)
    [(6, 13), None]
    >>> nondet_locations({"extra": {"nondet_loc": "6:13"}}, 2) is None
    True
    """
    raw = (f.get("extra") or {}).get("nondet_loc")
    if raw is None:
        return None
    out: list[tuple[int, int] | None] = []
    for part in str(raw).split(","):
        part = part.strip()
        if not part:
            continue
        m = re.fullmatch(r"(\d+):(\d+)", part)
        if not m:
            return None
        line, col = int(m.group(1)), int(m.group(2))
        out.append((line, col) if line > 0 and col > 0 else None)
    return out if len(out) == count else None


def _is_declaration(text: str, start: int) -> bool:
    """``int f()`` / ``extern unsigned f()``: a type name right before the
    identifier (a call follows ``=``, ``(``, ``,``, an operator or ``return``)."""
    before = text[max(0, text.rfind("\n", 0, start) + 1):start].rstrip()
    m = re.search(r"(\w+)\s*\**$", before)
    return m is not None and m.group(1) != "return" and not before.endswith(("=", "(", ","))


# `# 12 "file.c"` / `#line 12`: debug lines then name another file's lines
LINE_MARKER = re.compile(r"^[ \t]*#[ \t]*(line[ \t]+)?\d+", re.M)


def _paren_location(text: str, end: int) -> tuple[int, int]:
    """(line, column) of the character just before offset ``end`` (the call's ``)``)."""
    line = text.count("\n", 0, end) + 1
    return line, end - (text.rfind("\n", 0, end) + 1)


def _call_at(text: str, name: str, line: int, col: int) -> tuple[int, int] | None:
    """(line, column) of the ``)`` of a call of ``name`` that starts exactly at
    line:col (1-based), or None when the text has no such call there.

    >>> _call_at("int x;\\n  y = f ( );\\n", "f", 2, 7)
    (2, 11)
    >>> _call_at("int x;\\n  y = gf();\\n", "f", 2, 8) is None
    True
    """
    lines = text.split("\n")
    if not 1 <= line <= len(lines) or col < 1 or col > len(lines[line - 1]):
        return None
    start = sum(len(ln) + 1 for ln in lines[:line - 1]) + col - 1
    if start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
        return None
    m = re.compile(rf"{re.escape(name)}\s*\(\s*\)").match(text, start)
    return None if m is None else _paren_location(text, m.end())


def nondet_waypoints(task: Path, trace: list[tuple[str, int | float]],
                     locs: list[tuple[int, int] | None] | None = None, physical: bool = False) -> list[Any]:
    """function_return waypoints for the longest prefix of ``trace`` whose
    calls can each be placed exactly: at the engine's debug location of the
    call (``locs``, from ``pir``) when the task text really has that call
    there, else at the call's only call site in the task. A call that is
    neither ends the prefix: a waypoint at the wrong call would make the
    witness wrong, a missing one only weaker. Debug locations (pir) are
    ignored in a task with line markers (they then name another file's
    lines); ``physical`` locations (bmc: positions in the analysed text) are
    not."""
    text = task.read_text(encoding="utf-8", errors="replace")
    if locs is not None and (len(locs) != len(trace) or (not physical and LINE_MARKER.search(text))):
        locs = None
    out: list[Any] = []  # witness.NondetValue
    for i, (name, value) in enumerate(trace):
        at = locs[i] if locs is not None else None
        pos = _call_at(text, name, *at) if at is not None else None
        if pos is None:
            sites = [m for m in re.finditer(rf"\b{re.escape(name)}\s*\(\s*\)", text)
                     if not _is_declaration(text, m.start())]
            if len(sites) != 1:
                break
            # just past ')': format 2.0 points at the closing parenthesis
            pos = _paren_location(text, sites[0].end())
        out.append(W.NondetValue(W.Location(task.name, pos[0], pos[1]), value))
    return out


def replay(src: Path, prop: str, *, allow_exec: bool, nondet_values: list[int | float] | None,
           work: Path, timeout: float = 10.0) -> dict[str, Any]:
    """Compile and run the task on the counterexample; the violation must show."""
    text = src.read_text(encoding="utf-8", errors="replace")
    if NONDET_CALL.search(text) and nondet_values is None:
        return {"replay": "unsupported",
                "why": "program reads __VERIFIER_nondet_* inputs and the engine reported no nondet values"}
    if not allow_exec:
        return {"replay": "notrun", "why": "replay executes task code: re-run with --allow-exec (Law 9)"}
    cc = which_cc()
    if cc is None:
        return {"replay": "notrun", "why": "no C compiler (clang/gcc) on PATH"}
    # shift-base: `x << n` whose result is not representable, and a negative
    # x (a report OVERFLOW_MSG does not accept: not an overflow).
    san = {"no-overflow": "signed-integer-overflow,shift-base", "valid-memsafety": "address"}.get(prop)
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / "prism_stubs.c"
    try:
        stubs.write_text(stub_source(text, nondet_values or []), encoding="utf-8")
    except ValueError as e:
        return {"replay": "unsupported", "why": str(e)}
    csrc = work / (src.stem + ".c")
    shutil.copyfile(src, csrc)
    exe = work / "replay.bin"
    flags = ["-g", "-O0", "-w", "-std=gnu11"]
    if san:
        flags += [f"-fsanitize={san}", "-fno-sanitize-recover=all"]
    r = subprocess.run([cc, *flags, str(csrc), str(stubs), "-o", str(exe)], capture_output=True, text=True,
                       timeout=120)
    if r.returncode != 0:
        return {"replay": "not-replayed", "why": "replay build failed: " + (r.stderr or "")[-300:]}
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
               UBSAN_OPTIONS="print_stacktrace=0:halt_on_error=1")
    jail = bwrap_prefix(work)
    try:
        p = subprocess.run(jail + [str(exe)], capture_output=True, text=True, timeout=timeout, env=env,
                           cwd=work, stdin=subprocess.DEVNULL, preexec_fn=_rlimits(timeout))
    except subprocess.TimeoutExpired:
        return {"replay": "not-replayed", "why": f"program did not finish in {timeout}s"}
    err = p.stderr or ""
    rec: dict[str, Any] = {"exit": p.returncode, "sandbox": "bwrap+rlimits" if jail else "rlimits only (no bwrap)"}
    if prop == "no-overflow":
        for m in UBSAN_RE.finditer(err):
            if OVERFLOW_MSG.match(m.group("msg")):
                rec.update(replay="replayed", detail=f"UBSan: {m.group('msg')}",
                           line=int(m.group("line")), column=int(m.group("col")))
                return rec
    elif prop == "unreach-call":
        if "reach_error" in err and p.returncode != 0:
            # The assertion message names reach_error's own line, not the call
            # site; the witness target comes from reach_error_call_site().
            rec.update(replay="replayed", detail="reach_error() was called and the program aborted")
            return rec
    elif prop == "valid-memsafety":
        am = ASAN_RE.search(err)
        if am:
            kind = am.group("kind")
            sub = "valid-deref" if kind in ASAN_DEREF else "valid-free" if kind in ASAN_FREE else None
            if sub:
                rec.update(replay="replayed", detail=f"ASan: {kind}", subproperty=sub)
                fm = ASAN_FRAME_RE.search(err)
                if fm:
                    rec.update(line=int(fm.group("line")), column=int(fm.group("col")))
                return rec
    rec.update(replay="not-replayed", why="the program ran without the violation: " + err[-300:].strip())
    return rec


# --------------------------------------------------------------------------- prism


def find_prism(explicit: str | None) -> str | None:
    cands = [explicit, os.environ.get("PRISM_BIN")]
    here = Path(__file__).resolve().parent
    cands += [str(here / "prism"), str(here / "bin" / "prism"), str(here.parents[1] / "build" / "prism")]
    for c in cands:
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            return str(Path(c).resolve())
    return shutil.which("prism")


def _session_members(sid: int) -> list[int]:
    """PIDs whose session id is ``sid`` (Linux ``/proc``; empty elsewhere)."""
    out: list[int] = []
    try:
        names = os.listdir("/proc")
    except OSError:
        return out
    for n in names:
        if not n.isdigit():
            continue
        try:
            with open(f"/proc/{n}/stat", "rb") as fh:
                stat = fh.read().decode("latin-1")
        except OSError:
            continue
        rest = stat[stat.rfind(")") + 2:].split()  # state ppid pgrp session ...
        if len(rest) > 3 and rest[3] == str(sid):
            out.append(int(n))
    return out


def _kill_session(proc: subprocess.Popen[Any]) -> None:
    """SIGKILL every process of the session ``proc`` leads (PRISM puts its
    solver and checker children in process groups of their own, so killing
    PRISM's group alone would leave them running)."""
    if os.name != "posix":
        proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    for _ in range(3):
        left = [p for p in _session_members(proc.pid) if p != os.getpid()]
        if not left:
            break
        for p in left:
            try:
                os.kill(p, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
    try:
        proc.kill()
    except ProcessLookupError:
        pass


def run_tree(argv: list[str], timeout: float | None) -> int:
    """Run ``argv`` in a session of its own (output discarded); on a timeout,
    Ctrl-C or any other exception kill the whole session, then re-raise."""
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=os.name == "posix")
    try:
        return proc.wait(timeout=timeout)
    except BaseException:
        _kill_session(proc)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        raise


def run_prism(prism: str, task_c: Path, out: Path, timeout: float | None) -> dict[str, Any]:
    argv = [prism, str(task_c), "--no-llm", "--stage", STAGES, "--out", str(out)]
    run_tree(argv, timeout)
    rep = out / "report.json"
    if not rep.exists():
        raise RuntimeError(f"prism wrote no report.json ({' '.join(argv)})")
    return dict(json.loads(rep.read_text(encoding="utf-8")))


def reach_error_call_site(src: Path, hint: int | None) -> tuple[int, int] | None:
    """(line, column) of the reach_error() call the witness targets: the only
    call site, or the one on the finding's line; None when ambiguous."""
    sites = []
    for n, text in enumerate(src.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        for m in re.finditer(r"\breach_error\s*\(\s*\)", text):
            if re.search(r"\breach_error\s*\(\s*(void)?\s*\)\s*\{", text[m.start():]):
                continue  # the definition
            sites.append((n, m.start() + 1))
    if len(sites) == 1:
        return sites[0]
    same = [s for s in sites if s[0] == hint]
    return same[0] if len(same) == 1 else None


def first_code_column(src: Path, line: int) -> int:
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    if 1 <= line <= len(lines):
        s = lines[line - 1]
        return len(s) - len(s.lstrip()) + 1
    return 1


# `int x`, `unsigned long *p`, `struct s v[3]`: a declarator, not an lvalue like `y`, `*p`, `a[i]`
_DECL_HEAD = re.compile(r"^[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*[\s*]+[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)*$")
_CONTROL = re.compile(r"^(if|while|switch)\s*\(")


def target_column(src: Path, line: int, col: int) -> int | None:
    """Column of a violation ``target`` waypoint for a violation a sanitizer
    reported at ``line:col`` (the operator), or None to give no column.

    Format 2.0 puts the target at the first character of the statement or
    full expression whose evaluation the violation ends: an expression
    statement's start, a declaration's initializer, the controlling
    expression of ``if``/``while``/``switch``, the expression of ``return``.
    Without a column the target is "the first statement or full expression in
    that line", so a statement that is the first on its line gets no column:
    validators differ on parenthesised starts (UAutomizer 0.3.1 matches
    ``int x = (a + 1) - 2;`` at ``a`` or without a column, not at ``(``).
    A later statement on the line gets its start column (past any opening
    parentheses). Anything this cannot place on one line (``for`` headers,
    several declarators, a statement that begins on an earlier line) keeps
    the sanitizer's column.

    >>> import tempfile, os
    >>> d = tempfile.mkdtemp(); p = Path(d) / "t.c"
    >>> _ = p.write_text("int main() {\\n  y = y +2*f();\\n int x = (1 + 2) - 3;\\n  if (a + b > 0) g();\\n"
    ...                  "  return a * b;\\n  for (i = 0; i + 1 < n; i++) ;\\n  x = a +\\n    b;\\n  --x;\\n"
    ...                  "  a = 1; b = c + d;\\n  a = 1; int e = (c + d);\\n  a = 1; if (c + d) g();\\n}\\n")
    >>> [target_column(p, n, c) for n, c in [(2, 10), (3, 13), (4, 9), (5, 12), (9, 3)]]
    [None, None, None, None, None]
    >>> [target_column(p, 6, 20), target_column(p, 8, 5)]
    [20, 5]
    >>> [target_column(p, 10, 16), target_column(p, 11, 21), target_column(p, 12, 15)]
    [10, 19, 14]
    >>> [bool(_DECL_HEAD.match(h)) for h in ("int x", "unsigned int *p", "struct s v[3]", "y", "*p", "a[i]")]
    [True, True, True, False, False, False]
    """
    lines = src.read_text(encoding="utf-8", errors="replace").split("\n")
    if not 1 <= line <= len(lines):
        return col
    text = lines[line - 1]
    p = min(max(col - 1, 0), len(text))
    for fm in re.finditer(r"\bfor\s*\(", text[:p]):
        if text[fm.end() - 1:p].count("(") > text[fm.end() - 1:p].count(")"):
            return col  # inside a for header: its ';' are not statement ends
    cut = max(text.rfind(ch, 0, p) for ch in ";{}")
    if cut < 0:
        # the statement starts on this line only if the previous code line ends one
        prev = next((ln.strip() for ln in reversed(lines[:line - 1]) if ln.strip()), "")
        if prev and not prev.endswith((";", "{", "}")) and not prev.startswith("#"):
            return col
    s = cut + 1
    while s < p and text[s] in " \t":
        s += 1
    stmt = text[s:p]
    if stmt.startswith("for") and re.match(r"for\s*\(", stmt):
        return col
    if not text[:s].strip():
        return None  # the first statement on its line
    m = _CONTROL.match(stmt)
    if m:
        s += m.end()
    elif re.match(r"return\b", stmt):
        s += len("return")
    else:
        eq = re.search(r"(?<![=!<>+\-*/%&|^])=(?!=)", stmt)
        if eq and _DECL_HEAD.match(stmt[:eq.start()].strip()) and "," not in stmt[:eq.start()]:
            if "," in stmt[eq.end():]:
                return col  # several declarators, or a comma inside the initializer
            s += eq.end()
    while s < p and text[s] in " \t(":
        s += 1  # past opening parentheses: the first operand (see above)
    return s + 1


# Invariant conjuncts the witness may carry. The engine proves its
# invariants in C semantics over bit-vectors, where a signed `+`, `-` or `*`
# in the invariant text wraps; in C that is an overflow (UB), so a conjunct
# with arithmetic is exported only when the other exported conjuncts bound
# every variable in it so tightly that no subterm leaves the range of int
# (then wrapping and C agree, whatever the variables' integer types). A
# plain comparison of identifiers and constants is always exported. Leaving
# out a conjunct keeps the witness valid (each proved conjunct holds by
# itself); it only gives the validator less to work with.
_ATOM = re.compile(r"^\(?\s*([A-Za-z_]\w*|-?\d+)\s*(<=|>=|==|!=|<|>)\s*([A-Za-z_]\w*|-?\d+)\s*\)?$")
_CMP = re.compile(r"^(.*?)\s*(<=|>=|==|!=|<|>)\s*(.*)$")
_INT_MIN, _INT_MAX = -(1 << 31), (1 << 31) - 1


def _bounds(atoms: list[str]) -> dict[str, tuple[int | None, int | None]]:
    """Constant bounds of variables implied by comparison conjuncts."""
    lo: dict[str, int] = {}
    hi: dict[str, int] = {}
    rel: list[tuple[str, str, str]] = []
    for a in atoms:
        m = _ATOM.match(a)
        if not m:
            continue
        x, op, y = m.groups()
        if re.fullmatch(r"-?\d+", x) and not re.fullmatch(r"-?\d+", y):
            x, y, op = y, x, {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(op, op)
        rel.append((x, op, y))
    # Only `variable op constant`: a bound carried through `x <= y` could be
    # wrong when x and y differ in signedness (C compares them unsigned).
    for x, op, y in rel:
        if re.fullmatch(r"-?\d+", x) or not re.fullmatch(r"-?\d+", y):
            continue
        c = int(y)
        if op in ("<=", "=="):
            hi[x] = min(hi.get(x, c), c)
        if op == "<":
            hi[x] = min(hi.get(x, c - 1), c - 1)
        if op in (">=", "=="):
            lo[x] = max(lo.get(x, c), c)
        if op == ">":
            lo[x] = max(lo.get(x, c + 1), c + 1)
    return {v: (lo.get(v), hi.get(v)) for v in set(lo) | set(hi)}


def _int_safe(expr: str, bounds: dict[str, tuple[int | None, int | None]]) -> bool:
    """Every subterm of the arithmetic ``expr`` (identifiers, decimal
    constants, ``+ - *``, parentheses) stays within int for all values in
    ``bounds``.

    >>> b = {"i": (0, 1000), "s": (0, None)}
    >>> _int_safe("2 * i", b), _int_safe("i + 1", b), _int_safe("2 * s", b), _int_safe("i / 2", b)
    (True, True, False, False)
    """
    toks = re.findall(r"\d+|[A-Za-z_]\w*|[-+*()]|\S", expr)
    pos = 0

    def ok(iv: tuple[int, int]) -> tuple[int, int]:
        if iv[0] < _INT_MIN or iv[1] > _INT_MAX:
            raise ValueError
        return iv

    def atom() -> tuple[int, int]:
        nonlocal pos
        if pos >= len(toks):
            raise ValueError
        t = toks[pos]
        pos += 1
        if t == "(":
            v = add()
            if pos >= len(toks) or toks[pos] != ")":
                raise ValueError
            pos += 1
            return v
        if t == "-":
            a = atom()
            return ok((-a[1], -a[0]))
        if t.isdigit():
            return ok((int(t), int(t)))
        if re.fullmatch(r"[A-Za-z_]\w*", t):
            lo_, hi_ = bounds.get(t, (None, None))
            if lo_ is None or hi_ is None:
                raise ValueError
            return ok((lo_, hi_))
        raise ValueError

    def mul() -> tuple[int, int]:
        nonlocal pos
        v = atom()
        while pos < len(toks) and toks[pos] == "*":
            pos += 1
            w = atom()
            ps = [v[0] * w[0], v[0] * w[1], v[1] * w[0], v[1] * w[1]]
            v = ok((min(ps), max(ps)))
        return v

    def add() -> tuple[int, int]:
        nonlocal pos
        v = mul()
        while pos < len(toks) and toks[pos] in "+-":
            op = toks[pos]
            pos += 1
            w = mul()
            v = ok((v[0] + w[0], v[1] + w[1]) if op == "+" else (v[0] - w[1], v[1] - w[0]))
        return v

    try:
        add()
    except ValueError:
        return False
    return pos == len(toks)


def exportable_conjuncts(conjuncts: list[str]) -> list[str]:
    """The proved conjuncts a witness may carry (see above).

    >>> exportable_conjuncts(["i >= 0", "i <= 1000", "s == 2 * i", "s <= 2 * n", "i != s", "f(i) > 0"])
    ['i >= 0', 'i <= 1000', 's == 2 * i', 'i != s']
    """
    atoms = [c for c in conjuncts if _ATOM.match(c)]
    b = _bounds(atoms)
    out = []
    for c in conjuncts:
        if _ATOM.match(c):
            out.append(c)
            continue
        m = _CMP.match(c)
        if m and not re.search(r"[<>=!]", m.group(1) + m.group(3)) and all(
                re.fullmatch(r"[A-Za-z_]\w*|-?\d+", side.strip()) or _int_safe(side, b) for side in (m.group(1), m.group(3))):
            out.append(c)
    return out


def correctness_invariants(task: Path, finding: dict[str, Any]) -> tuple[list[Any], str]:
    """Loop invariants for a correctness witness of a ``true`` answer, and a
    note on where they came from.

    Only invariants the engine proved are exported: the Houdini-filtered
    loop invariants of a ``bmc`` ``PROVED-UNBOUNDED`` (``extra["invariants"]``,
    one list per cut loop, with the loops' source positions in
    ``extra["invariant_loops"]``). A loop whose position is unknown (inlined
    body), that is a ``do`` loop (its invariant is proved at the top of the
    body, not where the condition is evaluated), or whose keyword is not at
    that position in the task text gets none. Every other proof (``pir``
    PROVED within the unwind, k-induction without an invariant) exports
    nothing, and the witness is the empty ``invariant_set``: trivially valid,
    the validator has to find the proof itself.

    >>> import tempfile
    >>> p = Path(tempfile.mkdtemp()) / "t.c"
    >>> _ = p.write_text("int main() {\\n  int i = 0;\\n  while (i < 10) i++;\\n}\\n")
    >>> f = {"function": "main", "extra": {"k_induction": "closed-invariants",
    ...      "invariants": '[["i >= 0", "i <= 10", "i + 1 > i", "i <= 2 * n"]]',
    ...      "invariant_loops": '[{"kind": "while", "line": 3, "column": 3}]'}}
    >>> invs, note = correctness_invariants(p, f)
    >>> [(i.kind, i.location.line, i.location.column, i.value) for i in invs]
    [('loop_invariant', 3, 3, '(i >= 0) && (i <= 10) && (i + 1 > i)')]
    >>> f["extra"]["invariant_loops"] = '[{"kind": "while", "line": 2, "column": 3}]'
    >>> correctness_invariants(p, f)[0]
    []
    """
    extra = finding.get("extra") or {}
    if extra.get("k_induction") != "closed-invariants" or not ("invariants" in extra or
                                                               "invariant_conjuncts" in extra):
        return [], "no loop invariant exported by the proving stage (empty invariant set)"
    pir = "invariants" not in extra
    try:
        if not pir:
            invs = json.loads(str(extra["invariants"]))
        else:
            # pir: structured conjuncts over IR values, rendered in C here
            pir = pir_invariant_texts(task, finding)
            if pir is None:
                return [], "pir invariants not mapped to C (no debug information; empty invariant set)"
            invs = pir
        loops = json.loads(str(extra.get("invariant_loops", "[]")))
    except ValueError:
        return [], "unreadable invariants (empty invariant set)"
    lines = task.read_text(encoding="utf-8", errors="replace").split("\n")
    out: list[Any] = []
    for j, loop in enumerate(loops if isinstance(loops, list) else []):
        if j >= len(invs) or not isinstance(loop, dict) or loop.get("kind") not in ("for", "while", ""):
            continue
        line, col = int(loop.get("line") or 0), int(loop.get("column") or 0)
        if not (1 <= line <= len(lines)) or col < 1:
            continue
        at = lines[line - 1][col - 1:]
        kw = str(loop["kind"])
        if not kw:  # pir: the keyword at the loop's start position (a `do` loop gets none)
            km = re.match(r"(for|while)\b", at)
            if not km:
                continue
            kw = km.group(1)
        if not re.match(rf"{kw}\b", at) or (col > 1 and re.match(r"\w", lines[line - 1][col - 2])):
            continue
        # a name declared in the for-init is not in scope at the keyword
        decl = re.match(r"for\s*\(\s*(?:[A-Za-z_]\w*\s+)+\**\s*([A-Za-z_]\w*)\s*=", at)
        mine = [str(e).strip() for e in invs[j] if not (decl and re.search(rf"\b{decl.group(1)}\b", str(e)))]
        # pir conjuncts are rendered with their C types (_render); bmc's are filtered here
        kept = mine if pir else exportable_conjuncts(mine)
        if kept:
            out.append(W.Invariant("loop_invariant", W.Location(task.name, line, col, finding.get("function")),
                                   " && ".join(f"({e})" for e in kept)))
    n = sum(len(i.value.split(" && ")) for i in out)
    return out, f"{n} Houdini loop invariant conjunct(s) from {finding.get('stage', 'bmc')}"


# --------------------------------------------------------------------------- pir invariants -> C
#
# The pir stage exports its proved loop invariants as structured conjuncts
# over IR value names (extra["invariant_conjuncts"], see
# src/prism/pir/houdini.inc export_invariants). The C text needs the C
# variable behind each value at the loop head and its C type: the task is
# compiled once more with full debug information (-g, otherwise the same
# front end and passes as the pir stage) and the llvm.dbg.value records
# give both. A value is exported as variable X only when the mapping is
# certain at the loop head (see _current_var); a conjunct only when C's
# meaning of the rendered expression is the proved bit-vector relation
# (see _render). Anything else is left out: a subset of proved conjuncts is
# still a proved invariant.

IR_PASSES = "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer"


def _tool(*names: str) -> str | None:
    for n in names:
        if shutil.which(n):
            return shutil.which(n)
    return None


def debug_ir(task: Path) -> str | None:
    """The task's IR as the pir stage builds it, with full debug information."""
    cc, opt = _tool("clang-18", "clang"), _tool("opt-18", "opt")
    if not cc or not opt:
        return None
    try:
        r = subprocess.run([cc, "-x", "c", "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
                            "-fno-discard-value-names", "-fno-builtin-memcpy", "-g", "-std=c17", "-w", str(task),
                            "-o", "-"],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            return None
        o = subprocess.run([opt, "-S", IR_PASSES], input=r.stdout, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return o.stdout if o.returncode == 0 else None


@dataclass
class IrFunc:
    blocks: list[str] = field(default_factory=list)
    succ: dict[str, list[str]] = field(default_factory=dict)
    defs: dict[str, str] = field(default_factory=dict)          # value -> block
    phis: dict[str, set[str]] = field(default_factory=dict)     # block -> its phi values
    records: list[tuple[str, int, str, str]] = field(default_factory=list)  # (block, index, value, var md)
    loops: dict[str, list[str]] = field(default_factory=dict)   # llvm.loop md -> branch targets


_MD = re.compile(r"^(!\d+) = (?:distinct )?(.*)$")


def _md_field(body: str, key: str) -> str | None:
    m = re.search(rf"\b{key}: (\"[^\"]*\"|[^,)]+)", body)
    return m.group(1).strip('"') if m else None


def parse_debug_ir(ir: str, fn: str) -> tuple[IrFunc | None, dict[str, str]]:
    """The function's blocks, value definitions, dbg.value records and loop
    branches, and the module's metadata lines ("!N" -> body)."""
    md: dict[str, str] = {}
    for line in ir.splitlines():
        m = _MD.match(line)
        if m:
            md[m.group(1)] = m.group(2)
    lines = ir.splitlines()
    start = next((i for i, ln in enumerate(lines) if ln.startswith("define ") and f"@{fn}(" in ln), None)
    if start is None:
        return None, md
    f = IrFunc()
    cur = "entry"
    f.blocks.append(cur)
    idx = 0
    for m in re.finditer(r"%([\w.]+)", lines[start].split("(", 1)[1] if "(" in lines[start] else ""):
        f.defs[m.group(1)] = cur
    for line in lines[start + 1:]:
        if line.startswith("}"):
            break
        lab = re.match(r"^([\w.]+):", line)
        if lab:
            cur = lab.group(1)
            if cur not in f.blocks:
                f.blocks.append(cur)
            idx = 0
            continue
        t = line.strip()
        if not t or t.startswith(";"):
            continue
        idx += 1
        d = re.match(r"%([\w.]+) = (\w+)", t)
        if d:
            f.defs[d.group(1)] = cur
            if d.group(2) == "phi":
                f.phis.setdefault(cur, set()).add(d.group(1))
        r = re.search(r"@llvm\.dbg\.value\(metadata \S+ (%[\w.]+|[-\w.]+), metadata (!\d+),", t)
        if r:
            f.records.append((cur, idx, r.group(1).lstrip("%") if r.group(1).startswith("%") else "#" + r.group(1),
                              r.group(2)))
        if t.startswith(("br ", "switch ")):
            targets = re.findall(r"label %([\w.]+)", t)
            f.succ.setdefault(cur, []).extend(targets)
            lm = re.search(r"!llvm\.loop (!\d+)", t)
            if lm:
                f.loops.setdefault(lm.group(1), []).extend(targets)
    return f, md


def _scope_chain(md: dict[str, str], ref: str | None) -> list[str]:
    out: list[str] = []
    while ref and ref not in out and len(out) < 64:
        out.append(ref)
        body = md.get(ref, "")
        ref = _md_field(body, "scope") if "DILexicalBlock" in body else None
    return out


def _c_type(md: dict[str, str], ref: str | None) -> tuple[bool, int] | None:
    """(signed, bits) of an integer C type (through typedefs and qualifiers)."""
    for _ in range(16):
        body = md.get(ref or "", "")
        if body.startswith("!DIBasicType("):
            enc, size = _md_field(body, "encoding") or "", int(_md_field(body, "size") or 0)
            if enc in ("DW_ATE_signed", "DW_ATE_signed_char"):
                return True, size
            if enc in ("DW_ATE_unsigned", "DW_ATE_unsigned_char"):
                return False, size
            return None  # _Bool, floating point
        if body.startswith("!DIDerivedType(") and re.search(r"tag: DW_TAG_(typedef|const_type|volatile_type)", body):
            ref = _md_field(body, "baseType")
            continue
        return None
    return None


def _reach(f: IrFunc, src: str, stop: str) -> set[str]:
    """Blocks reachable from src's successors without passing through stop."""
    seen: set[str] = set()
    work = [s for s in f.succ.get(src, [])]
    while work:
        b = work.pop()
        if b in seen or b == stop:
            continue
        seen.add(b)
        work.extend(f.succ.get(b, []))
    return seen


def _current_var(f: IrFunc, md: dict[str, str], value: str, header: str,
                 loop_scope: list[str]) -> tuple[str, bool, int] | None:
    """The C variable whose current value at the loop head (the start of
    ``header``) is ``value``, with its C type; None unless certain:

    * the value is described by dbg.value records of exactly one variable,
      whose name no other variable of the function has, and whose scope
      encloses the loop;
    * after its record in the defining block D, no other record of that
      variable lies on a path from D to the header (the header's own phi
      records included, unless the value is that phi).
    """
    recs = [r for r in f.records if r[2] == value]
    vars_ = {r[3] for r in recs}
    if len(vars_) != 1:
        return None
    var = vars_.pop()
    body = md.get(var, "")
    name = _md_field(body, "name")
    if not name or not re.fullmatch(r"[A-Za-z_]\w*", name):
        return None
    all_vars = {r[3] for r in f.records}
    if sum(1 for v in all_vars if _md_field(md.get(v, ""), "name") == name) != 1:
        return None
    if _md_field(body, "scope") not in loop_scope:
        return None
    ty = _c_type(md, _md_field(body, "type"))
    if ty is None:
        return None
    d = f.defs.get(value)
    if d is None:
        return None
    mine = [r for r in recs if r[0] == d]
    if not mine:
        return None
    last = max(r[1] for r in mine)
    if d != header:
        if any(r[3] == var and r[0] == d and r[1] > last for r in f.records):
            return None
        region = _reach(f, d, d) & ({header} | {b for b in f.blocks if header in _reach(f, b, d) or b == header})
        if any(r[3] == var and r[0] in region for r in f.records):
            return None
    elif value not in f.phis.get(header, set()):
        return None  # defined in the header after the loop head
    return name, ty[0], ty[1]


_REL_C = {"eq": "==", "ne": "!=", "ule": "<=", "ult": "<", "uge": ">=", "ugt": ">",
          "sle": "<=", "slt": "<", "sge": ">=", "sgt": ">"}


def _render(conj: dict[str, Any], term_of: Any) -> str | None:
    """C text of one proved conjunct, or None when C would mean something
    else (signedness, promotion, wrap-around).

    >>> t = {"x": ("x", True, 32), "u": ("u", False, 32), "c": ("c", True, 8)}
    >>> term = lambda j: t[j["v"]] if "v" in j else None
    >>> _render({"rel": "sle", "a": {"v": "x", "w": 32}, "b": {"c": "4294967295", "w": 32}}, term)
    'x <= -1'
    >>> _render({"rel": "ule", "a": {"v": "x", "w": 32}, "b": {"c": "5", "w": 32}}, term) is None
    True
    >>> _render({"rel": "ult", "a": {"v": "x", "w": 32}, "b": {"v": "u", "w": 32}}, term)
    'x < u'
    >>> _render({"rel": "mask:1", "a": {"v": "u", "w": 32}, "b": {"c": "1", "w": 32}}, term)
    '(u & 1) == 1'
    >>> _render({"rel": "eq", "a": {"v": "c", "w": 8}, "b": {"v": "u", "w": 32}}, term) is None
    True
    """
    rel = str(conj.get("rel", ""))

    def side(j: dict[str, Any], signed: bool) -> str | None:
        if "v" in j:
            tv = term_of(j)
            return tv[0] if tv else None
        w = int(j.get("w", 0))
        v = int(str(j.get("c")))
        if signed and v >= 1 << (w - 1):
            v -= 1 << w
        return str(v) if (signed or v <= 2**31 - 1) else f"{v}U" if w <= 32 else f"{v}UL"

    a, b = conj.get("a") or {}, conj.get("b") or {}
    ta = term_of(a) if "v" in a else None
    if ta is None or int(a.get("w", 0)) != ta[2]:
        return None
    tb = term_of(b) if "v" in b else None
    if "v" in b and (tb is None or int(b.get("w", 0)) != tb[2]):
        return None
    w = ta[2]
    if rel.startswith("mask:"):
        m = int(rel.split(":", 1)[1])
        if "c" not in b or m >= 1 << (w - 1):
            return None
        return f"({ta[0]} & {m}) == {int(str(b['c'])) & m}"
    if rel in ("add", "sub"):
        c = conj.get("c") or {}
        if tb is None or ta[1] or tb[1] or w < 32 or tb[2] != w or "c" not in c:
            return None  # only unsigned int/long arithmetic wraps like the bit-vectors
        return f"{ta[0]} {'+' if rel == 'add' else '-'} {tb[0]} == {side(c, False)}"
    if rel not in _REL_C:
        return None
    if rel in ("eq", "ne"):
        if tb is not None and w < 32 and ta[1] != tb[1]:
            return None  # promotion keeps values: bit-equal is not value-equal
        sb = side(b, ta[1])
    elif rel[0] == "s":
        if not ta[1] or (tb is not None and not tb[1]):
            return None
        sb = side(b, True)
    else:
        if tb is None:
            if ta[1]:
                return None
        elif w < 32 and (ta[1] or tb[1]):
            return None
        elif w >= 32 and ta[1] and tb[1]:
            return None
        sb = side(b, False)
    return None if sb is None else f"{ta[0]} {_REL_C[rel]} {sb}"


def pir_invariant_texts(task: Path, finding: dict[str, Any]) -> list[list[str]] | None:
    """C conjuncts per exported loop of a pir PROVED-UNBOUNDED finding
    (aligned with extra["invariant_loops"]); None when unavailable."""
    extra = finding.get("extra") or {}
    try:
        conj = json.loads(str(extra["invariant_conjuncts"]))
        loops = json.loads(str(extra["invariant_loops"]))
    except (KeyError, ValueError):
        return None
    if LINE_MARKER.search(task.read_text(encoding="utf-8", errors="replace")):
        return None  # debug locations would name another file's lines
    ir = debug_ir(task)
    if ir is None:
        return None
    f, md = parse_debug_ir(ir, str(finding.get("function") or "main"))
    if f is None:
        return None
    # loop start (line, col) -> header block (the target of the loop's back edge)
    headers: dict[tuple[int, int], str] = {}
    scopes: dict[tuple[int, int], list[str]] = {}
    for ref, targets in f.loops.items():
        m = re.search(r"!\{(!\d+), (!\d+)", md.get(ref, ""))
        if not m or len(set(targets)) != 1:
            continue
        loc = md.get(m.group(2), "")
        if "DILocation" not in loc:
            continue
        key = (int(_md_field(loc, "line") or 0), int(_md_field(loc, "column") or 0))
        headers[key] = targets[0]
        scopes[key] = _scope_chain(md, _md_field(loc, "scope"))
    out: list[list[str]] = []
    for j, loop in enumerate(loops if isinstance(loops, list) else []):
        key = (int(loop.get("line") or 0), int(loop.get("column") or 0))
        texts: list[str] = []
        h = headers.get(key)
        if h is not None and j < len(conj):
            cache: dict[str, Any] = {}

            def term_of(t: dict[str, Any], _h: str = h, _k: tuple[int, int] = key) -> Any:
                v = str(t.get("v"))
                if v not in cache:
                    cache[v] = _current_var(f, md, v, _h, scopes[_k])
                return cache[v]

            for c in conj[j]:
                txt = _render(c, term_of)
                if txt and txt not in texts:
                    texts.append(txt)
        out.append(texts)
    return out


@dataclass
class Outcome:
    decision: Decision
    witness_path: Path | None = None
    lines: list[str] = field(default_factory=list)


def solve(task: Path, prop_file: Path, *, prism: str | None, allow_exec: bool, data_model: str,
          out: Path, witness_path: Path | None, timeout: float | None = None) -> Outcome:
    spec = prop_file.read_text(encoding="utf-8")
    prop = parse_property(spec)
    if prop not in PROPERTIES:
        return Outcome(Decision("unknown", f"unsupported property file {prop_file.name}"))
    src_text = task.read_text(encoding="utf-8", errors="replace")
    if data_model.upper() == "ILP32" and width_dependent_code(src_text):
        return Outcome(Decision("unknown", "ILP32 task uses width-dependent types; PRISM's encoders are LP64"))
    exe = find_prism(prism)
    if exe is None:
        return Outcome(Decision("error", "prism binary not found (--prism, PRISM_BIN, next to this script, PATH)"))
    out.mkdir(parents=True, exist_ok=True)
    # Preprocessed .i tasks are C; copy to .c so clang names the unit consistently.
    task_c = out / (task.stem + ".c")
    shutil.copyfile(task, task_c)
    try:
        report = run_prism(exe, task_c, out / "prism-out", timeout)
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as e:
        return Outcome(Decision("error", str(e)))

    def rp(f: dict[str, Any]) -> dict[str, Any]:
        trace = nondet_trace(f)
        values: list[int | float] | None = None if trace is None else [v for _, v in trace]
        with tempfile.TemporaryDirectory(prefix="prism-replay-", dir=out) as d:
            return replay(task, prop, allow_exec=allow_exec, nondet_values=values, work=Path(d))

    dec = decide(report, prop, rp)
    oc = Outcome(dec)
    if dec.answer == "true" and witness_path is not None and dec.finding is not None:
        invariants, note = correctness_invariants(task, dec.finding)
        doc = W.build_correctness_witness(
            invariants, input_file=task, input_file_name=task.name, specification=spec,
            data_model=data_model.upper(), producer_version=version_string(exe))
        W.write_witness(doc, witness_path)
        oc.witness_path = witness_path
        dec.reason += f"; correctness witness: {note}"
        return oc
    if dec.answer.startswith("false") and witness_path is not None and dec.finding is not None:
        line = int(dec.replay.get("line") or dec.finding.get("line") or 1)
        col: int | None = (target_column(task, line, int(dec.replay["column"])) if dec.replay.get("column")
                           else first_code_column(task, line))
        if prop == "unreach-call":
            site = reach_error_call_site(task, dec.finding.get("line"))
            if site is None:
                # No witness rather than a witness pointing at the wrong call.
                dec.reason += "; no witness: the reach_error() call site is ambiguous"
                return oc
            line, col = site
        # No "function" on the target: the location (UBSan report, reach_error()
        # call) need not be in main, and the field is optional in format 2.0.
        cex = W.Counterexample(function="main", target=W.Location(task.name, line, col))
        trace = nondet_trace(dec.finding) or []
        physical = (dec.finding.get("extra") or {}).get("nondet_loc_kind") == "physical"
        cex.nondet = nondet_waypoints(task, trace, nondet_locations(dec.finding, len(trace)), physical)
        doc = W.build_violation_witness(
            cex, input_file=task, input_file_name=task.name, specification=spec,
            data_model=data_model.upper(), producer_version=version_string(exe))
        W.write_witness(doc, witness_path)
        oc.witness_path = witness_path
    return oc


def prism_engine_version(prism: str) -> str | None:
    """Parse ``prism --version`` (``prism 0.1.0 (C++ engine)``)."""
    try:
        r = subprocess.run([prism, "--version"], capture_output=True, text=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    m = re.match(r"prism\s+(\S+)\s+\(C\+\+\s+engine\)", (r.stdout or "").strip())
    return m.group(1) if m else None


def version_string(prism: str | None) -> str:
    if not prism:
        return WRAPPER_VERSION
    ver = prism_engine_version(prism)
    if ver:
        return ver
    h = hashlib.sha256(Path(prism).read_bytes()).hexdigest()[:12]
    return f"{WRAPPER_VERSION}+sha256.{h}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("task", nargs="?")
    ap.add_argument("--prop", help="SV-COMP property file (.prp)")
    ap.add_argument("--prism", help="PRISM C++ binary (else PRISM_BIN, next to this script, PATH)")
    ap.add_argument("--data-model", default="LP64", choices=["LP64", "ILP32", "lp64", "ilp32"])
    ap.add_argument("--allow-exec", action="store_true",
                    help="replay counterexamples by compiling and running the task (Law 9)")
    ap.add_argument("--out", default="prism-svcomp-out")
    ap.add_argument("--witness", default="witness.yml")
    ap.add_argument("--version", action="store_true")
    a = ap.parse_args(argv)
    if a.version:
        print(version_string(find_prism(a.prism)))
        return 0
    if not a.task or not a.prop:
        ap.error("TASK and --prop are required")
    oc = solve(Path(a.task), Path(a.prop), prism=a.prism, allow_exec=a.allow_exec, data_model=a.data_model,
               out=Path(a.out), witness_path=Path(a.witness))
    print(RESULT_PREFIX + oc.decision.answer)
    print(REASON_PREFIX + oc.decision.reason.replace("\n", " "))
    if oc.witness_path:
        print(f"PRISM-SVCOMP-WITNESS: {oc.witness_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
