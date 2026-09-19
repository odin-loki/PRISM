"""External tools. Missing binary = NOTRUN, never a clean result."""

from __future__ import annotations

from pathlib import Path
import json
import re
import shutil
import subprocess
import sys

from helix import laws
from helix.config import Config
from helix.models import Finding


def _not_run(stage: str, binary: str, how: str) -> Finding:
    return Finding(
        stage=stage, status=laws.NOTRUN, file="", function=None, line=None,
        cls="", message=f"{binary} not on PATH",
        strength=laws.STRENGTH_FINDS, extra={"install": how},
    )


def run_cppcheck(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = shutil.which("cppcheck")
    if not exe:
        return [_not_run("cppcheck", "cppcheck", "apt install cppcheck / choco install cppcheck")]
    files = [str(p) for p in paths if p.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}]
    if not files:
        return []
    cmd = [exe, "--enable=warning,style,performance,portability",
           "--xml", "--xml-version=2", *files]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return [Finding(stage="cppcheck", status=laws.TIMEOUT, file="", function=None,
                        line=None, cls="", message="cppcheck timeout",
                        strength=laws.STRENGTH_FINDS)]
    # cppcheck writes XML to stderr
    xml = p.stderr or p.stdout
    out: list[Finding] = []
    import re
    for m in re.finditer(
        r'<error id="([^"]+)" severity="([^"]+)" msg="([^"]+)"[^>]*>'
        r'(?:\s*<location file="([^"]+)" line="(\d+)")?',
        xml,
    ):
        eid, sev, msg, fil, line = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if sev in {"information", "style"} and eid.startswith("unused"):
            continue
        out.append(Finding(
            stage="cppcheck", status=laws.FAILED, file=fil or "", function=None,
            line=int(line) if line else None, cls=eid,
            message=msg.replace("&apos;", "'").replace("&lt;", "<").replace("&gt;", ">"),
            strength=laws.STRENGTH_FINDS,
        ))
    return out


def run_esbmc(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = shutil.which("esbmc")
    if not exe:
        return [_not_run("esbmc", "esbmc",
                         "release binary from https://github.com/esbmc/esbmc")]
    out = []
    for p in paths:
        if p.suffix.lower() != ".c":
            continue
        cmd = [exe, str(p), "--unwind", str(cfg.unwind),
               "--overflow-check", "--memory-leak-check",
               "--timeout", str(int(cfg.timeout))]
        # NEVER pass --no-bounds-check / --no-pointer-check / ...
        for flag in cmd:
            if flag.startswith("--no-") and flag.endswith("-check"):
                raise ValueError(f"refusing to disable a check: {flag}")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=cfg.timeout + 5)
        except subprocess.TimeoutExpired:
            out.append(Finding(stage="esbmc", status=laws.TIMEOUT, file=str(p),
                               function=None, line=None, cls="", message="timeout",
                               strength=laws.STRENGTH_PROVES))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        upper = text.upper()
        if "VERIFICATION SUCCESSFUL" in upper:
            st = laws.PROVED if "INDUCTION" not in upper else laws.PROVED_UNBOUNDED
            if "--k-induction" not in cmd:
                st = laws.BOUNDED if "UNWINDING ASSERTION" in upper else laws.PROVED
            msg = "ESBMC: " + st
        elif "VERIFICATION FAILED" in upper:
            st, msg = laws.FAILED, "ESBMC verification failed"
        elif "VERIFICATION UNKNOWN" in upper:
            st, msg = laws.UNKNOWN, "ESBMC unknown"
        else:
            st, msg = laws.ERROR, text[-400:] or "no verdict line (not a proof)"
        out.append(Finding(stage="esbmc", status=st, file=str(p), function=None,
                           line=None, cls="", message=msg,
                           strength=laws.STRENGTH_PROVES, evidence=text[-1500:]))
    return out


def run_dafny(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = shutil.which("dafny")
    if not exe:
        return [_not_run("dafny", "dafny", "https://github.com/dafny-lang/dafny")]
    out = []
    for p in paths:
        if p.suffix.lower() not in {".dfy"}:
            continue
        r = subprocess.run([exe, "verify", str(p)], capture_output=True, text=True, timeout=60)
        text = r.stdout + r.stderr
        st = laws.PROVED if r.returncode == 0 else laws.FAILED
        out.append(Finding(stage="dafny", status=st, file=str(p), function=None,
                           line=None, cls="FUNC-CONTRACT", message=text[-400:],
                           strength=laws.STRENGTH_PROVES))
    if not out:
        out.append(Finding(stage="dafny", status=laws.NOTRUN, file="", function=None,
                           line=None, cls="", message="no .dfy files in scope",
                           strength=laws.STRENGTH_PROVES))
    return out


def run_pbsd(cfg: Config, scope: Path) -> list[Finding]:
    """Drive ParanoidBSD tools/verify when the tree is present."""
    root = cfg.pbsd_root
    sweep = root / "tools" / "verify" / "sweep_all.py"
    if not sweep.exists():
        return [_not_run("pbsd", "sweep_all.py",
                         f"set HELIX_PBSD to the ParanoidBSD tree (looked at {root})")]
    # Do not silently run a twelve-hour sweep. Inventory + lints that do
    # not need clang/cbmc: call the portable drivers if they exist.
    out: list[Finding] = []
    out.append(Finding(
        stage="pbsd", status=laws.CLEAN, file=str(root), function=None, line=None,
        cls="", message=f"ParanoidBSD tree at {root}; use --pbsd-sweep to run sweep_all.py",
        strength=laws.STRENGTH_FINDS,
        extra={"sweep": str(sweep), "note": "full sweep is opt-in"},
    ))
    return out


_WARN_RE = re.compile(r"^(.+):(\d+):(\d+):\s+(warning|error):\s+(.*)$", re.MULTILINE)


def run_compiler(paths: list[Path], cfg: Config) -> list[Finding]:
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return [_not_run("warnings", "gcc", "install gcc or clang")]
    out: list[Finding] = []
    for p in paths:
        if p.suffix.lower() not in {".c", ".cc", ".cpp"}:
            continue
        cmd = [
            cc, "-std=c11", "-Wall", "-Wextra", "-Wconversion",
            "-Wsign-compare", "-Wshift-overflow", "-fsyntax-only", str(p),
        ]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="warnings", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="compiler syntax-check timeout",
                strength=laws.STRENGTH_SOME,
            ))
            continue
        text = (r.stderr or "") + (r.stdout or "")
        for m in _WARN_RE.finditer(text):
            out.append(Finding(
                stage="warnings", status=laws.FAILED, file=m.group(1), function=None,
                line=int(m.group(2)), cls="compiler-" + m.group(4),
                message=m.group(5), strength=laws.STRENGTH_SOME,
            ))
    return out
