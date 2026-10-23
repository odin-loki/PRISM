"""Config.which Microsoft Store python stub probe (parity with C++ Config::which).

A ``python`` under ...\\WindowsApps\\ may be the "install Python from the
Store" stub (exits 9009, runs nothing). It must read as a missing tool
(NOTRUN, Law 1), and a real python later on PATH must still be found.
The tests fake the platform and the probe, so they run on every OS.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import config
from prism.config import Config


def _proc(rc: int) -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess([], rc, b"", b"")


class StoreStubTest(unittest.TestCase):
    def setUp(self) -> None:
        config._STORE_STUB_CACHE.clear()
        self.addCleanup(config._STORE_STUB_CACHE.clear)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.apps = self.root / "WindowsApps"
        self.real = self.root / "real"
        for d in (self.apps, self.real):
            d.mkdir()
        self.stub = self.apps / "python.exe"
        self.stub.write_text("")
        self.good = self.real / "python.exe"
        self.good.write_text("")

    def _win(self):
        return mock.patch.object(config.sys, "platform", "win32")

    def test_stub_is_missing_tool_not_found(self):
        """Stub only on PATH: which() is None, so the stage is NOTRUN."""
        with self._win(), \
             mock.patch.object(config.shutil, "which", return_value=str(self.stub)), \
             mock.patch.object(config.subprocess, "run", return_value=_proc(9009)), \
             mock.patch.dict(os.environ, {"PATH": str(self.apps)}):
            self.assertIsNone(Config().which("python"))

    def test_stub_skipped_for_later_real_python(self):
        path = os.pathsep.join([str(self.apps), str(self.real)])
        with self._win(), \
             mock.patch.object(config.shutil, "which", return_value=str(self.stub)), \
             mock.patch.object(config.subprocess, "run", return_value=_proc(9009)) as run, \
             mock.patch.dict(os.environ, {"PATH": path}):
            self.assertEqual(Config().which("python"), str(self.good))
            # The real python is outside WindowsApps: never probed.
            probed = [c.args[0][0] for c in run.call_args_list]
            self.assertEqual(probed, [str(self.stub)])

    def test_real_store_python_accepted(self):
        with self._win(), \
             mock.patch.object(config.shutil, "which", return_value=str(self.stub)), \
             mock.patch.object(config.subprocess, "run", return_value=_proc(0)):
            self.assertEqual(Config().which("python"), str(self.stub))

    def test_probe_that_cannot_start_is_a_stub(self):
        with self._win(), \
             mock.patch.object(config.subprocess, "run", side_effect=OSError("x")):
            self.assertTrue(config._is_store_python_stub(self.stub))

    def test_probe_timeout_is_a_stub(self):
        with self._win(), mock.patch.object(
                config.subprocess, "run",
                side_effect=subprocess.TimeoutExpired("python", 10.0)):
            self.assertTrue(config._is_store_python_stub(self.stub))

    def test_probe_result_is_cached_per_path(self):
        with self._win(), \
             mock.patch.object(config.subprocess, "run", return_value=_proc(9009)) as run:
            self.assertTrue(config._is_store_python_stub(self.stub))
            self.assertTrue(config._is_store_python_stub(str(self.stub).upper()))
            self.assertEqual(run.call_count, 1)

    def test_only_python_under_windowsapps_is_probed(self):
        with self._win(), mock.patch.object(config.subprocess, "run") as run:
            self.assertFalse(config._is_store_python_stub(self.good))
            other = self.apps / "winget.exe"
            self.assertFalse(config._is_store_python_stub(other))
            run.assert_not_called()

    def test_not_probed_off_windows(self):
        with mock.patch.object(config.sys, "platform", "linux"), \
             mock.patch.object(config.subprocess, "run") as run:
            self.assertFalse(config._is_store_python_stub(self.stub))
            run.assert_not_called()

    def test_missing_tool_is_none(self):
        with mock.patch.object(config.shutil, "which", return_value=None):
            self.assertIsNone(Config().which("no-such-tool-prism"))


if __name__ == "__main__":
    unittest.main()
