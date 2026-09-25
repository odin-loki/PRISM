"""The scorers kill PRISM's whole process tree on a timeout or Ctrl-C.

PRISM runs its solvers and proof checkers (CaDiCaL, cake_lpr, the LRAT
checkers) and compilers each in a process group of its own. Killing only
PRISM, or only PRISM's process group, left them running after the scorer had
moved on. `tools/proctree.py` (used by `tools/conformance.py` and
`tools/svcomp/run_subset.py`) and `run_tree` in `tools/svcomp/prism_svcomp.py`
(shipped on its own, so it has its own copy) kill every process of the
session instead.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]


def _load(name: str, path: Path) -> Any:
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


PT = _load("prism_tools_proctree", REPO / "tools" / "proctree.py")

# A stand-in for PRISM: starts a "solver" in a process group of its own (as
# src/prism/solver/util.cpp does), writes the solver's pid to a file, then
# waits for it.
FAKE_PRISM = r"""
import os, subprocess, sys, time
p = subprocess.Popen(["sleep", "60"], process_group=0) if sys.version_info >= (3, 11) else \
    subprocess.Popen(["sleep", "60"], preexec_fn=lambda: os.setpgid(0, 0))
with open(sys.argv[1], "w") as fh:
    fh.write(str(p.pid))
time.sleep(60)
"""


def _gone(pid: int, wait: float = 5.0) -> bool:
    """The process exited (absent, or a zombie waiting for its reaper)."""
    end = time.monotonic() + wait
    while time.monotonic() < end:
        try:
            with open(f"/proc/{pid}/stat", encoding="latin-1") as fh:
                stat = fh.read()
        except OSError:
            return True
        if stat[stat.rfind(")") + 2:].startswith("Z"):
            return True
        time.sleep(0.05)
    return False


def _solver_pid(path: Path, wait: float = 10.0) -> int:
    end = time.monotonic() + wait
    while time.monotonic() < end:
        try:
            text = path.read_text().strip()
            if text:
                return int(text)
        except OSError:
            pass
        time.sleep(0.05)
    raise AssertionError("fake prism did not start its solver")


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux /proc and sessions")
class ProcTreeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="prism-proctree-"))
        self.script = self.tmp / "fake_prism.py"
        self.script.write_text(FAKE_PRISM, encoding="utf-8")
        self.pidfile = self.tmp / "solver.pid"

    def tearDown(self) -> None:
        try:
            pid = int(self.pidfile.read_text().strip())
        except (OSError, ValueError):
            pid = 0
        if pid > 1:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_session_members_sees_other_process_groups(self) -> None:
        proc = subprocess.Popen([sys.executable, str(self.script), str(self.pidfile)], start_new_session=True)
        try:
            solver = _solver_pid(self.pidfile)
            self.assertNotEqual(os.getpgid(solver), proc.pid)  # its own group ...
            self.assertIn(solver, PT.session_members(proc.pid))  # ... in the same session
        finally:
            PT.kill_tree(proc)
            proc.wait()
        self.assertTrue(_gone(solver))

    def test_timeout_kills_the_solver_in_its_own_group(self) -> None:
        t0 = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            PT.run([sys.executable, str(self.script), str(self.pidfile)], capture_output=True, timeout=2)
        self.assertLess(time.monotonic() - t0, 30)
        self.assertTrue(_gone(_solver_pid(self.pidfile)))

    def test_interrupt_kills_the_tree(self) -> None:
        # Ctrl-C while waiting: KeyboardInterrupt inside communicate()
        def boom(_sig: int, _frm: Any) -> None:
            raise KeyboardInterrupt

        old = signal.signal(signal.SIGALRM, boom)
        try:
            signal.setitimer(signal.ITIMER_REAL, 2.0)
            with self.assertRaises(KeyboardInterrupt):
                PT.run([sys.executable, str(self.script), str(self.pidfile)], capture_output=True, timeout=60)
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, old)
        self.assertTrue(_gone(_solver_pid(self.pidfile)))

    def test_normal_exit_returns_output(self) -> None:
        r = PT.run([sys.executable, "-c", "print('ok')"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout.strip(), "ok")

    def test_conformance_run_prism_timeout(self) -> None:
        conf = _load("prism_conformance_under_test", REPO / "tools" / "conformance.py")
        src = self.tmp / "t.c"
        src.write_text("int f(int x) { return x; }\n", encoding="utf-8")
        task = conf.Task(ident="t", yml=src, source=src, origin="prism", category="x", lang="C", prop="p",
                         expected={"f": True})
        # the fake prism ignores PRISM's arguments after its first
        res = conf.run_prism([sys.executable, str(self.script), str(self.pidfile)], task, ["pir"],
                             self.tmp / "work", 2.0, None)
        self.assertEqual(res.get("error"), "TIMEOUT")
        self.assertTrue(_gone(_solver_pid(self.pidfile)))

    def test_svcomp_wrapper_timeout(self) -> None:
        wrapper = _load("prism_svcomp_wrapper_proctree", REPO / "tools" / "svcomp" / "prism_svcomp.py")
        with self.assertRaises(subprocess.TimeoutExpired):
            wrapper.run_tree([sys.executable, str(self.script), str(self.pidfile)], 2.0)
        self.assertTrue(_gone(_solver_pid(self.pidfile)))


if __name__ == "__main__":
    unittest.main()
