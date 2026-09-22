"""semgrep adapter honesty: missing is NOTRUN; silence is UNKNOWN; hits are FAILED.

`_run_semgrep` is STRENGTH_FINDS, never a proof. Empty results are not CLEAN.
Invalid `p/c` falls back to `auto`. python -m unittest tests.test_semgrep
"""

from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters_extra import _extract_json_object, _run_semgrep, run_optional_tools
from prism.config import Config
from prism.models import Finding

EXE = r"C:\tools\semgrep.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


def _empty_results() -> str:
    return json.dumps({"results": []})


def _one_hit() -> str:
    return json.dumps({
        "results": [{
            "check_id": "c.lang.security",
            "path": "planted.c",
            "start": {"line": 3},
            "extra": {"message": "use-after-free"},
        }],
    })


class TestSemgrepHonesty(unittest.TestCase):
    def _never_proof(self, findings: list[Finding]) -> None:
        self.assertTrue(findings)
        self.assertTrue(all(isinstance(f, Finding) for f in findings))
        for f in findings:
            self.assertEqual(f.stage, "semgrep", f.stage)
            self.assertEqual(f.strength, laws.STRENGTH_FINDS, f.strength)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _scan(self, run_side_effect):
        with mock.patch("prism.adapters_extra._run", side_effect=run_side_effect) as run:
            out = _run_semgrep(EXE, [C_FILE], Config())
        return out, run

    def test_missing_semgrep_via_run_optional_tools_is_notrun_never_clean(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None), \
             mock.patch("prism.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.NOTRUN)
        self.assertNotEqual(semgrep.status, laws.CLEAN)
        self.assertNotEqual(semgrep.status, laws.PROVED)
        self.assertFalse(laws.is_proof(semgrep.status))
        self.assertIn("not found", semgrep.message)
        self._never_proof([semgrep])

    def test_empty_results_json_is_unknown_not_a_proof(self):
        out, run = self._scan(lambda *_a, **_k: _proc(stdout=_empty_results()))
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertIn("no matches (not a proof)", f.message)
        self._never_proof(out)

    def test_present_no_c_files_is_unknown(self):
        with mock.patch("prism.adapters_extra._run") as run:
            out = _run_semgrep(EXE, [Path("notes.md"), Path("unit.py")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no c/c++ files", out[0].message.lower())
        self._never_proof(out)

    def test_json_on_stderr_only_is_parsed(self):
        """Python engine law: combined stdout+stderr, matching C++ parse_semgrep(r.text)."""
        out, run = self._scan(lambda *_a, **_k: _proc(stdout="", stderr=_one_hit()))
        self.assertTrue(run.called)
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].cls, "c.lang.security")
        self.assertIn("use-after-free", out[0].message)
        self._never_proof(out)

        empty, _ = self._scan(lambda *_a, **_k: _proc(stdout="", stderr=_empty_results()))
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self.assertNotEqual(empty[0].status, laws.CLEAN)
        self._never_proof(empty)

    def test_json_wrapped_in_combined_text_is_parsed(self):
        noise = "WARN: semgrep is experimental\n"
        wrapped = noise + _one_hit() + "\ntrailing log\n"
        out, _ = self._scan(lambda *_a, **_k: _proc(stdout=noise, stderr=_one_hit()))
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].cls, "c.lang.security")
        self._never_proof(out)

        empty_wrap, _ = self._scan(
            lambda *_a, **_k: _proc(stdout=noise, stderr=_empty_results() + "\n"),
        )
        self.assertEqual(empty_wrap[0].status, laws.UNKNOWN)
        self.assertIn("no matches (not a proof)", empty_wrap[0].message)
        self._never_proof(empty_wrap)

        self.assertEqual(
            _extract_json_object(wrapped),
            _one_hit(),
        )
        self.assertEqual(_extract_json_object(""), "{}")
        self.assertEqual(_extract_json_object("   \n"), "{}")

    def test_doctest_binary_is_notrun_never_error(self):
        out, run = self._scan(lambda *_a, **_k: _proc(
            stdout="[doctest] doctest version is 2.4.11\nUnknown option: --timeout\n",
            rc=0,
        ))
        self.assertTrue(run.called)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not semgrep", out[0].message.lower())
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self._never_proof(out)

    def test_garbage_combined_text_is_error_not_clean(self):
        out, _ = self._scan(lambda *_a, **_k: _proc(stdout="", stderr="fatal: boom", rc=2))
        self.assertEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self._never_proof(out)

    def test_one_results_hit_is_failed_never_proved(self):
        out, run = self._scan(lambda *_a, **_k: _proc(stdout=_one_hit()))
        self.assertTrue(run.called)
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].cls, "c.lang.security")
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertFalse(any(x.status in {laws.PROVED, laws.CLEAN} for x in out))
        self._never_proof(out)

    def test_invalid_p_c_falls_back_to_auto_empty_is_unknown(self):
        calls: list[list[str]] = []

        def fake_run(cmd, timeout):
            calls.append(list(cmd))
            if "--config" in cmd and cmd[cmd.index("--config") + 1] == "p/c":
                return _proc(
                    stdout="",
                    stderr="Invalid configuration: could not find config p/c",
                    rc=2,
                )
            return _proc(stdout=_empty_results())

        out, run = self._scan(fake_run)
        self.assertGreaterEqual(run.call_count, 2)
        configs = []
        for cmd in calls:
            self.assertIn("--config", cmd)
            configs.append(cmd[cmd.index("--config") + 1])
        self.assertEqual(configs[:2], ["p/c", "auto"])
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no matches (not a proof)", out[0].message)
        self._never_proof(out)

    def test_timeout_is_timeout(self):
        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        out, run = self._scan(boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertIn("timeout", f.message.lower())
        self._never_proof(out)

    def test_ruleset_download_failure_is_notrun_never_error(self):
        def fake_run(cmd, timeout):
            return _proc(
                stdout="",
                stderr="failed to download ruleset: HTTP 503\n",
                rc=2,
            )

        out, _ = self._scan(fake_run)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(out[0].status))
        self._never_proof(out)

    def test_semgrep_statuses_are_never_is_proof(self):
        cases = [
            ([_proc(stdout=_empty_results())], laws.UNKNOWN),
            ([_proc(stdout=_one_hit())], laws.FAILED),
            ([subprocess.TimeoutExpired(["semgrep"], 1)], laws.TIMEOUT),
        ]
        for side, want in cases:
            with self.subTest(want=want):
                def fake(cmd, timeout, _side=side):
                    effect = _side[0]
                    if isinstance(effect, Exception):
                        raise effect
                    return effect

                out, _ = self._scan(fake)
                self.assertEqual(out[0].status, want)
                self.assertFalse(laws.is_proof(out[0].status), out[0].status)
                self._never_proof(out)


if __name__ == "__main__":
    unittest.main()
