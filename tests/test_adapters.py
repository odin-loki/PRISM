"""Optional adapters: a missing tool is NOTRUN, never CLEAN.

python -m unittest tests.test_adapters
"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import OPTIONAL_TOOLS, _run_cbmc, run_optional_tools
from helix.config import Config
from helix.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


class TestOptionalAdapters(unittest.TestCase):
    def test_signature_returns_list_of_findings(self):
        with mock.patch("helix.adapters_extra.shutil.which", return_value=None):
            out = run_optional_tools([], Config())
        self.assertIsInstance(out, list)
        self.assertTrue(out)
        self.assertTrue(all(isinstance(f, Finding) for f in out))

    def test_every_missing_tool_is_notrun(self):
        with mock.patch("helix.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([TD], Config())
        by_stage = {f.stage: f for f in findings}
        expected = [spec[0] for spec in OPTIONAL_TOOLS] + ["libfuzzer"]
        self.assertEqual(expected, [
            "klee", "afl-fuzz", "frama-c", "infer",
            "codeql", "clang-tidy", "cbmc", "strix",
            "semgrep", "spatch", "libfuzzer",
        ])
        for name in expected:
            self.assertIn(name, by_stage, msg=f"missing finding for {name}")
            f = by_stage[name]
            self.assertEqual(f.status, laws.NOTRUN, msg=f"{name} status={f.status}")
            self.assertNotEqual(f.status, laws.CLEAN)
            install = (f.extra or {}).get("install", "")
            self.assertTrue(install, msg=f"{name} has no install URL")
            self.assertTrue(
                install.startswith("http://") or install.startswith("https://"),
                msg=f"{name} install is not a URL: {install}",
            )
            self.assertIn("not on PATH", f.message)

    def test_missing_is_never_clean_even_with_c_files(self):
        c_files = list(TD.glob("*.c"))[:2]
        with mock.patch("helix.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools(c_files, Config())
        statuses = {f.status for f in findings}
        self.assertEqual(statuses, {laws.NOTRUN})
        self.assertNotIn(laws.CLEAN, statuses)

    def test_present_help_is_not_a_code_verdict(self):
        def fake_which(name: str):
            if name == "klee":
                return r"C:\tools\klee.exe"
            return None

        help_proc = mock.Mock(returncode=0, stdout="KLEE --help", stderr="")
        with mock.patch("helix.adapters_extra.shutil.which", side_effect=fake_which), \
             mock.patch("helix.adapters_extra._run", return_value=help_proc):
            findings = run_optional_tools([], Config())
        klee = next(f for f in findings if f.stage == "klee")
        self.assertNotEqual(klee.status, laws.CLEAN)
        self.assertNotEqual(klee.status, laws.NOTRUN)
        missing = [f for f in findings if f.stage != "klee"]
        self.assertTrue(missing)
        self.assertTrue(all(f.status == laws.NOTRUN for f in missing))

    def test_semgrep_match_is_failed(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        result_json = (
            '{"results":[{"check_id":"c.lang.security","path":"test.c",'
            '"start":{"line":3},"extra":{"message":"use-after-free"}}]}'
        )
        help_proc = mock.Mock(returncode=0, stdout="semgrep --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout=result_json, stderr="")

        def fake_which(name: str):
            if name in ("semgrep", "semgrep.exe"):
                return r"C:\tools\semgrep.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("helix.adapters_extra.shutil.which", side_effect=fake_which), \
             mock.patch("helix.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.FAILED)
        self.assertEqual(semgrep.cls, "c.lang.security")

    def test_semgrep_empty_results_is_unknown_not_clean(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        result_json = '{"results":[]}'
        help_proc = mock.Mock(returncode=0, stdout="semgrep --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout=result_json, stderr="")

        def fake_which(name: str):
            if name in ("semgrep", "semgrep.exe"):
                return r"C:\tools\semgrep.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("helix.adapters_extra.shutil.which", side_effect=fake_which), \
             mock.patch("helix.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.UNKNOWN)
        self.assertNotEqual(semgrep.status, laws.CLEAN)
        self.assertNotEqual(semgrep.status, laws.PROVED)
        self.assertIn("no matches", semgrep.message.lower())

    def test_spatch_match_is_failed(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        help_proc = mock.Mock(returncode=0, stdout="spatch --help", stderr="")
        match_proc = mock.Mock(
            returncode=0,
            stdout=f"{c_files[0]}:12: realloc(p, n)\n",
            stderr="",
        )

        def fake_which(name: str):
            if name in ("spatch", "spatch.exe"):
                return r"C:\tools\spatch.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return match_proc

        with mock.patch("helix.adapters_extra.shutil.which", side_effect=fake_which), \
             mock.patch("helix.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        spatch = [f for f in findings if f.stage == "spatch" and f.status == laws.FAILED]
        self.assertTrue(spatch)
        self.assertIn("realloc_self", {f.cls for f in spatch})

    def test_spatch_empty_is_unknown_not_clean(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        help_proc = mock.Mock(returncode=0, stdout="spatch --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout="", stderr="")

        def fake_which(name: str):
            if name in ("spatch", "spatch.exe"):
                return r"C:\tools\spatch.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("helix.adapters_extra.shutil.which", side_effect=fake_which), \
             mock.patch("helix.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        spatch = next(f for f in findings if f.stage == "spatch")
        self.assertEqual(spatch.status, laws.UNKNOWN)
        self.assertNotEqual(spatch.status, laws.CLEAN)
        self.assertNotEqual(spatch.status, laws.PROVED)
        self.assertIn("no matches", spatch.message.lower())

    def test_cbmc_refuses_no_bounds_check(self):
        paths = list(TD.glob("*.c"))[:1]
        self.assertTrue(paths)
        cmd = [r"C:\tools\cbmc.exe", str(paths[0]), "--no-bounds-check"]
        with self.assertRaises(ValueError) as ctx:
            for flag in cmd:
                if flag.startswith("--no-") and flag.endswith("-check"):
                    raise ValueError(f"refusing to disable a check: {flag}")
        self.assertIn("--no-bounds-check", str(ctx.exception))

        cmds: list[list[str]] = []
        proc = mock.Mock(returncode=0, stdout="VERIFICATION SUCCESSFUL", stderr="")

        def capture(cmd, timeout):
            cmds.append(list(cmd))
            return proc

        with mock.patch("helix.adapters_extra._run", side_effect=capture):
            _run_cbmc(r"C:\tools\cbmc.exe", paths, Config())
        for built in cmds:
            for flag in built:
                self.assertFalse(
                    flag.startswith("--no-") and flag.endswith("-check"),
                    msg=f"cbmc must not pass check-disabling flags: {flag}",
                )


if __name__ == "__main__":
    unittest.main()
