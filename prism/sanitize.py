"""ASan/UBSan/TSan compile+run probes. Missing compiler or sanitizer = NOTRUN.

Never reports CLEAN from a probe alone. A CLEAN run is not a proof of absence.
A MinGW-only compiler with no libubsan is NOTRUN, never a fake sanitized CLEAN.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from prism import laws
from prism.config import Config
from prism.cparse import extract_functions
from prism.models import Finding

_PROBE_SRC = "int main(void){return 0;}\n"
# Signed overflow must fire under a real UBSan; MinGW without libubsan will not.
_UB_FIRE_SRC = (
    "int main(void){\n"
    "    volatile int a = 2147483647;\n"
    "    volatile int b = 1;\n"
    "    volatile int c = a + b;\n"
    "    (void)c;\n"
    "    return 0;\n"
    "}\n"
)
# Heap OOB must fire under a real ASan; flag-accept is not enough.
_AS_FIRE_SRC = (
    "#include <stdlib.h>\n"
    "int main(void){\n"
    "    char *p = (char*)malloc(1);\n"
    "    if (!p) return 1;\n"
    "    p[8] = 1;\n"
    "    free(p);\n"
    "    return 0;\n"
    "}\n"
)
_PROBE_TIMEOUT = 15.0
_UB_FLAGS = ("-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-O0")
_TS_FLAGS = ("-fsanitize=thread", "-O0")
_AS_FLAGS = ("-fsanitize=address", "-fno-sanitize-recover=address", "-O0")
_INSTALL = "install gcc or clang with sanitizer support (https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)"
_UB_LIBS = (
    "libubsan.so",
    "libubsan.so.1",
    "libubsan.a",
    "libubsan.dll",
    "libubsan.dll.a",
    "libclang_rt.ubsan_standalone.a",
    "libclang_rt.ubsan_standalone-x86_64.a",
    "clang_rt.ubsan_standalone-x86_64.lib",
)
_TS_LIBS = (
    "libtsan.so",
    "libtsan.so.1",
    "libtsan.a",
    "libtsan.dll",
    "libtsan.dll.a",
    "libclang_rt.tsan.a",
    "libclang_rt.tsan-x86_64.a",
)
_AS_LIBS = (
    "libasan.so",
    "libasan.so.1",
    "libasan.a",
    "libasan.dll",
    "libasan.dll.a",
    "libclang_rt.asan.a",
    "libclang_rt.asan-x86_64.a",
    "clang_rt.asan-x86_64.lib",
)


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


def _dumpmachine(cc: str) -> str:
    try:
        r = _run([cc, "-dumpmachine"], _PROBE_TIMEOUT)
        return ((r.stdout or "") + (r.stderr or "")).strip().lower()
    except (subprocess.TimeoutExpired, OSError):
        return ""


def _is_mingw(cc: str) -> bool:
    """True for MinGW / mingw-w64 triples and paths. Never treat as UBSan."""
    if "mingw" in cc.lower().replace("\\", "/"):
        return True
    return "mingw" in _dumpmachine(cc)


def _sanitizer_lib_names(flags: tuple[str, ...]) -> tuple[str, ...]:
    joined = " ".join(flags)
    if "thread" in joined:
        return _TS_LIBS
    if "address" in joined:
        return _AS_LIBS
    return _UB_LIBS


def _has_sanitizer_lib(cc: str, flags: tuple[str, ...]) -> bool:
    """True when -print-file-name resolves a real sanitizer runtime, not an echo."""
    for name in _sanitizer_lib_names(flags):
        try:
            r = _run([cc, f"-print-file-name={name}"], _PROBE_TIMEOUT)
        except (subprocess.TimeoutExpired, OSError):
            continue
        printed = (r.stdout or "").strip()
        if not printed:
            continue
        # gcc/clang echo the search name unchanged when the file is missing.
        if printed.replace("\\", "/") == name:
            continue
        if Path(printed).is_file():
            return True
    return False


def _compile_ok(cc: str, src: Path, exe: Path, flags: tuple[str, ...]) -> bool:
    r = _run([cc, *flags, str(src), "-o", str(exe)], _PROBE_TIMEOUT)
    return r.returncode == 0 and exe.is_file()


def _ubsan_actually_fires(cc: str, td: Path, flags: tuple[str, ...]) -> bool:
    """A real UBSan must trap planted signed overflow. Flag-accept is not enough."""
    src = td / "ub_fire.c"
    src.write_text(_UB_FIRE_SRC, encoding="utf-8")
    exe = td / f"ub_fire{_exe_suffix()}"
    if not _compile_ok(cc, src, exe, flags):
        return False
    try:
        run = _run([str(exe)], _PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False
    return _sanitizer_hit(run.stderr or "", run.stdout or "", run.returncode)


def _asan_actually_fires(cc: str, td: Path, flags: tuple[str, ...]) -> bool:
    """A real ASan must trap planted heap OOB. Flag-accept is not enough."""
    src = td / "as_fire.c"
    src.write_text(_AS_FIRE_SRC, encoding="utf-8")
    exe = td / f"as_fire{_exe_suffix()}"
    if not _compile_ok(cc, src, exe, flags):
        return False
    try:
        run = _run([str(exe)], _PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False
    return _sanitizer_hit(run.stderr or "", run.stdout or "", run.returncode)


def _probe_sanitizer(cc: str, flags: tuple[str, ...]) -> bool:
    """True only when cc actually instruments with these flags.

    MinGW without libubsan/libtsan/libasan is False even if -fsanitize=* is accepted
    and a binary is produced. A trivial compile is not a sanitizer.
    """
    if _is_mingw(cc) and not _has_sanitizer_lib(cc, flags):
        return False
    try:
        with tempfile.TemporaryDirectory(prefix="prism_san_probe_") as td:
            tdp = Path(td)
            src = tdp / "probe.c"
            src.write_text(_PROBE_SRC, encoding="utf-8")
            exe = tdp / f"probe{_exe_suffix()}"
            if not _compile_ok(cc, src, exe, flags):
                return False
            if "-fsanitize=undefined" in flags:
                return _ubsan_actually_fires(cc, tdp, flags)
            if "-fsanitize=address" in flags:
                return _asan_actually_fires(cc, tdp, flags)
            return True
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


def _sanitizer_runtime_unusable(text: str) -> bool:
    """WSL/ASLR TSan mapping abort is the instrument dying, not a race in the plant."""
    return "unexpected memory mapping" in (text or "").lower()


def _sanitizer_hit(stderr: str, stdout: str, returncode: int) -> bool:
    text = (stderr or "") + (stdout or "")
    if _sanitizer_runtime_unusable(text):
        return False
    low = text.lower()
    if "undefinedbehaviorsanitizer" in low or "threadsanitizer" in low:
        return True
    if "addresssanitizer" in low or "heap-buffer-overflow" in low:
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
        with tempfile.TemporaryDirectory(prefix="prism_san_run_") as td:
            wrapper = Path(td) / "prism_san_main.c"
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
            if _sanitizer_runtime_unusable(text):
                return (
                    laws.NOTRUN,
                    "sanitizer runtime unusable (unexpected memory mapping); not a defect finding",
                    text,
                )
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
        return laws.NOTRUN, str(exc), ""


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
    """Probe ASan, UBSan, and TSan; compile+run each .c when the sanitizer is supported."""
    cc = _find_cc()
    if not cc:
        return [_notrun("gcc/clang not on PATH", "ubsan")]

    out: list[Finding] = []

    if not _probe_sanitizer(cc, _AS_FLAGS):
        out.append(_notrun("compiler has no ASan", "asan"))
    else:
        out.extend(_run_sanitizer_on_paths(cc, paths, _AS_FLAGS, "asan", cfg))

    if not _probe_sanitizer(cc, _UB_FLAGS):
        out.append(_notrun("compiler has no UBSan", "ubsan"))
    else:
        out.extend(_run_sanitizer_on_paths(cc, paths, _UB_FLAGS, "ubsan", cfg))

    if not _probe_sanitizer(cc, _TS_FLAGS):
        out.append(_notrun("compiler has no TSan", "tsan"))
    else:
        out.extend(_run_sanitizer_on_paths(cc, paths, _TS_FLAGS, "tsan", cfg))

    return out
