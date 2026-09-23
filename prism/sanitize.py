"""ASan/UBSan/TSan compile+run probes. Missing compiler or sanitizer = NOTRUN.

Never reports CLEAN from a probe alone. A CLEAN run is not a proof of absence.
A MinGW-only compiler with no libubsan is NOTRUN, never a fake sanitized CLEAN.

Law 9: this stage runs code from the scanned tree, so it is NOTRUN without
--allow-exec (Config.allow_exec). Even with it, PRISM never calls an
arbitrary function: only a zero-argument function the author marked with a
``// prism: run`` comment on or directly above its definition is called
from the generated main, and the binary runs inside prism.sandbox. (Scalar
functions fed chosen inputs are the fuzz stage's job, under ASan+UBSan.)
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from prism import laws, sandbox
from prism.config import Config, ordered_map
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


# `// prism: run` or `/* prism: run */` (same marker in src/prism/adapters.cpp).
RUN_MARKER = re.compile(r"(?://|/\*)\s*prism:\s*run\b")
_RUN_WORD = re.compile(r"\bprism:\s*run\b")  # inside a comment block line
_COMMENT_LINE = re.compile(r"^\s*(?://|/\*|\*)|\*/\s*$")
NO_OPT_IN = ("no function marked `// prism: run` in {n} .c file(s); sanitize calls "
             "only opted-in zero-argument functions")
OPT_IN_HINT = "mark a zero-argument function with a `// prism: run` comment (trusted code only)"


def marked_run(lines: list[str], line: int) -> bool:
    """True when `// prism: run` sits on the definition or directly above it.

    ``line`` is the 1-based first line of the definition. The signature
    lines up to the opening brace count as "on"; the contiguous comment
    block right above counts as "above".
    """
    i = max(0, line - 1)
    j = i
    while j < len(lines):
        if RUN_MARKER.search(lines[j]):
            return True
        if "{" in lines[j] or j - i >= 8:
            break
        j += 1
    k = i - 1
    while k >= 0 and lines[k].strip() and _COMMENT_LINE.search(lines[k]):
        if _RUN_WORD.search(lines[k]):
            return True
        k -= 1
    return False


def _opted_in_callable(path: Path) -> str | None:
    """Name of the zero-argument function the author opted in, or None.

    Never "any void(void)": a hostile tree's cleanup_everything() is not
    called. A static function cannot be called from the separate main TU.
    """
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for fn in extract_functions(path, str(path)):
        if fn.params or fn.static or fn.name == "main":
            continue
        if fn.kind in {"SCALAR", "VOID"} and marked_run(lines, fn.line):
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


_UNPARSED = object()


def _compile_and_run(
    cc: str,
    source: Path,
    flags: tuple[str, ...],
    timeout: float,
    call: str | None | object = _UNPARSED,
) -> tuple[str, str, str]:
    """Compile+run one translation unit under sanitizer flags.

    ``call`` is ``_opted_in_callable(source)`` when the caller already has it.
    No opted-in function means nothing is compiled or run (NOTRUN).
    """
    if call is _UNPARSED:
        call = _opted_in_callable(source)
    if not isinstance(call, str):
        return laws.NOTRUN, NO_OPT_IN.format(n=1), ""
    try:
        with tempfile.TemporaryDirectory(prefix="prism_san_run_") as td:
            wrapper = Path(td) / "prism_san_main.c"
            wrapper.write_text(_wrapper_main(call), encoding="utf-8")
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
                # Sanitizer shadow memory needs an unlimited address space.
                run = sandbox.run_binary(
                    [str(exe)], scratch=Path(td), timeout=min(timeout, _PROBE_TIMEOUT),
                    text=True, limit_as=False,
                )
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


def _no_opt_in(n: int, sanitizer: str) -> Finding:
    return _notrun(NO_OPT_IN.format(n=n), sanitizer, OPT_IN_HINT)


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


def _c_units(paths: list[Path]) -> list[Path]:
    return [p for p in paths if p.suffix.lower() == ".c" and p.is_file()]


def _no_c_files(cc: str, sanitizer: str) -> Finding:
    return Finding(
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


_RunResult = tuple[tuple[str, str, str], str | None]


def _run_one(
    cc: str, p: Path, flags: tuple[str, ...], cfg: Config,
    callables: dict[Path, str | None],
) -> _RunResult:
    # One parse per file per run: the wrapper's call and the finding's
    # function are the same name, and every sanitizer calls the same one.
    if p in callables:
        fn = callables[p]
    else:
        fn = callables[p] = _opted_in_callable(p)
    return _compile_and_run(cc, p, flags, cfg.timeout, fn), fn


def _results_to_findings(
    cc: str, c_files: list[Path], sanitizer: str, results: list[_RunResult],
) -> list[Finding]:
    out: list[Finding] = []
    for p, ((st, msg, evidence), fn) in zip(c_files, results):
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
                extra={"exe": cc, "sanitizer": sanitizer, "sandbox": sandbox.sandbox_kind()},
            )
        )
    return out


def _run_sanitizer_on_paths(
    cc: str,
    paths: list[Path],
    flags: tuple[str, ...],
    sanitizer: str,
    cfg: Config,
) -> list[Finding]:
    if not getattr(cfg, "allow_exec", False):
        return [sandbox.exec_notrun("sanitize", "sanitize (ASan/UBSan/TSan runs)")]
    c_files = _c_units(paths)
    if not c_files:
        return [_no_c_files(cc, sanitizer)]
    callables = {p: _opted_in_callable(p) for p in c_files}
    targets = [p for p in c_files if callables[p]]
    if not targets:
        return [_no_opt_in(len(c_files), sanitizer)]
    results = ordered_map(
        lambda p: _run_one(cc, p, flags, cfg, callables), targets, getattr(cfg, "jobs", 1),
    )
    return _results_to_findings(cc, targets, sanitizer, results)


_SANITIZERS = (
    (_AS_FLAGS, "asan", "compiler has no ASan"),
    (_UB_FLAGS, "ubsan", "compiler has no UBSan"),
    (_TS_FLAGS, "tsan", "compiler has no TSan"),
)


def run_sanitize(paths: list[Path], cfg: Config) -> list[Finding]:
    """Probe ASan, UBSan, and TSan; compile+run each .c when the sanitizer is supported.

    Every (sanitizer, file) compile+run is independent, so they share one
    pool of cfg.jobs threads. Findings keep the serial order: ASan block,
    UBSan block, TSan block, files in input order within each.

    Law 9: NOTRUN without cfg.allow_exec; with it, only files holding a
    `// prism: run` function are compiled and run (inside prism.sandbox).
    """
    if not getattr(cfg, "allow_exec", False):
        return [sandbox.exec_notrun("sanitize", "sanitize (ASan/UBSan/TSan runs)")]
    cc = _find_cc()
    if not cc:
        return [_notrun("gcc/clang not on PATH", "ubsan")]

    supported = [(flags, name, _probe_sanitizer(cc, flags)) for flags, name, _ in _SANITIZERS]
    all_c = _c_units(paths)
    callables: dict[Path, str | None] = {p: _opted_in_callable(p) for p in all_c}
    c_files = [p for p in all_c if callables[p]]
    tasks = [
        (flags, p)
        for flags, _name, ok in supported if ok
        for p in c_files
    ]
    results = iter(ordered_map(
        lambda t: _run_one(cc, t[1], t[0], cfg, callables), tasks, getattr(cfg, "jobs", 1),
    ))

    out: list[Finding] = []
    for (flags, name, ok), (_f, _n, missing) in zip(supported, _SANITIZERS):
        if not ok:
            out.append(_notrun(missing, name))
        elif not all_c:
            out.append(_no_c_files(cc, name))
        elif not c_files:
            out.append(_no_opt_in(len(all_c), name))
        else:
            batch = [next(results) for _ in c_files]
            out.extend(_results_to_findings(cc, c_files, name, batch))
    return out
