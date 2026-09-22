"""Optional AFL++ integration for FuSeBMC scalar harnesses."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from prism import laws
from prism.fuzz import _compile, harness_source, param_nbytes
from prism.models import Finding, FunctionInfo


def afl_available() -> str | None:
    """Return path to afl-fuzz if on PATH, else None."""
    for name in ("afl-fuzz", "afl-fuzz.exe"):
        p = shutil.which(name)
        if p:
            return p
    return None


def _compile_afl_harness(harness: Path, out_exe: Path) -> tuple[bool, str]:
    """Compile the AFL stdin harness.

    Sanitizer fallback (C++ ``compile_afl_harness`` must match these comments):
      1. -fsanitize=address,undefined  -fno-sanitize-recover=address,undefined
      2. -fsanitize=undefined          -fno-sanitize-recover=undefined
      3. -fsanitize=address            -fno-sanitize-recover=address
      4. bare (no sanitizer)

    Prefer ASan+UBSan together; fall back to one sanitizer, then bare.
    TSan cannot combine with ASan. Missing sanitizer runtime is not a fake
    CLEAN. Missing gcc/clang is mapped by the caller to NOTRUN, never ERROR.
    """
    return _compile(harness, out_exe)


def run_afl_fuzz(
    fn: FunctionInfo,
    src: Path,
    *,
    timeout: float = 2.0,
    work: Path | None = None,
) -> Finding | None:
    """Bounded AFL run on a scalar stdin harness.

    Returns a CRASH or CLEAN Finding with extra["engine"]="afl" only after
    AFL actually compiled and ran, or None if AFL is unavailable or the
    function is not SCALAR. Missing gcc/clang is NOTRUN (the AFL half did
    not run), never a fake CLEAN or a proof, and never extra["engine"]="afl".
    """
    afl = afl_available()
    if not afl or fn.kind != "SCALAR":
        return None

    base = dict(
        stage="fuse", file=fn.file, function=fn.name, line=fn.line,
        cls="", strength=laws.STRENGTH_FINDS,
    )

    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return Finding(
            **base, status=laws.NOTRUN,
            message="AFL: no C compiler on PATH",
            extra={"install": "install gcc or clang"},
        )

    extra: dict = {"engine": "afl"}
    cleanup = work is None
    work = work or Path(tempfile.mkdtemp(prefix="prism_afl_"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        src_copy = work / src.name
        if not src_copy.exists():
            src_copy.write_text(
                src.read_text(encoding="utf-8", errors="replace"), encoding="utf-8",
            )
        hpath = work / f"harness_{fn.name}.c"
        hpath.write_text(harness_source(fn, src.name), encoding="utf-8")
        exe = work / f"harness_{fn.name}.exe"
        ok, err = _compile_afl_harness(hpath, exe)
        if not ok:
            if "no c compiler" in (err or "").lower():
                return Finding(
                    **base, status=laws.NOTRUN,
                    message="AFL: no C compiler on PATH",
                    extra={"install": "install gcc or clang"},
                )
            return Finding(
                **base, status=laws.ERROR,
                message=f"AFL: compile failed: {err[:200]}",
                extra=extra,
            )

        in_dir = work / "in"
        out_dir = work / "out"
        in_dir.mkdir(exist_ok=True)
        nbytes = param_nbytes(fn.params)
        (in_dir / "seed").write_bytes(b"\x00" * nbytes)

        env = os.environ.copy()
        env.setdefault("AFL_NO_UI", "1")
        env.setdefault("AFL_SKIP_CPUFREQ", "1")
        env.setdefault("AFL_NO_AFFINITY", "1")

        try:
            subprocess.run(
                [
                    afl, "-i", str(in_dir), "-o", str(out_dir),
                    "-V", str(max(1, int(timeout))),
                    "--", str(exe),
                ],
                capture_output=True,
                timeout=timeout + 10,
                env=env,
                cwd=str(work),
            )
        except subprocess.TimeoutExpired:
            pass

        crash_dirs = [
            out_dir / "crashes",
            out_dir / "default" / "crashes",
        ]
        for cdir in crash_dirs:
            if not cdir.is_dir():
                continue
            crashes = [p for p in cdir.iterdir() if p.is_file() and p.name != "README.txt"]
            if crashes:
                data = crashes[0].read_bytes()
                rec = dict(base)
                rec["cls"] = "AFL-CRASH"
                return Finding(
                    **rec, status=laws.CRASH,
                    message=f"AFL crash on {data[:16].hex()}",
                    counterexample=data.hex(),
                    extra={**extra, "afl_crashes": len(crashes)},
                )

        return Finding(
            **base, status=laws.CLEAN,
            message=f"no AFL crash in {timeout:.0f}s (not a proof)",
            extra=extra,
        )
    finally:
        if cleanup:
            shutil.rmtree(work, ignore_errors=True)
