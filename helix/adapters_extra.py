"""Optional external tools. Missing binary = NOTRUN, never a clean result.

KLEE, AFL++, Frama-C, Infer, CodeQL, clang-tidy, CBMC, Strix, semgrep:
if the binary is not on PATH we record NOTRUN plus an install URL. If it
is present we run a safe --help when there is nothing to analyse, or a
cheap real check when C/C++ sources (or tool-specific inputs) exist.
We never treat absence as CLEAN and never map a successful empty run to
CLEAN or PROVED.
"""

from __future__ import annotations

from pathlib import Path
import json
import re
import shutil
import subprocess
import sys
import tempfile

from helix import laws
from helix.config import Config
from helix.models import Finding

# (stage, PATH names, install URL)
OPTIONAL_TOOLS: tuple[tuple[str, tuple[str, ...], str], ...] = (
    ("klee", ("klee",), "https://klee.github.io/"),
    ("afl-fuzz", ("afl-fuzz", "afl-fuzz.exe"), "https://github.com/AFLplusplus/AFLplusplus"),
    ("frama-c", ("frama-c", "frama-c.exe"), "https://frama-c.com/"),
    ("infer", ("infer",), "https://fbinfer.com/"),
    ("codeql", ("codeql", "codeql.exe"), "https://github.com/github/codeql-cli-binaries"),
    ("clang-tidy", ("clang-tidy", "clang-tidy.exe"), "https://clang.llvm.org/extra/clang-tidy/"),
    ("cbmc", ("cbmc", "cbmc.exe"), "https://github.com/diffblue/cbmc"),
    ("strix", ("strix", "strix.exe"), "https://github.com/meyerphi/strix"),
    ("semgrep", ("semgrep", "semgrep.exe"), "https://semgrep.dev/"),
    ("spatch", ("spatch", "spatch.exe"), "https://coccinelle.gitlabpages.inria.fr/website/"),
)

_LIBFUZZER_INSTALL = "https://llvm.org/docs/LibFuzzer.html"

_C_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}

_PKG_COCCI = Path(__file__).resolve().parent / "cocci"

_SPATCH_HIT_RE = re.compile(r"^(.+):(\d+):\s*(.*)$")


def _not_run(stage: str, binary: str, how: str) -> Finding:
    return Finding(
        stage=stage, status=laws.NOTRUN, file="", function=None, line=None,
        cls="", message=f"{binary} not on PATH",
        strength=laws.STRENGTH_FINDS, extra={"install": how},
    )


def _run(cmd: list[str], timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd, capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=timeout,
    )


def _probe(exe: str) -> subprocess.CompletedProcess | None:
    """Safe presence check: --help / -h / --version. Never a code verdict."""
    for args in (("--help",), ("-h",), ("--version",), ("-version",)):
        try:
            r = _run([exe, *args], timeout=12)
        except (subprocess.TimeoutExpired, OSError):
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if text.strip() or r.returncode in {0, 1}:
            return r
    return None


def _c_files(paths: list[Path]) -> list[Path]:
    return [p for p in paths if p.suffix.lower() in _C_EXTS]


def _run_clang_tidy(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    files = [p for p in _c_files(paths) if p.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}]
    if not files:
        return [Finding(
            stage="clang-tidy", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"clang-tidy present at {exe}; no C/C++ translation units",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out: list[Finding] = []
    for p in files:
        cmd = [exe, str(p), "--", "-std=c11"]
        try:
            r = _run(cmd, timeout=min(60.0, cfg.timeout + 15))
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="clang-tidy", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="clang-tidy timeout",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        hits = 0
        for ln in text.splitlines():
            if ": warning:" in ln or ": error:" in ln:
                hits += 1
                out.append(Finding(
                    stage="clang-tidy", status=laws.FAILED, file=str(p), function=None,
                    line=None, cls="clang-tidy", message=ln.strip()[:400],
                    strength=laws.STRENGTH_FINDS,
                ))
        if hits == 0 and r.returncode not in {0, 1}:
            out.append(Finding(
                stage="clang-tidy", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=text[-400:] or f"clang-tidy exit {r.returncode}",
                strength=laws.STRENGTH_FINDS,
            ))
    return out


def _run_cbmc(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    files = [p for p in paths if p.suffix.lower() == ".c"]
    if not files:
        return [Finding(
            stage="cbmc", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"cbmc present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_PROVES, extra={"exe": exe},
        )]
    out: list[Finding] = []
    for p in files:
        cmd = [exe, str(p), "--unwind", str(cfg.unwind),
               "--timeout", str(int(cfg.timeout))]
        for flag in cmd:
            if flag.startswith("--no-") and flag.endswith("-check"):
                raise ValueError(f"refusing to disable a check: {flag}")
        try:
            r = _run(cmd, timeout=cfg.timeout + 5)
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="cbmc", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="cbmc timeout",
                strength=laws.STRENGTH_PROVES,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        upper = text.upper()
        if "VERIFICATION SUCCESSFUL" in upper:
            st, msg = laws.BOUNDED, "CBMC: BOUNDED (unwind limited; not a proof)"
        elif "VERIFICATION FAILED" in upper:
            st, msg = laws.FAILED, "CBMC verification failed"
        elif "VERIFICATION UNKNOWN" in upper:
            st, msg = laws.UNKNOWN, "CBMC unknown"
        else:
            st, msg = laws.ERROR, (text[-400:] or "no verdict line (not a proof)")
        out.append(Finding(
            stage="cbmc", status=st, file=str(p), function=None, line=None,
            cls="", message=msg, strength=laws.STRENGTH_PROVES,
            evidence=text[-1500:],
        ))
    return out


def _libfuzzer_probe(cfg: Config) -> Finding:  # noqa: ARG001 — cfg kept for API symmetry
    """Probe clang for -fsanitize=fuzzer support (not a code verdict)."""
    clang = shutil.which("clang")
    if not clang:
        return Finding(
            stage="libfuzzer", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message="clang not on PATH",
            strength=laws.STRENGTH_FINDS,
            extra={"install": _LIBFUZZER_INSTALL},
        )
    null_out = "NUL" if sys.platform == "win32" else "/dev/null"
    probe_src = (
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {"
        " (void)d; (void)n; return 0; }\n"
    )
    try:
        with tempfile.TemporaryDirectory(prefix="helix_libfuzzer_") as td:
            src = Path(td) / "fuzz_probe.c"
            src.write_text(probe_src, encoding="utf-8")
            obj = Path(td) / "fuzz_probe.o"
            r = _run(
                [clang, "-fsanitize=fuzzer", "-x", "c", "-c", str(src), "-o", str(obj)],
                timeout=12,
            )
            if r.returncode != 0:
                text = (r.stderr or "") + (r.stdout or "")
                if "unsupported" in text.lower() or "unknown" in text.lower() or "unrecognized" in text.lower():
                    return Finding(
                        stage="libfuzzer", status=laws.NOTRUN, file="", function=None, line=None,
                        cls="", message="clang has no libFuzzer (-fsanitize=fuzzer)",
                        strength=laws.STRENGTH_FINDS,
                        extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
                    )
                # Cheap flag-reject probe on Windows when compile-only object fails.
                r2 = _run(
                    [clang, "-fsanitize=fuzzer", "-x", "c", "-c", "-o", null_out, str(src)],
                    timeout=12,
                )
                if r2.returncode != 0:
                    return Finding(
                        stage="libfuzzer", status=laws.NOTRUN, file="", function=None, line=None,
                        cls="", message="clang has no libFuzzer (-fsanitize=fuzzer)",
                        strength=laws.STRENGTH_FINDS,
                        extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
                    )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return Finding(
            stage="libfuzzer", status=laws.ERROR, file="", function=None, line=None,
            cls="", message=f"libFuzzer probe failed: {exc}",
            strength=laws.STRENGTH_FINDS,
            extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
        )
    return Finding(
        stage="libfuzzer", status=laws.UNKNOWN, file="", function=None, line=None,
        cls="", message=f"libFuzzer supported by {clang}; probe only (not a code verdict)",
        strength=laws.STRENGTH_FINDS,
        extra={"exe": clang},
    )


def _help_ok_finding(stage: str, exe: str, r: subprocess.CompletedProcess) -> Finding:
    """Tool is installed; --help is not a verdict on the source."""
    return Finding(
        stage=stage, status=laws.UNKNOWN, file="", function=None, line=None,
        cls="", message=f"{stage} present at {exe}; ran a help/version probe (not a code verdict)",
        strength=laws.STRENGTH_FINDS,
        extra={"exe": exe, "help_exit": r.returncode},
    )


def _source_roots(paths: list[Path]) -> set[Path]:
    if not paths:
        return {Path(".")}
    return {p.parent if p.is_file() else p for p in paths}


def _cocci_rules(paths: list[Path]) -> list[Path]:
    """Bundled helix/cocci rules first, then *.cocci under path roots."""
    rules: list[Path] = []
    seen: set[Path] = set()
    if _PKG_COCCI.is_dir():
        for rule in sorted(_PKG_COCCI.glob("*.cocci")):
            rp = rule.resolve()
            if rp not in seen:
                rules.append(rule)
                seen.add(rp)
    for root in _source_roots(paths):
        for rule in sorted(root.glob("*.cocci")):
            rp = rule.resolve()
            if rp not in seen:
                rules.append(rule)
                seen.add(rp)
    return rules


def _parse_spatch_hits(text: str) -> list[tuple[str, int | None, str]]:
    hits: list[tuple[str, int | None, str]] = []
    for ln in text.splitlines():
        m = _SPATCH_HIT_RE.match(ln.strip())
        if m:
            line = int(m.group(2)) if m.group(2).isdigit() else None
            hits.append((m.group(1), line, ln.strip()))
    return hits


def _run_spatch(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    rules = _cocci_rules(paths)
    if not rules:
        return [Finding(
            stage="spatch", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="spatch present; no .cocci rules (not a verdict)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    c_files = [p for p in paths if p.suffix.lower() == ".c"][:3]
    if not c_files:
        return [Finding(
            stage="spatch", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"spatch present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out: list[Finding] = []
    any_hit = False
    for rule in rules:
        cls = rule.stem
        for p in c_files:
            cmd = [exe, "--sp-file", str(rule), "--no-show-diff", str(p)]
            try:
                r = _run(cmd, timeout=cfg.timeout + 15)
            except subprocess.TimeoutExpired:
                out.append(Finding(
                    stage="spatch", status=laws.TIMEOUT, file=str(p), function=None,
                    line=None, cls=cls, message="spatch timeout",
                    strength=laws.STRENGTH_FINDS, extra={"exe": exe, "rule": rule.name},
                ))
                continue
            except OSError as exc:
                out.append(Finding(
                    stage="spatch", status=laws.ERROR, file=str(p), function=None,
                    line=None, cls=cls, message=f"spatch failed: {exc}",
                    strength=laws.STRENGTH_FINDS, extra={"exe": exe, "rule": rule.name},
                ))
                continue
            text = (r.stdout or "") + (r.stderr or "")
            hits = _parse_spatch_hits(text)
            if hits:
                any_hit = True
                for path, line, msg in hits:
                    out.append(Finding(
                        stage="spatch", status=laws.FAILED, file=path, function=None,
                        line=line, cls=cls, message=msg[:400],
                        strength=laws.STRENGTH_FINDS,
                        extra={"exe": exe, "rule": rule.name},
                    ))
            elif r.returncode != 0:
                out.append(Finding(
                    stage="spatch", status=laws.ERROR, file=str(p), function=None,
                    line=None, cls=cls,
                    message=text[-400:] or f"spatch exit {r.returncode}",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "rule": rule.name},
                ))
    if any_hit:
        return out
    if out and all(f.status in {laws.TIMEOUT, laws.ERROR} for f in out):
        return out
    return [Finding(
        stage="spatch", status=laws.UNKNOWN, file="", function=None, line=None,
        cls="", message="spatch ran; no matches (not a proof)",
        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
    )]


def _run_semgrep(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    files = [str(p) for p in _c_files(paths)]
    if not files:
        return [Finding(
            stage="semgrep", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"semgrep present at {exe}; no C/C++ files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]

    timeout_s = max(1, int(cfg.timeout))

    def _parse(stdout: str) -> list[Finding] | None:
        try:
            data = json.loads(stdout or "{}")
        except json.JSONDecodeError:
            return None
        results = data.get("results") or []
        if not results:
            return [Finding(
                stage="semgrep", status=laws.UNKNOWN, file="", function=None, line=None,
                cls="", message="semgrep ran; no matches (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            )]
        out: list[Finding] = []
        for item in results:
            check_id = str(item.get("check_id") or "semgrep")
            path = str(item.get("path") or "")
            line = (item.get("start") or {}).get("line")
            extra = item.get("extra") or {}
            msg = str(extra.get("message") or check_id)
            out.append(Finding(
                stage="semgrep", status=laws.FAILED, file=path, function=None,
                line=line, cls=check_id, message=msg[:400],
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        return out

    configs = ("p/c", "auto")
    last_text = ""
    for idx, config in enumerate(configs):
        cmd = [exe, "--config", config, "--json", "--quiet",
               "--timeout", str(timeout_s), *files]
        try:
            r = _run(cmd, timeout=cfg.timeout + 15)
        except subprocess.TimeoutExpired:
            return [Finding(
                stage="semgrep", status=laws.TIMEOUT, file="", function=None,
                line=None, cls="", message="semgrep timeout",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            )]
        text = (r.stdout or "") + (r.stderr or "")
        last_text = text
        parsed = _parse(r.stdout or "")
        if parsed is not None:
            if r.returncode == 0 or any(f.status == laws.FAILED for f in parsed):
                return parsed
        lower = text.lower()
        ruleset_missing = any(
            tok in lower for tok in (
                "invalid configuration", "could not find config",
                "failed to download", "no config", "unknown configuration",
            )
        )
        if ruleset_missing and idx + 1 < len(configs):
            continue
        if parsed is not None:
            return parsed
        return [Finding(
            stage="semgrep", status=laws.ERROR, file="", function=None, line=None,
            cls="", message=(text[-400:] or "semgrep failed (not a proof)"),
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    return [Finding(
        stage="semgrep", status=laws.ERROR, file="", function=None, line=None,
        cls="", message=(last_text[-400:] or "semgrep config failed (not a proof)"),
        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
    )]


def _run_infer(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    c_files = [p for p in paths if p.suffix.lower() == ".c"][:3]
    if not c_files:
        return [Finding(
            stage="infer", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"infer present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        return [Finding(
            stage="infer", status=laws.ERROR, file="", function=None, line=None,
            cls="", message="infer present but gcc/clang not on PATH",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out: list[Finding] = []
    for p in c_files:
        try:
            with tempfile.TemporaryDirectory(prefix="helix_infer_") as td:
                r = _run(
                    [exe, "run", "--", compiler, "-c", str(p.resolve())],
                    timeout=cfg.timeout + 15,
                )
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="infer", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="infer timeout",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        except OSError as exc:
            out.append(Finding(
                stage="infer", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=f"infer failed: {exc}",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        file_failed = False
        for ln in text.splitlines():
            if re.search(r"\berror:\s", ln, re.IGNORECASE):
                file_failed = True
                out.append(Finding(
                    stage="infer", status=laws.FAILED, file=str(p), function=None,
                    line=None, cls="infer", message=ln.strip()[:400],
                    strength=laws.STRENGTH_FINDS, extra={"exe": exe},
                ))
        if file_failed:
            continue
        if "no issues found" in text.lower() or not text.strip():
            out.append(Finding(
                stage="infer", status=laws.UNKNOWN, file=str(p), function=None,
                line=None, cls="", message="infer ran; no issues (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        elif r.returncode != 0:
            out.append(Finding(
                stage="infer", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=text[-400:] or f"infer exit {r.returncode}",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        else:
            out.append(Finding(
                stage="infer", status=laws.UNKNOWN, file=str(p), function=None,
                line=None, cls="", message="infer ran; no issues (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
    return out


def _run_frama_c(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    c_files = [p for p in paths if p.suffix.lower() == ".c"][:3]
    if not c_files:
        return [Finding(
            stage="frama-c", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"frama-c present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out: list[Finding] = []
    for p in c_files:
        cmd = [exe, "-eva", "-eva-no-progress", "-eva-verbose", "0", str(p)]
        try:
            r = _run(cmd, timeout=cfg.timeout + 15)
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="frama-c", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="frama-c timeout",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if not text.strip() and r.returncode not in {0, 1}:
            out.append(Finding(
                stage="frama-c", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=f"frama-c exit {r.returncode} (parse failure)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
            continue
        file_failed = False
        for ln in text.splitlines():
            lower = ln.lower()
            if "warning:" in lower:
                file_failed = True
                out.append(Finding(
                    stage="frama-c", status=laws.FAILED, file=str(p), function=None,
                    line=None, cls="FUNC-CONTRACT", message=ln.strip()[:400],
                    strength=laws.STRENGTH_FINDS, extra={"exe": exe},
                ))
            elif "alarm" in lower and "0 alarm" not in lower:
                file_failed = True
                cls = "UNINIT-READ" if "uninit" in lower else "FUNC-CONTRACT"
                out.append(Finding(
                    stage="frama-c", status=laws.FAILED, file=str(p), function=None,
                    line=None, cls=cls, message=ln.strip()[:400],
                    strength=laws.STRENGTH_FINDS, extra={"exe": exe},
                ))
        if file_failed:
            continue
        if re.search(r"\b0\s+alarm", text, re.IGNORECASE):
            out.append(Finding(
                stage="frama-c", status=laws.UNKNOWN, file=str(p), function=None,
                line=None, cls="", message="frama-c EVA: 0 alarms (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        elif r.returncode != 0:
            out.append(Finding(
                stage="frama-c", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=text[-400:] or f"frama-c exit {r.returncode}",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        else:
            out.append(Finding(
                stage="frama-c", status=laws.UNKNOWN, file=str(p), function=None,
                line=None, cls="", message="frama-c ran; no alarms parsed (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
    return out


def _run_klee(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    clang = shutil.which("clang")
    c_files = [p for p in paths if p.suffix.lower() == ".c"][:2]
    if not c_files:
        return [Finding(
            stage="klee", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"klee present at {exe}; no .c files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    if not clang:
        return [Finding(
            stage="klee", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="klee present; no bitcode toolchain (not a verdict)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out: list[Finding] = []
    bitcode_ok = False
    for p in c_files:
        try:
            with tempfile.TemporaryDirectory(prefix="helix_klee_") as td:
                td_path = Path(td)
                bc = td_path / "x.bc"
                br = _run(
                    [clang, "-emit-llvm", "-c", "-g", "-o", str(bc), str(p.resolve())],
                    timeout=min(30.0, cfg.timeout + 10),
                )
                if br.returncode != 0 or not bc.is_file():
                    continue
                bitcode_ok = True
                try:
                    kr = _run(
                        [exe, "--max-time=5", "--max-forks=16", str(bc)],
                        timeout=min(20.0, cfg.timeout + 10),
                    )
                except subprocess.TimeoutExpired:
                    out.append(Finding(
                        stage="klee", status=laws.TIMEOUT, file=str(p), function=None,
                        line=None, cls="", message="klee timeout",
                        strength=laws.STRENGTH_FINDS,
                    ))
                    continue
                text = (kr.stdout or "") + (kr.stderr or "")
                crashes = list(td_path.glob("*.err")) + list(td_path.glob("*.ktest"))
                if "KLEE: ERROR" in text or any("error" in c.name.lower() for c in crashes):
                    out.append(Finding(
                        stage="klee", status=laws.FAILED, file=str(p), function=None,
                        line=None, cls="klee", message=text[-400:] or "klee error path",
                        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
                        evidence=text[-1500:],
                    ))
                else:
                    out.append(Finding(
                        stage="klee", status=laws.UNKNOWN, file=str(p), function=None,
                        line=None, cls="",
                        message="klee ran; no path dumped a error (not a proof)",
                        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
                    ))
        except OSError as exc:
            out.append(Finding(
                stage="klee", status=laws.ERROR, file=str(p), function=None,
                line=None, cls="", message=f"klee failed: {exc}",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
    if not out and not bitcode_ok:
        return [Finding(
            stage="klee", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="klee present; no bitcode toolchain (not a verdict)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    return out


def _strix_specs(paths: list[Path]) -> list[Path]:
    specs: list[Path] = []
    seen: set[Path] = set()
    for root in _source_roots(paths):
        for pattern in ("*.ltl", "*.tlsf"):
            for spec in sorted(root.glob(pattern)):
                if spec not in seen:
                    seen.add(spec)
                    specs.append(spec)
    return specs


def _run_strix(exe: str, paths: list[Path], cfg: Config,
               probed: subprocess.CompletedProcess) -> list[Finding]:
    specs = _strix_specs(paths)
    if not specs:
        return [_help_ok_finding("strix", exe, probed)]
    out: list[Finding] = []
    for spec in specs[:3]:
        try:
            r = _run([exe, str(spec)], timeout=cfg.timeout + 10)
        except subprocess.TimeoutExpired:
            out.append(Finding(
                stage="strix", status=laws.TIMEOUT, file=str(spec), function=None,
                line=None, cls="", message="strix timeout",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        lower = text.lower()
        if any(tok in lower for tok in ("counterexample", "violation", "falsified")):
            out.append(Finding(
                stage="strix", status=laws.FAILED, file=str(spec), function=None,
                line=None, cls="strix", message=text[-400:] or "strix spec failed",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        else:
            out.append(Finding(
                stage="strix", status=laws.UNKNOWN, file=str(spec), function=None,
                line=None, cls="", message="strix ran; result is not a proof",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
    return out


def _find_codeql_db(paths: list[Path]) -> Path | None:
    for root in _source_roots(paths):
        cand = root / "codeql-db"
        if cand.is_dir():
            return cand
    return None


def _run_codeql(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    db = _find_codeql_db(paths)
    if db is None:
        return [Finding(
            stage="codeql", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message="codeql present; no database (not a verdict)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    out_dir = db.parent / "helix-codeql-out"
    try:
        out_dir.mkdir(exist_ok=True)
        r = _run(
            [exe, "database", "analyze", str(db), "--format=sarif-latest",
             str(out_dir / "results.sarif")],
            timeout=min(60.0, cfg.timeout + 30),
        )
    except subprocess.TimeoutExpired:
        return [Finding(
            stage="codeql", status=laws.TIMEOUT, file=str(db), function=None,
            line=None, cls="", message="codeql database analyze timeout",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    except OSError as exc:
        return [Finding(
            stage="codeql", status=laws.ERROR, file=str(db), function=None,
            line=None, cls="", message=f"codeql analyze failed: {exc}",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    text = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        return [Finding(
            stage="codeql", status=laws.ERROR, file=str(db), function=None,
            line=None, cls="", message=text[-400:] or f"codeql exit {r.returncode}",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    try:
        sarif = json.loads((out_dir / "results.sarif").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return [Finding(
            stage="codeql", status=laws.UNKNOWN, file=str(db), function=None,
            line=None, cls="", message="codeql analyze ran; no SARIF parsed (not a proof)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
    runs = sarif.get("runs") or []
    findings: list[Finding] = []
    for run in runs:
        for res in run.get("results") or []:
            rule_id = str((res.get("ruleId") or "codeql"))
            msg = str(((res.get("message") or {}).get("text")) or rule_id)
            locs = res.get("locations") or []
            path, line = "", None
            if locs:
                phys = (locs[0].get("physicalLocation") or {})
                art = phys.get("artifactLocation") or {}
                path = str(art.get("uri") or "")
                line = (phys.get("region") or {}).get("startLine")
            findings.append(Finding(
                stage="codeql", status=laws.FAILED, file=path, function=None,
                line=line, cls=rule_id, message=msg[:400],
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
    if findings:
        return findings
    return [Finding(
        stage="codeql", status=laws.UNKNOWN, file=str(db), function=None,
        line=None, cls="", message="codeql analyze ran; no results (not a proof)",
        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
    )]


def _dispatch_optional(stage: str, exe: str, paths: list[Path], cfg: Config,
                       probed: subprocess.CompletedProcess) -> list[Finding]:
    c_files = _c_files(paths)
    if stage == "clang-tidy":
        return _run_clang_tidy(exe, paths, cfg)
    if stage == "cbmc":
        return _run_cbmc(exe, paths, cfg)
    if stage == "afl-fuzz":
        return [_help_ok_finding(stage, exe, probed)]
    if stage == "strix":
        return _run_strix(exe, paths, cfg, probed)
    if stage == "codeql":
        return _run_codeql(exe, paths, cfg)
    if stage == "spatch":
        return _run_spatch(exe, paths, cfg)
    if not c_files:
        return [_help_ok_finding(stage, exe, probed)]
    runners = {
        "semgrep": _run_semgrep,
        "infer": _run_infer,
        "frama-c": _run_frama_c,
        "klee": _run_klee,
    }
    runner = runners.get(stage)
    if runner is None:
        return [_help_ok_finding(stage, exe, probed)]
    return runner(exe, paths, cfg)


def run_optional_tools(paths: list[Path], cfg: Config) -> list[Finding]:
    """Probe optional PATH tools plus a libFuzzer clang probe.

    Missing → NOTRUN + install URL. Present → --help or a real check.
    Never maps a missing binary to CLEAN.
    """
    out: list[Finding] = []
    for stage, names, install in OPTIONAL_TOOLS:
        exe = None
        for n in names:
            exe = shutil.which(n)
            if exe:
                break
        if not exe:
            out.append(_not_run(stage, names[0], install))
            continue
        try:
            probed = _probe(exe)
        except Exception as exc:  # noqa: BLE001 — adapter must not crash the pipeline
            out.append(Finding(
                stage=stage, status=laws.ERROR, file="", function=None, line=None,
                cls="", message=f"{stage} probe failed: {exc}",
                strength=laws.STRENGTH_FINDS, extra={"install": install, "exe": exe},
            ))
            continue
        if probed is None:
            out.append(Finding(
                stage=stage, status=laws.ERROR, file="", function=None, line=None,
                cls="", message=f"{stage} at {exe} did not answer --help/-h/--version",
                strength=laws.STRENGTH_FINDS, extra={"install": install, "exe": exe},
            ))
            continue
        try:
            out.extend(_dispatch_optional(stage, exe, paths, cfg, probed))
        except Exception as exc:  # noqa: BLE001 — parse/run must not escape
            out.append(Finding(
                stage=stage, status=laws.ERROR, file="", function=None, line=None,
                cls="", message=f"{stage} run failed: {exc}",
                strength=laws.STRENGTH_FINDS, extra={"install": install, "exe": exe},
            ))
    out.append(_libfuzzer_probe(cfg))
    return out
