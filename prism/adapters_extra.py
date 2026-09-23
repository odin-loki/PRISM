"""Optional external tools. Missing binary = NOTRUN, never a clean result.

KLEE, AFL++, Frama-C, Infer, CodeQL, clang-tidy, CBMC, Strix, semgrep:
search (1) Config.tools / --tool, (2) a built executable under
third_party/<vendor>/ if present, (3) PATH. Missing is NOTRUN with an
install hint at the vendored tree in third_party/SOURCES.md. If present
we run a safe --help when there is nothing to analyse, or a cheap real
check when C/C++ sources exist. Absence is never CLEAN. A successful
help/version probe is never CLEAN or PROVED.
"""

from __future__ import annotations

from pathlib import Path
import json
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

from prism import laws, sandbox
from prism.config import Config, adapter_install, ordered_map, resolve_adapter
from prism.models import Finding, FunctionInfo

# (stage, PATH names). Install hint is adapter_install(stage) → SOURCES.md.
OPTIONAL_TOOLS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("klee", ("klee",)),
    ("afl-fuzz", ("afl-fuzz", "afl-fuzz.exe")),
    ("frama-c", ("frama-c", "frama-c.exe")),
    ("infer", ("infer",)),
    ("codeql", ("codeql", "codeql.exe")),
    ("clang-tidy", ("clang-tidy", "clang-tidy.exe")),
    ("cbmc", ("cbmc", "cbmc.exe")),
    ("strix", ("strix", "strix.exe")),
    ("semgrep", ("semgrep", "semgrep.exe")),
    ("spatch", ("spatch", "spatch.exe")),
)

_LIBFUZZER_INSTALL = "clang -fsanitize=fuzzer is not vendored (see third_party/SOURCES.md)"

_C_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}

_PKG_COCCI = Path(__file__).resolve().parent / "cocci"

_SPATCH_HIT_RE = re.compile(r"^(.+):(\d+):\s*(.*)$")

# A .cocci rule with a script/initialize/finalize block runs embedded
# Python/OCaml inside spatch (Law 9). Same regex in src/prism/adapters.cpp.
_COCCI_SCRIPT_RE = re.compile(r"^\s*@\s*(?:script|initialize|finalize)\s*:", re.M)


def cocci_has_script(rule: Path) -> bool:
    """True when the rule would execute embedded code in spatch."""
    try:
        return bool(_COCCI_SCRIPT_RE.search(rule.read_text(encoding="utf-8", errors="replace")))
    except OSError:
        return False


def _not_run(stage: str, binary: str, how: str) -> Finding:
    return Finding(
        stage=stage, status=laws.NOTRUN, file="", function=None, line=None,
        cls="", message=f"{binary} not found (config, vendored tree, PATH)",
        strength=laws.STRENGTH_FINDS, extra={"install": how},
    )


def _run(cmd: list[str], timeout: float, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd, capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=timeout, cwd=cwd,
    )


def _run_harness(cmd: list[str], timeout: float, cwd: Path) -> subprocess.CompletedProcess:
    """Run a built harness (Law 9): prism.sandbox jail + rlimits, cwd as scratch."""
    return sandbox.run_binary(cmd, scratch=cwd, timeout=timeout, text=True, cwd=cwd,
                              limit_as=False)


def _is_fake_adapter(text: str) -> bool:
    """Catch2/doctest (or unknown --timeout/--unwind) is not CBMC/ESBMC/cppcheck/dafny."""
    low = (text or "").lower()
    return (
        "doctest version" in low
        or "catch2 v" in low
        or "unknown option" in low
    )


def _probe_looks_missing(text: str, rc: int) -> bool:
    """Shell 'not found' / 126 / 127 is a missing tool, not a help page.

    A doctest/Catch2 binary that answers --help is not CBMC/ESBMC/spatch.
    """
    low = (text or "").lower()
    if _is_fake_adapter(text) or "unknown option: --timeout" in low:
        return True
    if rc in {0, 1}:
        return False
    if rc in {126, 127}:
        return True
    return "command not found" in low or "no such file" in low or ": not found" in low


def _probe(exe: str) -> subprocess.CompletedProcess | None:
    """Safe presence check: --help / -h / --version. Never a code verdict.

    A path that exists but cannot start (shell 'not found', exec format) is
    missing — the caller maps that to NOTRUN, never ERROR/CLEAN/PROVED.
    """
    for args in (("--help",), ("-h",), ("--version",), ("-version",)):
        try:
            r = _run([exe, *args], timeout=12)
        except (subprocess.TimeoutExpired, OSError):
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if _probe_looks_missing(text, r.returncode):
            continue
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

    def tidy(p: Path) -> subprocess.CompletedProcess | subprocess.TimeoutExpired:
        std = "-std=c++11" if p.suffix.lower() in {".cc", ".cpp", ".cxx"} else "-std=c11"
        try:
            return _run([exe, str(p), "--", std], timeout=min(60.0, cfg.timeout + 15))
        except subprocess.TimeoutExpired as exc:
            return exc

    # One clang-tidy process per file on cfg.jobs threads; findings are built
    # in file order below, exactly as the serial loop did.
    results = ordered_map(tidy, files, getattr(cfg, "jobs", 1))
    out: list[Finding] = []
    for p, r in zip(files, results):
        if isinstance(r, subprocess.TimeoutExpired):
            out.append(Finding(
                stage="clang-tidy", status=laws.TIMEOUT, file=str(p), function=None,
                line=None, cls="", message="clang-tidy timeout",
                strength=laws.STRENGTH_FINDS,
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
            out.append(Finding(
                stage="clang-tidy", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="",
                message="clang-tidy at PATH is not clang-tidy (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("clang-tidy")},
            ))
            continue
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
    if not out:
        return [Finding(
            stage="clang-tidy", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"clang-tidy present at {exe}; no diagnostics (not a proof)",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]
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
        except OSError as exc:
            out.append(Finding(
                stage="cbmc", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="", message=f"cbmc unusable: {exc}",
                strength=laws.STRENGTH_PROVES,
                extra={"exe": exe, "install": adapter_install("cbmc")},
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        upper = text.upper()
        if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
            st, msg = laws.NOTRUN, "cbmc at PATH is not CBMC (not a proof)"
        elif "VERIFICATION SUCCESSFUL" in upper:
            st, msg = laws.BOUNDED, "CBMC: BOUNDED (unwind limited; not a proof)"
        elif "VERIFICATION FAILED" in upper:
            st, msg = laws.FAILED, "CBMC verification failed"
        elif "VERIFICATION UNKNOWN" in upper:
            st, msg = laws.UNKNOWN, "CBMC unknown"
        else:
            st, msg = laws.ERROR, (text[-400:] or "no verdict line (not a proof)")
        f = Finding(
            stage="cbmc", status=st, file=str(p), function=None, line=None,
            cls="", message=msg, strength=laws.STRENGTH_PROVES,
            evidence=text[-1500:],
        )
        if st == laws.NOTRUN:
            f.extra = {"exe": exe, "install": adapter_install("cbmc")}
        out.append(f)
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
        with tempfile.TemporaryDirectory(prefix="prism_libfuzzer_") as td:
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
            stage="libfuzzer", status=laws.NOTRUN, file="", function=None, line=None,
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


def _libfuzzer_flag_rejected(text: str) -> bool:
    low = (text or "").lower()
    return "unsupported" in low or "unknown" in low or "unrecognized" in low


def _libfuzzer_harness_source(fn: FunctionInfo, src_rel: str) -> str:
    from prism.fuzz import C_TYPE_SIZE, param_nbytes

    nbytes = param_nbytes(fn.params)
    dlines: list[str] = []
    reads: list[str] = []
    args: list[str] = []
    off = 0
    for typ, name in fn.params:
        key = " ".join(typ.split()) or "int"
        dlines.append(f"    {key} {name};")
        sz = C_TYPE_SIZE.get(" ".join(typ.replace("*", " ").split()), 4)
        reads.append(f"    memcpy(&{name}, Data + {off}, {sz});")
        args.append(name)
        off += sz
    rel = src_rel.replace("\\", "/")
    return (
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "#include <string.h>\n"
        f'#include "{rel}"\n'
        "\n"
        "int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {\n"
        f"    if (Size < {nbytes}) return 0;\n"
        + "\n".join(dlines) + "\n"
        + "\n".join(reads) + "\n"
        f"    (void){fn.name}({', '.join(args)});\n"
        "    return 0;\n"
        "}\n"
    )


def _compile_libfuzzer(clang: str, src: Path, exe: Path) -> tuple[str, str]:
    """Compile a libFuzzer harness. Returns ('ok'|'notrun'|'error', detail).

    Prefer fuzzer+ASan+UBSan together; fall back to one sanitizer, then
    fuzzer alone. Missing ``-fsanitize=fuzzer`` is NOTRUN, never ERROR/CLEAN.
    """
    san_tries = [
        ["-fsanitize=fuzzer,address,undefined",
         "-fno-sanitize-recover=address,undefined"],
        ["-fsanitize=fuzzer,undefined", "-fno-sanitize-recover=undefined"],
        ["-fsanitize=fuzzer,address", "-fno-sanitize-recover=address"],
        ["-fsanitize=fuzzer"],
    ]
    last_text = ""
    rejected = False
    for san in san_tries:
        r = _run([clang, *san, "-O0", "-g", "-std=c11", str(src), "-o", str(exe)], timeout=30)
        text = (r.stderr or "") + (r.stdout or "")
        last_text = text
        if r.returncode == 0:
            return "ok", ""
        if _libfuzzer_flag_rejected(text):
            rejected = True
    if rejected:
        return "notrun", last_text
    return "error", last_text


def _run_libfuzzer(
    fn: FunctionInfo,
    src: Path,
    *,
    timeout: float = 2.0,
    work: Path | None = None,
) -> Finding:
    """Bounded libFuzzer campaign on a SCALAR harness.

    Missing clang or ``-fsanitize=fuzzer`` is NOTRUN, never CLEAN and never
    extra["engine"]="libfuzzer". POINTER is NEEDS-HARNESS, never ERROR.
    A campaign that finds nothing is CLEAN, which is not a proof.
    """
    from prism.bmc import unencoded_syntax_reason
    from prism.cparse import body_needs_pointer_harness

    base: dict[str, Any] = dict(
        stage="libfuzzer", file=fn.file, function=fn.name, line=fn.line,
        cls="", strength=laws.STRENGTH_FINDS,
    )
    if fn.kind == "POINTER":
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="POINTER: libFuzzer harness would invent a buffer or pass NULL",
        )
    if fn.kind == "OTHER":
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="OTHER signature, not harnessed",
        )
    syn = unencoded_syntax_reason(fn, "libFuzzer")
    if syn:
        return Finding(**base, status=laws.NEEDS_HARNESS, message=syn)
    if body_needs_pointer_harness(fn.body or ""):
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="local pointer or heap object: libFuzzer harness would invent a buffer",
        )

    if not sandbox.allowed():
        # Law 9: the libFuzzer binary runs the scanned function.
        return sandbox.exec_notrun("libfuzzer", "libFuzzer harness", file=fn.file,
                                   function=fn.name, line=fn.line, exec=laws.NOTRUN)

    clang = shutil.which("clang")
    if not clang:
        return Finding(
            **base, status=laws.NOTRUN,
            message="clang not on PATH",
            extra={"install": _LIBFUZZER_INSTALL},
        )

    cleanup = work is None
    work = work or Path(tempfile.mkdtemp(prefix="prism_libfuzzer_run_"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        src_copy = work / Path(src).name
        if not src_copy.exists():
            src_copy.write_text(
                Path(src).read_text(encoding="utf-8", errors="replace"),
                encoding="utf-8",
            )
        hpath = work / f"lfuzzer_{fn.name}.c"
        hpath.write_text(_libfuzzer_harness_source(fn, src_copy.name), encoding="utf-8")
        exe = work / f"lfuzzer_{fn.name}.exe"
        try:
            st, err = _compile_libfuzzer(clang, hpath, exe)
        except subprocess.TimeoutExpired:
            return Finding(
                **base, status=laws.TIMEOUT,
                message="libFuzzer compile timeout",
                extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
            )
        except OSError as exc:
            return Finding(
                **base, status=laws.NOTRUN,
                message=f"libFuzzer compile unusable: {exc}",
                extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
            )
        if st == "notrun":
            return Finding(
                **base, status=laws.NOTRUN,
                message="clang has no libFuzzer (-fsanitize=fuzzer)",
                extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
            )
        if st != "ok":
            return Finding(
                **base, status=laws.ERROR,
                message=f"libFuzzer compile failed: {(err or '')[:200]}",
                extra={"exe": clang},
            )

        extra = {"engine": "libfuzzer", "exe": clang, "sandbox": sandbox.sandbox_kind()}
        corpus = work / "corpus"
        corpus.mkdir(exist_ok=True)
        from prism.fuzz import param_nbytes
        (corpus / "seed").write_bytes(b"\x00" * param_nbytes(fn.params))
        try:
            r = _run_harness(
                [str(exe), str(corpus), f"-max_total_time={max(1, int(timeout))}",
                 "-timeout=1"],
                timeout + 10,
                work,
            )
        except subprocess.TimeoutExpired:
            r = None
        except OSError as exc:
            return Finding(
                **base, status=laws.NOTRUN,
                message=f"libFuzzer run unusable: {exc}",
                extra={"install": _LIBFUZZER_INSTALL, "exe": clang},
            )

        crashes = [
            p for p in work.iterdir()
            if p.is_file() and p.name.startswith("crash-")
        ]
        if not crashes:
            crashes = list(work.glob("**/crash-*"))
        if crashes:
            data = crashes[0].read_bytes()
            rec = dict(base)
            rec["cls"] = "LIBFUZZER-CRASH"
            return Finding(
                **rec, status=laws.CRASH,
                message=f"libFuzzer crash on {data[:16].hex()}",
                counterexample=data.hex(),
                extra={**extra, "libfuzzer_crashes": len(crashes)},
            )
        text = ""
        if r is not None:
            text = ((r.stderr or "") + (r.stdout or "")).lower()
            if "addresssanitizer" in text or "undefinedbehaviorsanitizer" in text:
                rec = dict(base)
                rec["cls"] = "LIBFUZZER-CRASH"
                return Finding(
                    **rec, status=laws.CRASH,
                    message="libFuzzer sanitizer crash",
                    extra=extra,
                )
        return Finding(
            **base, status=laws.CLEAN,
            message=f"no libFuzzer crash in {timeout:.0f}s (not a proof)",
            extra=extra,
        )
    finally:
        if cleanup:
            shutil.rmtree(work, ignore_errors=True)


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
    """Bundled prism/cocci rules first, then *.cocci under path roots."""
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
    held: list[Finding] = []
    if not getattr(cfg, "allow_exec", False):
        for rule in [r for r in rules if cocci_has_script(r)]:
            held.append(sandbox.exec_notrun(
                "spatch", f"spatch ({rule.name} has a script block)",
                exe=exe, rule=rule.name,
            ))
        rules = [r for r in rules if not cocci_has_script(r)]
    if not rules and held:
        return held
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
                    stage="spatch", status=laws.NOTRUN, file=str(p), function=None,
                    line=None, cls=cls, message=f"spatch unusable: {exc}",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "rule": rule.name, "install": adapter_install("spatch")},
                ))
                continue
            text = (r.stdout or "") + (r.stderr or "")
            if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
                out.append(Finding(
                    stage="spatch", status=laws.NOTRUN, file=str(p), function=None,
                    line=None, cls=cls,
                    message="spatch at PATH is not Coccinelle (not a proof)",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "rule": rule.name, "install": adapter_install("spatch")},
                ))
                continue
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
                continue
            low = text.lower()
            # Match-only rules often exit non-zero with "No rules apply".
            # That is silence, not a defect and not a broken spatch.
            if "no rules apply" in low:
                continue
            if r.returncode != 0:
                if _probe_looks_missing(text, r.returncode):
                    out.append(Finding(
                        stage="spatch", status=laws.NOTRUN, file=str(p), function=None,
                        line=None, cls=cls,
                        message="spatch at PATH is not Coccinelle (not a proof)",
                        strength=laws.STRENGTH_FINDS,
                        extra={"exe": exe, "rule": rule.name, "install": adapter_install("spatch")},
                    ))
                    continue
                out.append(Finding(
                    stage="spatch", status=laws.ERROR, file=str(p), function=None,
                    line=None, cls=cls,
                    message=text[-400:] or f"spatch exit {r.returncode}",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "rule": rule.name},
                ))
    if any_hit:
        return held + out
    if out and all(f.status in {laws.TIMEOUT, laws.ERROR, laws.NOTRUN} for f in out):
        return held + out
    return held + [Finding(
        stage="spatch", status=laws.UNKNOWN, file="", function=None, line=None,
        cls="", message="spatch ran; no matches (not a proof)",
        strength=laws.STRENGTH_FINDS, extra={"exe": exe},
    )]


def _extract_json_object(text: str) -> str:
    """First '{' .. last '}' — same slice C++ extract_json_object uses.

    PRISM law: parse combined stdout+stderr, not stdout alone. Empty text
    is '{}' so a silent success is UNKNOWN, not a dropped parse.
    """
    blob = text or ""
    if not blob.strip():
        return "{}"
    first, last = blob.find("{"), blob.rfind("}")
    if first >= 0 and last > first:
        return blob[first : last + 1]
    return blob


def _run_semgrep(exe: str, paths: list[Path], cfg: Config) -> list[Finding]:
    files = [str(p) for p in _c_files(paths)]
    if not files:
        return [Finding(
            stage="semgrep", status=laws.UNKNOWN, file="", function=None, line=None,
            cls="", message=f"semgrep present at {exe}; no C/C++ files in scope",
            strength=laws.STRENGTH_FINDS, extra={"exe": exe},
        )]

    timeout_s = max(1, int(cfg.timeout))

    def _parse(blob: str) -> list[Finding] | None:
        try:
            data = json.loads(_extract_json_object(blob))
        except json.JSONDecodeError:
            return None
        if not isinstance(data, dict):
            return None
        results = data.get("results") or []
        if not isinstance(results, list):
            results = []
        if not results:
            return [Finding(
                stage="semgrep", status=laws.UNKNOWN, file="", function=None, line=None,
                cls="", message="semgrep ran; no matches (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            )]
        out: list[Finding] = []
        for item in results:
            if not isinstance(item, dict):
                continue
            check_id = str(item.get("check_id") or "semgrep")
            path = str(item.get("path") or "")
            line = (item.get("start") or {}).get("line")
            extra = item.get("extra") or {}
            if not isinstance(extra, dict):
                extra = {}
            msg = str(extra.get("message") or check_id)
            out.append(Finding(
                stage="semgrep", status=laws.FAILED, file=path, function=None,
                line=line, cls=check_id, message=msg[:400],
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            ))
        if not out:
            return [Finding(
                stage="semgrep", status=laws.UNKNOWN, file="", function=None, line=None,
                cls="", message="semgrep ran; no matches (not a proof)",
                strength=laws.STRENGTH_FINDS, extra={"exe": exe},
            )]
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
        if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
            return [Finding(
                stage="semgrep", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message="semgrep at PATH is not semgrep (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("semgrep")},
            )]
        parsed = _parse(text)
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
        if ruleset_missing:
            return [Finding(
                stage="semgrep", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message=(text[-400:] or "semgrep ruleset unavailable (not a code verdict)"),
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("semgrep")},
            )]
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
            stage="infer", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message="infer present but gcc/clang not on PATH",
            strength=laws.STRENGTH_FINDS,
            extra={"exe": exe, "install": "install gcc or clang"},
        )]
    out: list[Finding] = []
    # Each file gets its own scratch directory for infer-out/ and the object
    # file: `infer run -- cc -c` would otherwise write both into whatever
    # directory PRISM was launched from (often the user's tree).
    for p in c_files:
        try:
            with tempfile.TemporaryDirectory(prefix="prism_infer_") as td:
                scratch = Path(td)
                r = _run(
                    [exe, "run", "--results-dir", str(scratch / "infer-out"), "--",
                     compiler, "-c", str(p.resolve()), "-o", str(scratch / "unit.o")],
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
                stage="infer", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="", message=f"infer unusable: {exc}",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("infer")},
            ))
            continue
        text = (r.stdout or "") + (r.stderr or "")
        if _probe_looks_missing(text, r.returncode):
            out.append(Finding(
                stage="infer", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="",
                message="infer at PATH is not Infer (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("infer")},
            ))
            continue
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
            if _probe_looks_missing(text, r.returncode):
                out.append(Finding(
                    stage="infer", status=laws.NOTRUN, file=str(p), function=None,
                    line=None, cls="",
                    message="infer at PATH is not Infer (not a proof)",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "install": adapter_install("infer")},
                ))
            else:
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
        if _probe_looks_missing(text, r.returncode):
            out.append(Finding(
                stage="frama-c", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="",
                message="frama-c at PATH is not Frama-C (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("frama-c")},
            ))
            continue
        if not text.strip() and r.returncode not in {0, 1}:
            if _probe_looks_missing(text, r.returncode):
                out.append(Finding(
                    stage="frama-c", status=laws.NOTRUN, file=str(p), function=None,
                    line=None, cls="",
                    message="frama-c at PATH is not Frama-C (not a proof)",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "install": adapter_install("frama-c")},
                ))
            else:
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
            elif "alarm" in lower and not re.search(r"\b0\s+alarm", ln, re.IGNORECASE):
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
            if _probe_looks_missing(text, r.returncode):
                out.append(Finding(
                    stage="frama-c", status=laws.NOTRUN, file=str(p), function=None,
                    line=None, cls="",
                    message="frama-c at PATH is not Frama-C (not a proof)",
                    strength=laws.STRENGTH_FINDS,
                    extra={"exe": exe, "install": adapter_install("frama-c")},
                ))
            else:
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
    if not getattr(cfg, "allow_exec", False):
        # KLEE interprets bitcode but performs external calls (unlink,
        # system, ...) natively on the host.
        return [sandbox.exec_notrun("klee", "klee (native external calls)", exe=exe)]
    out: list[Finding] = []
    bitcode_ok = False
    for p in c_files:
        try:
            with tempfile.TemporaryDirectory(prefix="prism_klee_") as td:
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
                    kr = _run_harness(
                        [exe, "--max-time=5", "--max-forks=16", str(bc)],
                        min(20.0, cfg.timeout + 10),
                        td_path,
                    )
                except subprocess.TimeoutExpired:
                    out.append(Finding(
                        stage="klee", status=laws.TIMEOUT, file=str(p), function=None,
                        line=None, cls="", message="klee timeout",
                        strength=laws.STRENGTH_FINDS,
                    ))
                    continue
                text = (kr.stdout or "") + (kr.stderr or "")
                if _is_fake_adapter(text) or _probe_looks_missing(text, kr.returncode):
                    out.append(Finding(
                        stage="klee", status=laws.NOTRUN, file=str(p), function=None,
                        line=None, cls="",
                        message="klee at PATH is not KLEE (not a proof)",
                        strength=laws.STRENGTH_FINDS,
                        extra={"exe": exe, "install": adapter_install("klee")},
                    ))
                    continue
                # rglob *.err includes testNNNNNN.ptr.err (the name has no "error").
                dump_hit = any(td_path.rglob("*.err"))
                if "KLEE: ERROR" in text or dump_hit:
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
                stage="klee", status=laws.NOTRUN, file=str(p), function=None,
                line=None, cls="", message=f"klee unusable: {exc}",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("klee")},
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
        if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
            out.append(Finding(
                stage="strix", status=laws.NOTRUN, file=str(spec), function=None,
                line=None, cls="",
                message="strix at PATH is not Strix (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": exe, "install": adapter_install("strix")},
            ))
            continue
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
    out_dir = db.parent / "prism-codeql-out"
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
            stage="codeql", status=laws.NOTRUN, file=str(db), function=None,
            line=None, cls="", message=f"codeql unusable: {exc}",
            strength=laws.STRENGTH_FINDS,
            extra={"exe": exe, "install": adapter_install("codeql")},
        )]
    text = (r.stdout or "") + (r.stderr or "")
    if _is_fake_adapter(text) or _probe_looks_missing(text, r.returncode):
        return [Finding(
            stage="codeql", status=laws.NOTRUN, file=str(db), function=None,
            line=None, cls="",
            message="codeql at PATH is not CodeQL (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"exe": exe, "install": adapter_install("codeql")},
        )]
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
    """Probe optional adapters plus a libFuzzer clang probe.

    Search order: config/explicit, vendored third_party/<name>/ binary if
    already built, then PATH. Missing → NOTRUN + vendored install hint.
    Present → --help or a real check. Never maps a missing binary to CLEAN.
    Successful --help is never CLEAN or PROVED. A --help/-h/--version probe
    that raises or does not answer is NOTRUN, never CLEAN, PROVED, or ERROR.
    """
    out: list[Finding] = []
    for stage, names in OPTIONAL_TOOLS:
        install = adapter_install(stage)
        exe = resolve_adapter(cfg, stage, names)
        if not exe:
            out.append(_not_run(stage, names[0], install))
            continue
        try:
            probed = _probe(exe)
        except Exception as exc:  # noqa: BLE001 — adapter must not crash the pipeline
            f = _not_run(stage, names[0], install)
            f.message = f"{stage} probe failed: {exc}"
            f.extra["exe"] = exe
            out.append(f)
            continue
        if probed is None:
            f = _not_run(stage, names[0], install)
            f.message = f"{stage} at {exe} did not answer --help/-h/--version"
            f.extra["exe"] = exe
            out.append(f)
            continue
        try:
            out.extend(_dispatch_optional(stage, exe, paths, cfg, probed))
        except OSError as exc:
            f = _not_run(stage, names[0], install)
            f.message = f"{stage} unusable: {exc}"
            f.extra["exe"] = exe
            out.append(f)
        except Exception as exc:  # noqa: BLE001 — parse/run must not escape
            out.append(Finding(
                stage=stage, status=laws.ERROR, file="", function=None, line=None,
                cls="", message=f"{stage} run failed: {exc}",
                strength=laws.STRENGTH_FINDS, extra={"install": install, "exe": exe},
            ))
    out.append(_libfuzzer_probe(cfg))
    return out
