#!/usr/bin/env python3
"""PRISM as an SV-COMP verifier: run PRISM on one task, map report.json to an
SV-COMP answer, replay the counterexample and write a violation witness.

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
  for no-overflow, ``pir`` for unreach-call; nothing covers the whole of
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
  refutation of a program with ``__VERIFIER_nondet_*`` inputs (the engines do
  not report nondet values yet, so it cannot be replayed), a stage that
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
    "no-overflow": {"prove": {"bmc", "pir"}, "classes": {"INT-SIGNED-OVF"}},
    "unreach-call": {"prove": {"pir"}, "classes": {"FUNC-CONTRACT"}, "props": {"reach_error", "assert"}},
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


def refutes(f: dict[str, Any], prop: str) -> bool:
    spec = PROPERTIES[prop]
    if f.get("status") != "FAILED" or f.get("cls") not in spec["classes"]:
        return False
    if "props" in spec:
        return str((f.get("extra") or {}).get("prop", "")) in spec["props"]
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
OVERFLOW_MSG = re.compile(r"^(signed integer overflow|negation of .* cannot be represented|division of .* cannot be represented)")
ASAN_DEREF = {"heap-buffer-overflow", "stack-buffer-overflow", "global-buffer-overflow", "SEGV",
              "heap-use-after-free", "stack-use-after-return", "stack-use-after-scope", "stack-buffer-underflow"}
ASAN_FREE = {"attempting", "bad-free", "double-free"}


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
    san = {"no-overflow": "signed-integer-overflow", "valid-memsafety": "address"}.get(prop)
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
            m = re.search(r":(\d+):", err)
            rec.update(replay="replayed", detail="reach_error() was called and the program aborted")
            if m:
                rec["line"] = int(m.group(1))
            return rec
    elif prop == "valid-memsafety":
        m = ASAN_RE.search(err)
        if m:
            kind = m.group("kind")
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


def run_prism(prism: str, task_c: Path, out: Path, timeout: float | None) -> dict[str, Any]:
    argv = [prism, str(task_c), "--no-llm", "--stage", STAGES, "--out", str(out)]
    subprocess.run(argv, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
    rep = out / "report.json"
    if not rep.exists():
        raise RuntimeError(f"prism wrote no report.json ({' '.join(argv)})")
    return dict(json.loads(rep.read_text(encoding="utf-8")))


def first_code_column(src: Path, line: int) -> int:
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    if 1 <= line <= len(lines):
        s = lines[line - 1]
        return len(s) - len(s.lstrip()) + 1
    return 1


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
    # The pir stage takes .c/.cpp units only; a preprocessed .i task is C.
    task_c = out / (task.stem + ".c")
    shutil.copyfile(task, task_c)
    try:
        report = run_prism(exe, task_c, out / "prism-out", timeout)
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as e:
        return Outcome(Decision("error", str(e)))

    def rp(f: dict[str, Any]) -> dict[str, Any]:
        with tempfile.TemporaryDirectory(prefix="prism-replay-", dir=out) as d:
            return replay(task, prop, allow_exec=allow_exec, nondet_values=None, work=Path(d))

    dec = decide(report, prop, rp)
    oc = Outcome(dec)
    if dec.answer.startswith("false") and witness_path is not None and dec.finding is not None:
        line = int(dec.replay.get("line") or dec.finding.get("line") or 1)
        col = int(dec.replay.get("column") or first_code_column(task, line))
        cex = W.Counterexample(function="main", target=W.Location(task.name, line, col, "main"))
        doc = W.build_violation_witness(
            cex, input_file=task, input_file_name=task.name, specification=spec,
            data_model=data_model.upper(), producer_version=version_string(exe))
        W.write_witness(doc, witness_path)
        oc.witness_path = witness_path
    return oc


def version_string(prism: str | None) -> str:
    if not prism:
        return WRAPPER_VERSION
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
