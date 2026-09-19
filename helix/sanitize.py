"""ASan/UBSan/TSan compile+run probes. Missing compiler or sanitizer = NOTRUN.

Never reports CLEAN from a probe alone. A CLEAN run is not a proof of absence.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from helix import laws
from helix.config import Config
from helix.cparse import extract_functions
from helix.models import Finding

_PROBE_SRC = "int main(void){return 0;}\n"
_PROBE_TIMEOUT = 15.0
_UB_FLAGS = ("-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-O0")
_TS_FLAGS = ("-fsanitize=thread", "-O0")
_INSTALL = "install gcc or clang with sanitizer support (https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)"


def _find_cc() -> str | None:
    return shutil.which("gcc") or shutil.which("clang")


def _exe_suffix() -> str:
    return ".exe" if sys.platform == "win32" else ""


def _run(cmd: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def _probe_sanitizer(cc: str, flags: tuple[str, ...]) -> bool:
    """Return True when cc accepts the sanitizer flags for a trivial program."""
    try:
        with tempfile.TemporaryDirectory(prefix="helix_san_probe_") as td:
            src = Path(td) / "probe.c"
            src.write_text(_PROBE_SRC, encoding="utf-8")
            exe = Path(td) / f"probe{_exe_suffix()}"
            r = _run([cc, *flags, str(src), "-o", str(exe)], _PROBE_TIMEOUT)
            return r.returncode == 0 and exe.is_file()
    except (subprocess.TimeoutExpired, OSError):
        return False


def _zero_param_callable(path: Path) -> str | None:
    """Name of a zero-argument function we may call, or None."""
    rel = str(path)
    for fn in extract_functions(path, rel):
        if fn.params:
            continue
        if fn.kind in {"SCALAR", "VOID"} and fn.name != "main":
            return fn.name
    return None


def _wrapper_main(call: str | None) -> str:
    if call:
        return f"int main(void){{\n    (void){call}();\n    return 0;\n}}\n"
    return "int main(void){ return 0; }\n"


def _sanitizer_hit(stderr: str, stdout: str, returncode: int) -> bool:
    text = (stderr or "") + (stdout or "")
    low = text.lower()
    if "undefinedbehaviorsanitizer" in low or "threadsanitizer" in low:
        return True
    if "runtime error:" in low:
        return True
    if returncode < 0:
        return True
    return False


def _compile_and_run(
    cc: str,
    source: Path,
    flags: tuple[str, ...],
    timeout: float,
) -> tuple[str, str, str]:
    """Compile+run one translation unit under sanitizer flags."""
    try:
        with tempfile.TemporaryDirectory(prefix="helix_san_run_") as td:
            wrapper = Path(td) / "helix_san_main.c"
            wrapper.write_text(_wrapper_main(_zero_param_callable(source)), encoding="utf-8")
            exe = Path(td) / f"run{_exe_suffix()}"
            comp = _run(
                [cc, "-std=c11", *flags, str(source), str(wrapper), "-o", str(exe)],
                min(timeout, _PROBE_TIMEOUT),
            )
            if comp.returncode != 0:
                text = (comp.stderr or "") + (comp.stdout or "")
                return laws.ERROR, text[-800:] or f"compile failed (exit {comp.returncode})", text
            if not exe.is_file():
                return laws.ERROR, "compile produced no binary", (comp.stderr or "") + (comp.stdout or "")
            try:
                run = _run([str(exe)], min(timeout, _PROBE_TIMEOUT))
            except subprocess.TimeoutExpired:
                return laws.TIMEOUT, "sanitizer run timeout", ""
            text = (run.stderr or "") + (run.stdout or "")
            if _sanitizer_hit(run.stderr or "", run.stdout or "", run.returncode):
                return laws.FAILED, text[-800:] or "sanitizer abort", text
            if run.returncode != 0:
                return laws.FAILED, text[-800:] or f"exit {run.returncode}", text
            return (
                laws.CLEAN,
                "ran under sanitizer with exit 0 (not a proof of absence)",
                text,
            )
    except subprocess.TimeoutExpired:
        return laws.TIMEOUT, "sanitizer compile/run timeout", ""
    except OSError as exc:
        return laws.ERROR, str(exc), ""


def _notrun(message: str, sanitizer: str, install: str = _INSTALL) -> Finding:
    return Finding(
        stage="sanitize",
        status=laws.NOTRUN,
        file="",
        function=None,
        line=None,
        cls=sanitizer,
        message=message,
        strength=laws.STRENGTH_FINDS,
        extra={"install": install, "sanitizer": sanitizer},
    )


def _run_sanitizer_on_paths(
    cc: str,
    paths: list[Path],
    flags: tuple[str, ...],
    sanitizer: str,
    cfg: Config,
) -> list[Finding]:
    c_files = [p for p in paths if p.suffix.lower() == ".c" and p.is_file()]
    if not c_files:
        return [
            Finding(
                stage="sanitize",
                status=laws.UNKNOWN,
                file="",
                function=None,
                line=None,
                cls=sanitizer,
                message=f"{sanitizer} supported by {cc}; no .c files in scope",
                strength=laws.STRENGTH_FINDS,
                extra={"exe": cc, "sanitizer": sanitizer},
            )
        ]
    out: list[Finding] = []
    for p in c_files:
        st, msg, evidence = _compile_and_run(cc, p, flags, cfg.timeout)
        fn = _zero_param_callable(p)
        out.append(
            Finding(
                stage="sanitize",
                status=st,
                file=str(p),
                function=fn,
                line=None,
                cls=sanitizer,
                message=msg,
                strength=laws.STRENGTH_FINDS,
                evidence=evidence[-1500:],
                extra={"exe": cc, "sanitizer": sanitizer},
            )
        )
    return out


def run_sanitize(paths: list[Path], cfg: Config) -> list[Finding]:
    """Probe UBSan and TSan; compile+run each .c when the sanitizer is supported."""
    cc = _find_cc()
    if not cc:
        return [_notrun("gcc/clang not on PATH", "ubsan")]

    out: list[Finding] = []

    if not _probe_sanitizer(cc, _UB_FLAGS):
        out.append(_notrun("compiler has no UBSan", "ubsan"))
    else:
        out.extend(_run_sanitizer_on_paths(cc, paths, _UB_FLAGS, "ubsan", cfg))

    if not _probe_sanitizer(cc, _TS_FLAGS):
        out.append(_notrun("compiler has no TSan", "tsan"))
    else:
        out.extend(_run_sanitizer_on_paths(cc, paths, _TS_FLAGS, "tsan", cfg))

    return out
