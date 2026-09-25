"""Run a command so that a timeout or Ctrl-C kills its whole process tree.

``subprocess.run(..., timeout=...)`` kills only the direct child. PRISM
starts solvers, proof checkers and compilers (CaDiCaL, cake_lpr, clang, the
LRAT checkers), each in a process group of its own so that PRISM can kill
it; killing PRISM alone leaves them running, still using CPU and memory,
after the scorer has moved on.

``run`` starts the command in a new session (``setsid``). Every descendant
stays in that session unless it calls ``setsid`` itself (``setpgid`` changes
only the process group), so on a timeout, on Ctrl-C (``KeyboardInterrupt``)
or on any other exception ``kill_tree`` sends ``SIGKILL`` to the session's
process group and, on Linux, to every process whose session id is the
command's (``/proc/<pid>/stat``). Processes that leave the session
(``bwrap --new-session``) are expected to use ``--die-with-parent``.

The scorers load this file by path (``load()``), not through ``sys.path``.
"""

from __future__ import annotations

import os
import signal
import subprocess
from typing import Any

_POSIX = os.name == "posix"


def session_members(sid: int) -> list[int]:
    """PIDs whose session id is ``sid`` (Linux ``/proc``; empty elsewhere)."""
    out: list[int] = []
    try:
        names = os.listdir("/proc")
    except OSError:
        return out
    for n in names:
        if not n.isdigit():
            continue
        try:
            with open(f"/proc/{n}/stat", "rb") as fh:
                stat = fh.read().decode("latin-1")
        except OSError:
            continue
        # comm (field 2) may contain blanks and ')': split after the last ')'
        rest = stat[stat.rfind(")") + 2:].split()
        # rest[0] state, [1] ppid, [2] pgrp, [3] session
        if len(rest) > 3 and rest[3] == str(sid):
            out.append(int(n))
    return out


def kill_tree(proc: subprocess.Popen[Any]) -> None:
    """SIGKILL the session ``proc`` leads: its process group, then every other member."""
    if not _POSIX:
        proc.kill()
        return
    sid = proc.pid
    try:
        os.killpg(sid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    # members that moved to process groups of their own (PRISM's solver children)
    for _ in range(3):
        left = [p for p in session_members(sid) if p != os.getpid()]
        if not left:
            break
        for p in left:
            try:
                os.kill(p, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
    try:
        proc.kill()
    except ProcessLookupError:
        pass


def run(argv: list[str], *, timeout: float | None = None, input: Any = None,
        capture_output: bool = False, **kw: Any) -> subprocess.CompletedProcess[Any]:
    """``subprocess.run`` with the same arguments, killing the whole tree on timeout or interrupt.

    Raises ``subprocess.TimeoutExpired`` like ``subprocess.run``.
    """
    if capture_output:
        kw.setdefault("stdout", subprocess.PIPE)
        kw.setdefault("stderr", subprocess.PIPE)
    if input is not None:
        kw["stdin"] = subprocess.PIPE
    if _POSIX:
        kw["start_new_session"] = True
    proc = subprocess.Popen(argv, **kw)
    try:
        out, err = proc.communicate(input, timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_tree(proc)
        out, err = _drain(proc)
        raise subprocess.TimeoutExpired(argv, timeout or 0.0, output=out, stderr=err) from None
    except BaseException:  # KeyboardInterrupt, SystemExit, ...
        kill_tree(proc)
        _drain(proc)
        raise
    return subprocess.CompletedProcess(argv, proc.returncode, out, err)


def _drain(proc: subprocess.Popen[Any]) -> tuple[Any, Any]:
    # a descendant outside the session could still hold a pipe open: bounded wait
    try:
        return proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        for f in (proc.stdout, proc.stderr, proc.stdin):
            if f is not None:
                try:
                    f.close()
                except OSError:
                    pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        return None, None
