"""CodeQL adapter honesty: missing is NOTRUN; silence is UNKNOWN; hits are FAILED.

CodeQL is STRENGTH_FINDS, never a proof. python -m unittest tests.test_codeql
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import _run_codeql, run_optional_tools
from helix.config import Config


def _no_adapter(*_a, **_k):
    return None


def _ok_proc():
    return mock.Mock(returncode=0, stdout="", stderr="")


def _sarif(results: list[dict]) -> str:
    return json.dumps({"runs": [{"results": results}]})


def _one_hit() -> dict:
    return {
        "ruleId": "cpp/use-after-free",
        "message": {"text": "use after free"},
        "locations": [{
            "physicalLocation": {
                "artifactLocation": {"uri": "foo.c"},
                "region": {"startLine": 3},
            },
        }],
    }


class TestCodeqlHonesty(unittest.TestCase):
    def _assert_finds_not_proof(self, f):
        self.assertEqual(f.stage, "codeql")
        self.assertEqual(f.strength, laws.STRENGTH_FINDS)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(f.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))

    def test_missing_codeql_is_notrun_never_clean(self):
        with mock.patch("helix.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("helix.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        codeql = next(f for f in findings if f.stage == "codeql")
        self.assertEqual(codeql.status, laws.NOTRUN)
        self.assertNotEqual(codeql.status, laws.CLEAN)
        self._assert_finds_not_proof(codeql)
        self.assertIn("not found", codeql.message)

    def test_no_database_is_unknown_never_proved(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no database", f.message.lower())
        self._assert_finds_not_proof(f)

    def test_sarif_hit_is_failed_never_proved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()

            def fake_run(cmd, timeout):
                Path(cmd[-1]).write_text(_sarif([_one_hit()]), encoding="utf-8")
                return _ok_proc()

            with mock.patch("helix.adapters_extra._run", side_effect=fake_run):
                findings = _run_codeql("codeql", [src], Config())
        self.assertTrue(findings)
        f = findings[0]
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.cls, "cpp/use-after-free")
        self._assert_finds_not_proof(f)
        self.assertFalse(any(x.status in {laws.PROVED, laws.CLEAN} for x in findings))

    def test_empty_sarif_is_unknown_not_a_proof(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()

            def fake_run(cmd, timeout):
                Path(cmd[-1]).write_text(_sarif([]), encoding="utf-8")
                return _ok_proc()

            with mock.patch("helix.adapters_extra._run", side_effect=fake_run):
                findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no results (not a proof)", f.message.lower())
        self._assert_finds_not_proof(f)

    def test_analyze_timeout_is_timeout_never_proved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()

            def fake_run(cmd, timeout):
                raise subprocess.TimeoutExpired(cmd, timeout)

            with mock.patch("helix.adapters_extra._run", side_effect=fake_run):
                findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertIn("timeout", f.message.lower())
        self._assert_finds_not_proof(f)

    def test_analyze_nonzero_is_error_not_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()
            fail = mock.Mock(returncode=2, stdout="", stderr="codeql: no queries")
            with mock.patch("helix.adapters_extra._run", return_value=fail):
                findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self._assert_finds_not_proof(f)

    def test_missing_sarif_is_unknown_not_a_proof(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()
            with mock.patch("helix.adapters_extra._run", return_value=_ok_proc()):
                findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no sarif", f.message.lower())
        self._assert_finds_not_proof(f)

    def test_doctest_analyze_is_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "foo.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (root / "codeql-db").mkdir()
            fake = mock.Mock(
                returncode=0,
                stdout="[doctest] doctest version is 2.4.11\nUnknown option: --timeout\n",
                stderr="",
            )
            with mock.patch("helix.adapters_extra._run", return_value=fake):
                findings = _run_codeql("codeql", [src], Config())
        self.assertEqual(findings[0].status, laws.NOTRUN)
        self.assertIn("not codeql", findings[0].message.lower())
        self._assert_finds_not_proof(findings[0])


if __name__ == "__main__":
    unittest.main()
