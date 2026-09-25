"""External tools. Missing binary = NOTRUN, never a clean result."""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess

from prism import laws
from prism.adapters_extra import _is_fake_adapter
from prism.config import Config, adapter_install, ordered_map, resolve_adapter, stamps_tool
from prism.models import Finding
from prism.pbsd import C_EXTS, run_pbsd_lints


def _not_run(stage: str, binary: str, how: str) -> Finding:
    return Finding(
        stage=stage, status=laws.NOTRUN, file="", function=None, line=None,
        cls="", message=f"{binary} not found (config, ~/.prism/tools, PATH)",
        strength=laws.STRENGTH_FINDS, extra={"install": how},
    )


def _tool_unusable(text: str, rc: int) -> bool:
    """Doctest/Catch2 / shell 126/127 / not-found is a missing tool, not a verdict."""
    if _is_fake_adapter(text):
        return True
    if rc in {126, 127}:
        return True
    low = (text or "").lower()
    return (
        "command not found" in low
        or "no such file" in low
        or ": not found" in low
        or "is not recognized as" in low
        or "cannot execute" in low
    )


@stamps_tool("cppcheck", ("cppcheck", "cppcheck.exe"))
def run_cppcheck(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = resolve_adapter(cfg, "cppcheck", ("cppcheck", "cppcheck.exe"))
    if not exe:
        return [_not_run("cppcheck", "cppcheck", adapter_install("cppcheck"))]
    files = [str(p) for p in paths if p.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}]
    if not files:
        return [Finding(
            stage="cppcheck", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="cppcheck present; no C/C++ files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    cmd = [exe, "--enable=warning,style,performance,portability",
           "--xml", "--xml-version=2", *files]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return [Finding(stage="cppcheck", status=laws.TIMEOUT, file="", function=None,
                        line=None, cls="", message="cppcheck timeout",
                        strength=laws.STRENGTH_FINDS)]
    except OSError as exc:
        return [Finding(
            stage="cppcheck", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message=f"cppcheck unusable: {exc}",
            strength=laws.STRENGTH_FINDS,
            extra={"exe": exe, "install": adapter_install("cppcheck")},
        )]
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
    if not out:
        if _tool_unusable(xml, p.returncode):
            return [Finding(
                stage="cppcheck", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message="cppcheck at PATH is not cppcheck (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("cppcheck")},
            )]
        if p.returncode not in {0, 1}:
            text = (xml or "")[-400:] or f"cppcheck exit {p.returncode}"
            return [Finding(
                stage="cppcheck", status=laws.ERROR, file="", function=None, line=None,
                cls="", message=text, strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            )]
        return [Finding(
            stage="cppcheck", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"cppcheck present at {exe}; no diagnostics (not a proof)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    return out


@stamps_tool("esbmc", ("esbmc", "esbmc.exe"))
def run_esbmc(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = resolve_adapter(cfg, "esbmc", ("esbmc", "esbmc.exe"))
    if not exe:
        return [_not_run("esbmc", "esbmc", adapter_install("esbmc"))]
    files = [p for p in paths if p.suffix.lower() == ".c"]
    if not files:
        return [Finding(
            stage="esbmc", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"esbmc present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_PROVES, extra={"exe": exe},
        )]
    out = []
    for p in files:
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
        except OSError as exc:
            out.append(Finding(
                stage="esbmc", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="", message=f"esbmc unusable: {exc}",
                strength=laws.STRENGTH_PROVES,
                extra={"exe": exe, "install": adapter_install("esbmc")},
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        upper = text.upper()
        if _tool_unusable(text, r.returncode):
            st, msg = laws.NOTRUN, "esbmc at PATH is not ESBMC (not a proof)"
        elif "VERIFICATION SUCCESSFUL" in upper:
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
        extra = {"exe": exe} if st == laws.NOTRUN else {}
        if st == laws.NOTRUN:
            extra["install"] = adapter_install("esbmc")
        out.append(Finding(stage="esbmc", status=st, file=str(p), function=None,
                           line=None, cls="", message=msg,
                           strength=laws.STRENGTH_PROVES, evidence=text[-1500:],
                           extra=extra))
    return out


@stamps_tool("dafny", ("dafny", "dafny.exe"))
def run_dafny(paths: list[Path], cfg: Config) -> list[Finding]:
    exe = resolve_adapter(cfg, "dafny", ("dafny", "dafny.exe"))
    if not exe:
        return [_not_run("dafny", "dafny", adapter_install("dafny"))]
    out = []
    for p in paths:
        if p.suffix.lower() not in {".dfy"}:
            continue
        try:
            r = subprocess.run(
                [exe, "verify", str(p)], capture_output=True, text=True, timeout=60,
                encoding="utf-8", errors="replace",
            )
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="dafny", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="FUNC-CONTRACT", message="dafny timeout",
                strength=laws.STRENGTH_PROVES,
            ))
            continue
        except OSError as exc:
            out.append(Finding(
                stage="dafny", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="FUNC-CONTRACT", message=f"dafny unusable: {exc}",
                strength=laws.STRENGTH_PROVES,
                extra={"exe": exe, "install": adapter_install("dafny")},
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if _tool_unusable(text, r.returncode):
            st, msg = laws.NOTRUN, "dafny at PATH is not Dafny (not a proof)"
            extra = {"exe": exe, "install": adapter_install("dafny")}
        else:
            st = laws.PROVED if r.returncode == 0 else laws.FAILED
            msg = text[-400:]
            extra = {}
        out.append(Finding(stage="dafny", status=st, file=str(p), function=None,
                           line=None, cls="FUNC-CONTRACT", message=msg,
                           strength=laws.STRENGTH_PROVES, extra=extra))
    if not out:
        out.append(Finding(stage="dafny", status=laws.NOTRUN, file="", function=None,
                           line=None, cls="", message="no .dfy files in scope",
                           strength=laws.STRENGTH_PROVES))
    return out


def run_pbsd(cfg: Config, scope: Path) -> list[Finding]:
    """Drive ParanoidBSD portable checkers. Tree present is never CLEAN-as-proof."""
    if scope.is_dir():
        paths = [p for p in scope.rglob("*") if p.suffix.lower() in C_EXTS and p.is_file()]
    else:
        paths = [scope]
    return run_pbsd_lints(paths, cfg)


_WARN_RE = re.compile(r"^(.+):(\d+):(\d+):\s+(warning|error):\s+(.*)$", re.MULTILINE)
_C_UNITS = {".c", ".cc", ".cpp", ".cxx"}
# `fatal error: 'unity.h' file not found` (clang) / `unity.h: No such file or
# directory` (gcc): the unit's include path is unknown, a gap in what PRISM
# could compile, not a defect in the code (Law 7).
_MISSING_HEADER_RE = re.compile(
    r"fatal error:\s*(?:'([^'\n]+)' file not found|([^:\n]+): No such file or directory)")


def _host_compilers() -> list[str]:
    """gcc and clang when both exist. Same resolved path is not a second compiler."""
    out: list[str] = []
    seen: set[str] = set()
    for name in ("gcc", "clang"):
        hit = shutil.which(name)
        if not hit:
            continue
        try:
            key = str(Path(hit).resolve()).casefold()
        except OSError:
            key = hit.replace("\\", "/").casefold()
        if key in seen:
            continue
        seen.add(key)
        out.append(hit)
    return out


def _cc_name(cc: str) -> str:
    """gcc / clang from a resolved path (either separator, .exe dropped)."""
    name = re.split(r"[\\/]", cc)[-1]
    return name[:-4] if name.lower().endswith(".exe") else name


def _norm_diag(msg: str) -> str:
    """gcc and clang word one diagnostic alike but quote and tag it apart:
    drop the trailing [-Wflag], unify quotes, fold case and spaces."""
    m = re.sub(r"\s*\[-W[^\]]*\]\s*$", "", msg)
    m = re.sub(r"[\u2018\u2019\u201c\u201d`\"]", "'", m)
    return " ".join(m.lower().split())


def _rel_to(p: Path, base: Path) -> str:
    """Path relative to the scan root, like every other stage's findings."""
    try:
        return Path(p).resolve().relative_to(Path(base).resolve()).as_posix()
    except (ValueError, OSError):
        return Path(p).as_posix()


def _compiler_cmd(cc: str, path: Path) -> list[str]:
    # Enable checks. Never -w / -Wno-* / a flag that silences diagnostics.
    std = "-std=c++11" if path.suffix.lower() in {".cc", ".cpp", ".cxx"} else "-std=c11"
    return [
        cc, std, "-Wall", "-Wextra", "-Wconversion",
        "-Wsign-compare", "-Wshift-overflow", "-fsyntax-only", str(path),
    ]


def run_compiler(paths: list[Path], cfg: Config) -> list[Finding]:
    compilers = _host_compilers()
    if not compilers:
        return [_not_run("warnings", "gcc", "install gcc or clang")]
    units = [p for p in paths if p.suffix.lower() in _C_UNITS]
    if not units:
        return [Finding(
            stage="warnings", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="gcc/clang present; no .c/.cc/.cpp/.cxx files in scope",
            strength=laws.STRENGTH_SOME,
        )]
    out: list[Finding] = []
    seen: set[tuple] = set()
    by_diag: dict[tuple, Finding] = {}
    base = cfg.root if Path(cfg.root).is_dir() else Path(cfg.root).parent

    def _add(f: Finding) -> None:
        key = (f.file, f.line, f.cls, f.status, f.message)
        if key in seen:
            return
        seen.add(key)
        out.append(f)

    def _diag(cc: str, file: str, line: int, sev: str, msg: str) -> None:
        """One finding per (file, line, message): gcc and clang agreeing is
        one defect with extra.compilers naming both."""
        name = _cc_name(cc)
        key = (file, line, _norm_diag(msg))
        f = by_diag.get(key)
        if f is not None:
            names = f.extra["compilers"].split(",")
            if name not in names:
                f.extra["compilers"] = ",".join([*names, name])
            if sev == "error" and f.extra.get("severity") != "error":
                f.extra["severity"] = "error"
                f.cls = "compiler-error"
            return
        f = Finding(
            stage="warnings", status=laws.FAILED, file=file, function=None,
            line=line, cls="compiler-" + sev, message=msg, strength=laws.STRENGTH_SOME,
            extra={"severity": sev, "compilers": name},
        )
        by_diag[key] = f
        out.append(f)

    jobs_list = [(cc, p, _compiler_cmd(cc, p)) for cc in compilers for p in units]
    for _cc, _p, cmd in jobs_list:
        for flag in cmd:
            if flag == "-w" or flag.startswith("-Wno-"):
                raise ValueError(f"refusing to disable a check: {flag}")

    def syntax_check(
        job: tuple[str, Path, list[str]],
    ) -> subprocess.CompletedProcess[str] | subprocess.TimeoutExpired | OSError:
        try:
            return subprocess.run(
                job[2], capture_output=True, text=True, timeout=30,
                encoding="utf-8", errors="replace",
            )
        except (subprocess.TimeoutExpired, OSError) as exc:
            return exc

    # Each (compiler, unit) check is independent: run them on cfg.jobs
    # threads, then build findings in the serial order so dedup and output
    # order are unchanged.
    results = ordered_map(syntax_check, jobs_list, getattr(cfg, "jobs", 1))
    for (cc, p, _cmd), r in zip(jobs_list, results):
        rel = _rel_to(p, base)
        if isinstance(r, subprocess.TimeoutExpired):
            _add(Finding(
                stage="warnings", status=laws.TIMEOUT, file=rel, function=None,
                line=None, cls="", message="compiler syntax-check timeout",
                strength=laws.STRENGTH_SOME,
            ))
            continue
        if isinstance(r, OSError):
            _add(Finding(
                stage="warnings", status=laws.NOTRUN, file=rel, function=None,
                line=None, cls="", message=f"{cc} unusable: {r}",
                strength=laws.STRENGTH_SOME,
                extra={"install": "install gcc or clang"},
            ))
            continue
        text = (r.stderr or "") + (r.stdout or "")
        mh = _MISSING_HEADER_RE.search(text) if r.returncode != 0 else None
        if mh:
            hdr = mh.group(1) or mh.group(2).strip()
            _add(Finding(
                stage="warnings", status=laws.NOTRUN, file=rel, function=None,
                line=None, cls="",
                message=f"does not compile standalone: header '{hdr}' not found "
                "(include path unknown: no compile_commands.json entry); "
                "compiler warnings not checked for this unit",
                strength=laws.STRENGTH_SOME,
                extra={"install": "generate compile_commands.json "
                       "(cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, bear)"},
            ))
            continue
        hits = 0
        for m in _WARN_RE.finditer(text):
            hits += 1
            _diag(cc, _rel_to(Path(m.group(1)), base), int(m.group(2)), m.group(4),
                  m.group(5))
        if hits == 0 and r.returncode != 0:
            _add(Finding(
                stage="warnings", status=laws.FAILED, file=rel, function=None,
                line=None, cls="compiler-error",
                message=(text.strip()[-400:] or f"{cc} exit {r.returncode}"),
                strength=laws.STRENGTH_SOME,
                extra={"severity": "error", "compilers": _cc_name(cc)},
            ))
    return out
