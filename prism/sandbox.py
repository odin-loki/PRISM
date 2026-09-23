"""Law 9: executing code from the scanned tree requires --allow-exec.

One place for the exec policy and the process sandbox (the C++ engine's
twin is src/prism/sandbox.cpp, include/prism/sandbox.hpp).

* Policy. A stage that would run code derived from the scanned tree (a
  compiled harness, a sanitizer build, ``perl -c`` BEGIN blocks, a
  ``build.rs``, an ``eslint.config.js``) or from the LLM checks
  ``Config.allow_exec`` (stages that get a Config) or :func:`allowed`
  (stages that do not; the pipeline sets it for the run with
  :func:`policy`). Without the opt-in the step does not run and the stage
  writes :func:`exec_notrun` — NOTRUN, never CLEAN. The default is deny.
* Sandbox. With the opt-in, built binaries run through :func:`run_binary`:
  on Linux with a working ``bwrap`` the child runs in a bubblewrap jail
  (read-only /, private /tmp, only its scratch dir writable, no network,
  new namespaces) and always under rlimits (CPU, address space, open files,
  file size, no core). Without bwrap the rlimits still apply and findings
  record ``extra.sandbox = "rlimits-only"``.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from pathlib import Path
import shutil
import subprocess
import sys
import threading
from typing import Any

from prism import laws
from prism.models import Finding

try:  # POSIX only; imported up front so the preexec hook never imports.
    import resource
except ImportError:  # pragma: no cover - Windows
    resource = None  # type: ignore[assignment]

EXEC_FLAG = "--allow-exec"
EXEC_INSTALL = "re-run with --allow-exec (only on code you trust)"
EXEC_REASON = "executes-scanned-code"

# rlimits for a sandboxed child (same numbers in src/prism/sandbox.cpp).
LIMIT_AS_BYTES = 2 * 1024 * 1024 * 1024
LIMIT_NOFILE = 256
LIMIT_FSIZE_BYTES = 64 * 1024 * 1024


def exec_message(what: str) -> str:
    """The NOTRUN text for a step that would execute scanned code."""
    return f"{what}: executes code from the scanned tree; {EXEC_INSTALL}"


def exec_notrun(
    stage: str,
    what: str,
    *,
    file: str = "",
    function: str | None = None,
    line: int | None = None,
    strength: str = laws.STRENGTH_FINDS,
    **extra: Any,
) -> Finding:
    """NOTRUN for a step Law 9 kept from running (extra.install/extra.reason)."""
    return Finding(
        stage=stage, status=laws.NOTRUN, file=file, function=function, line=line,
        cls="", message=exec_message(what), strength=strength,
        extra={"install": EXEC_INSTALL, "reason": EXEC_REASON, **extra},
    )


# --- policy ---------------------------------------------------------------

_lock = threading.Lock()
_allowed = False


def allowed() -> bool:
    """True only while a run opted in with --allow-exec."""
    return _allowed


def set_allowed(value: bool) -> bool:
    """Set the process-wide policy; returns the previous value."""
    global _allowed
    with _lock:
        prev, _allowed = _allowed, bool(value)
    return prev


@contextmanager
def policy(allow: bool) -> Iterator[None]:
    """Run a block under an exec policy, restoring the previous one."""
    prev = set_allowed(allow)
    try:
        yield
    finally:
        set_allowed(prev)


# --- sandbox --------------------------------------------------------------

_bwrap_state: dict[str, str | None] = {}


def _bwrap_base(bwrap: str, scratch: Path) -> list[str]:
    s = str(scratch)
    return [
        bwrap, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
        "--tmpfs", "/tmp", "--bind", s, s, "--unshare-all", "--die-with-parent",
    ]


def bwrap_path() -> str | None:
    """A bwrap that actually starts a jail here (probed once), or None."""
    if not sys.platform.startswith("linux"):
        return None
    if "path" in _bwrap_state:
        return _bwrap_state["path"]
    exe = shutil.which("bwrap")
    ok: str | None = None
    if exe:
        try:
            r = subprocess.run(
                [*_bwrap_base(exe, Path("/")), "--", "true"],
                capture_output=True, timeout=10,
            )
            ok = exe if r.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            ok = None
    _bwrap_state["path"] = ok
    return ok


def sandbox_kind() -> str:
    """"bwrap", "rlimits-only" (POSIX without a working bwrap) or "none"."""
    if bwrap_path():
        return "bwrap"
    return "rlimits-only" if resource is not None else "none"


def wrap_argv(argv: Sequence[str], scratch: Path) -> list[str]:
    """argv inside the bubblewrap jail when bwrap works, else unchanged."""
    bw = bwrap_path()
    if not bw:
        return list(argv)
    return [*_bwrap_base(bw, Path(scratch).resolve()), "--", *argv]


def limits_preexec(cpu_seconds: float, *, limit_as: bool = True) -> Callable[[], None] | None:
    """setrlimit hook for the child (POSIX). ASan/TSan builds need limit_as=False."""
    if resource is None:
        return None
    # One second over the wall-clock timeout: the caller's timeout fires
    # first, so SIGXCPU never masquerades as a crash of the code under test.
    cpu = max(1, int(cpu_seconds + 0.999)) + 1
    res = resource

    def apply() -> None:
        pairs = [
            (res.RLIMIT_CPU, cpu, cpu + 1),
            (res.RLIMIT_NOFILE, LIMIT_NOFILE, LIMIT_NOFILE),
            (res.RLIMIT_FSIZE, LIMIT_FSIZE_BYTES, LIMIT_FSIZE_BYTES),
            (res.RLIMIT_CORE, 0, 0),
        ]
        if limit_as:
            pairs.append((res.RLIMIT_AS, LIMIT_AS_BYTES, LIMIT_AS_BYTES))
        for which, soft, hard in pairs:
            try:
                cur_soft, cur_hard = res.getrlimit(which)
                if cur_hard != res.RLIM_INFINITY:
                    hard = min(hard, cur_hard)
                    soft = min(soft, hard)
                res.setrlimit(which, (soft, hard))
            except (ValueError, OSError):
                pass

    return apply


def run_binary(
    argv: Sequence[str],
    *,
    scratch: Path,
    timeout: float,
    input: bytes | str | None = None,
    text: bool = False,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    limit_as: bool = True,
    merge_stderr: bool = False,
) -> subprocess.CompletedProcess:
    """Run a built binary under the sandbox. The caller has checked the policy.

    ``scratch`` is the only writable directory inside the jail (besides a
    private /tmp); the binary and its inputs should live there.
    """
    kw: dict[str, Any] = {
        "timeout": timeout, "cwd": cwd, "env": env, "input": input,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT if merge_stderr else subprocess.PIPE,
    }
    if text:
        kw.update(text=True, encoding="utf-8", errors="replace")
    pre = limits_preexec(timeout, limit_as=limit_as)
    if pre is not None:
        kw["preexec_fn"] = pre
    if sys.platform == "win32":
        kw["creationflags"] = int(getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000))
    return subprocess.run(wrap_argv(list(argv), Path(scratch)), **kw)


def stamp(findings: list[Finding], kind: str | None = None) -> list[Finding]:
    """Record extra.sandbox on findings that came from sandboxed runs."""
    k = kind or sandbox_kind()
    for f in findings:
        f.extra = {**(f.extra or {}), "sandbox": k}
    return findings
