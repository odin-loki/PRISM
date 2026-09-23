"""cppcheck adapter honesty: missing is NOTRUN; silence is not a proof.

prism.adapters.run_cppcheck is law. Missing binary is NOTRUN, never CLEAN.
Present with no C/C++ files is UNKNOWN. A real run with no <error> rows
is UNKNOWN (no diagnostics, not a proof) — not CLEAN, not PROVED, not
silence. Parsed errors are FAILED never PROVED. Unmatched exit not in
{0, 1} is ERROR. Fake doctest/Catch2 is NOTRUN. C++ run_cppcheck must
match: empty files UNKNOWN, unmatched rc ERROR.

python -m unittest tests.test_cppcheck
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters import run_cppcheck
from prism.config import Config, adapter_install
from prism.models import Finding

EXE = r"C:\tools\cppcheck.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _present(*_a, **_k):
    return EXE


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


def _empty_xml() -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<results version="2">\n'
        '  <cppcheck version="2.13.0"/>\n'
        "  <errors>\n"
        "  </errors>\n"
        "</results>\n"
    )


def _error_xml(
    eid: str = "nullPointer",
    sev: str = "error",
    msg: str = "Null pointer dereference",
    fil: str = "planted.c",
    line: int = 3,
) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<results version="2">\n'
        "  <errors>\n"
        f'    <error id="{eid}" severity="{sev}" msg="{msg}">\n'
        f'      <location file="{fil}" line="{line}"/>\n'
        "    </error>\n"
        "  </errors>\n"
        "</results>\n"
    )


class TestCppcheckHonesty(unittest.TestCase):
    def _never_proof(self, findings: list[Finding]) -> None:
        self.assertTrue(findings)
        self.assertTrue(all(isinstance(f, Finding) for f in findings))
        for f in findings:
            self.assertEqual(f.stage, "cppcheck", f.stage)
            self.assertEqual(f.strength, laws.STRENGTH_FINDS, f.strength)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _scan(self, paths, run_side_effect, resolve=_present):
        with mock.patch("prism.adapters.resolve_adapter", side_effect=resolve) as resolved, \
             mock.patch("prism.adapters.subprocess.run", side_effect=run_side_effect) as run:
            out = run_cppcheck(paths, Config())
        return out, run, resolved

    def test_missing_cppcheck_is_notrun_never_clean(self):
        with mock.patch("prism.adapters.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters.subprocess.run") as run:
            out = run_cppcheck([C_FILE], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cppcheck")
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(f.status))
        self.assertIn("not found", f.message)
        self.assertEqual((f.extra or {}).get("install"), adapter_install("cppcheck"))
        self.assertIn("third_party/MANIFEST.toml", (f.extra or {}).get("install", ""))
        self.assertIn("fetch_deps.py --tool cppcheck", (f.extra or {}).get("install", ""))
        self._never_proof(out)

    def test_present_no_c_files_is_unknown_not_silence(self):
        out, run, _ = self._scan(
            [Path("readme.md"), Path("notes.txt"), Path("unit.py")],
            lambda *_a, **_k: _proc(),
        )
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cppcheck")
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no c/c++ files", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.NOTRUN)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof(out)

        empty, run2, _ = self._scan([], lambda *_a, **_k: _proc())
        run2.assert_not_called()
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self._never_proof(empty)

    def test_present_empty_xml_is_unknown_not_clean_not_proof(self):
        # No <error> rows → UNKNOWN. Silence is not CLEAN and not a proof.
        out, run, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(stderr=_empty_xml()),
        )
        self.assertTrue(run.called)
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], EXE)
        self.assertIn("--enable=warning,style,performance,portability", cmd)
        self.assertIn("--xml", cmd)
        self.assertIn("--xml-version=2", cmd)
        self.assertIn(str(C_FILE), cmd)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no diagnostics (not a proof)", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.NOTRUN)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof(out)

    def test_unused_style_or_information_is_skipped_not_clean(self):
        xml = (
            '<?xml version="1.0"?>\n'
            "<errors>\n"
            '  <error id="unusedFunction" severity="style" msg="The function &apos;f&apos; is never used">\n'
            '    <location file="planted.c" line="1"/>\n'
            "  </error>\n"
            '  <error id="unusedVariable" severity="information" msg="Unused variable">\n'
            '    <location file="planted.c" line="2"/>\n'
            "  </error>\n"
            "</errors>\n"
        )
        out, run, _ = self._scan([C_FILE], lambda *_a, **_k: _proc(stderr=xml))
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no diagnostics (not a proof)", out[0].message.lower())
        self.assertNotIn(laws.CLEAN, {f.status for f in out})
        self.assertNotIn(laws.PROVED, {f.status for f in out})
        self._never_proof(out)

    def test_xml_error_is_failed_never_proved(self):
        xml = _error_xml(
            eid="nullPointer",
            sev="error",
            msg="Null pointer dereference",
            fil="planted.c",
            line=12,
        )
        out, run, _ = self._scan([C_FILE], lambda *_a, **_k: _proc(stderr=xml))
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cppcheck")
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.cls, "nullPointer")
        self.assertEqual(f.file, "planted.c")
        self.assertEqual(f.line, 12)
        self.assertEqual(f.message, "Null pointer dereference")
        self.assertEqual(f.strength, laws.STRENGTH_FINDS)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(any(x.status in {laws.PROVED, laws.CLEAN} for x in out))
        self._never_proof(out)

    def test_xml_entities_and_error_severity_unused_is_failed(self):
        xml = (
            "<errors>\n"
            '  <error id="compareError" severity="error" msg="a &lt; b &gt; c &apos;x&apos;">\n'
            '    <location file="planted.c" line="4"/>\n'
            "  </error>\n"
            '  <error id="unusedFunction" severity="error" msg="unused but error severity">\n'
            '    <location file="planted.c" line="8"/>\n'
            "  </error>\n"
            "</errors>\n"
        )
        out, _, _ = self._scan([C_FILE], lambda *_a, **_k: _proc(stderr=xml))
        self.assertEqual(len(out), 2)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].message, "a < b > c 'x'")
        self.assertEqual(out[1].cls, "unusedFunction")
        self._never_proof(out)

    def test_xml_on_stdout_when_stderr_empty(self):
        xml = _error_xml(eid="memleak", sev="warning", msg="Memory leak", line=9)
        out, _, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout=xml, stderr=""),
        )
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(out[0].cls, "memleak")
        self.assertEqual(out[0].line, 9)
        self._never_proof(out)

    def test_timeout_is_timeout_never_proved(self):
        def boom(cmd, **_k):
            raise subprocess.TimeoutExpired(cmd, 120)

        out, run, _ = self._scan([C_FILE], boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cppcheck")
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertIn("timeout", f.message.lower())
        self.assertEqual(f.strength, laws.STRENGTH_FINDS)
        self._never_proof(out)

    def test_accepted_suffixes_are_passed_others_dropped(self):
        paths = [
            Path("a.c"), Path("b.cc"), Path("c.cpp"), Path("d.cxx"),
            Path("e.h"), Path("f.hpp"), Path("skip.py"), Path("skip.md"),
        ]
        captured: list[list[str]] = []

        def fake_run(cmd, **_k):
            captured.append(list(cmd))
            return _proc(stderr=_empty_xml())

        out, run, _ = self._scan(paths, fake_run)
        self.assertTrue(run.called)
        cmd = captured[0]
        for name in ("a.c", "b.cc", "c.cpp", "d.cxx", "e.h", "f.hpp"):
            self.assertIn(name, cmd)
        self.assertNotIn("skip.py", cmd)
        self.assertNotIn("skip.md", cmd)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self._never_proof(out)

    def test_unmatched_nonzero_exit_is_error_not_silence(self):
        out, run, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(stderr="cppcheck: Failed to generate XML", rc=2),
        )
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cppcheck")
        self.assertEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self.assertIn("Failed to generate XML", f.message)
        self._never_proof(out)

    def test_shell_127_is_notrun_never_error(self):
        out, run, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(stderr="sh: 1: cppcheck: not found\n", rc=127),
        )
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof(out)

    def test_doctest_masquerade_is_notrun_never_error(self):
        out, _, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(
                stderr='[doctest] doctest version is "2.4.11"\n', rc=0,
            ),
        )
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertFalse(laws.is_proof(out[0].status))
        self._never_proof(out)

    def test_doctest_output_is_notrun_never_silence(self):
        doctest = (
            "[doctest] doctest version is \"2.4.11\"\n"
            "Unknown option: --timeout\n"
        )
        out, run, _ = self._scan(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout=doctest, stderr="", rc=0),
        )
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertIn("not cppcheck", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertFalse(laws.is_proof(f.status))
        self.assertEqual((f.extra or {}).get("install"), adapter_install("cppcheck"))
        self._never_proof(out)

    def test_statuses_are_never_is_proof(self):
        cases = [
            (lambda *_a, **_k: _proc(stderr=_empty_xml()), laws.UNKNOWN),
            (lambda *_a, **_k: _proc(stderr=_error_xml()), laws.FAILED),
            (lambda *_a, **_k: (_ for _ in ()).throw(subprocess.TimeoutExpired(["cppcheck"], 1)), laws.TIMEOUT),
        ]
        for side, want in cases:
            with self.subTest(want=want):
                out, _, _ = self._scan([C_FILE], side)
                self.assertEqual(out[0].status, want)
                self.assertFalse(laws.is_proof(out[0].status), out[0].status)
                self._never_proof(out)


ADAPTERS = Path(__file__).resolve().parents[1] / "src" / "prism" / "adapters.cpp"


def _brace_body(src: str, sig: str) -> str:
    i = src.find(sig)
    if i < 0:
        raise AssertionError(f"missing {sig!r} in {ADAPTERS.name}")
    brace = src.find("{", i)
    if brace < 0:
        raise AssertionError(f"no body after {sig!r}")
    depth = 0
    for j, ch in enumerate(src[brace:], brace):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace : j + 1]
    raise AssertionError(f"unbalanced braces after {sig!r}")


class TestCppCppcheckSourceContract(unittest.TestCase):
    """adapters.cpp run_cppcheck: empty files UNKNOWN; unmatched rc ERROR."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = ADAPTERS.read_text(encoding="utf-8")
        # The body is run_cppcheck_unstamped; run_cppcheck adds extra["tool_sha"].
        cls.body = _brace_body(cls.src, "std::vector<Finding> run_cppcheck_unstamped(")

    def test_empty_files_is_unknown_never_silence(self):
        self.assertIn("files.empty()", self.body)
        self.assertIn("no C/C++ files in scope", self.body)
        empty = _brace_body(self.body, "if (files.empty())")
        self.assertIn("laws::UNKNOWN", empty)
        self.assertNotIn("laws::CLEAN", empty)
        self.assertNotIn("return {};", empty)
        for proof in _PROOF:
            self.assertNotIn(proof, empty)

    def test_unmatched_rc_is_error(self):
        self.assertRegex(
            self.body,
            r"out\.empty\(\)\s*&&\s*r\.rc\s*!=\s*0\s*&&\s*r\.rc\s*!=\s*1",
        )
        unmatched = _brace_body(self.body, "if (out.empty() && r.rc != 0 && r.rc != 1)")
        self.assertIn("laws::ERROR", unmatched)
        self.assertNotIn("laws::CLEAN", unmatched)
        self.assertNotIn("laws::UNKNOWN", unmatched)
        for proof in _PROOF:
            self.assertNotIn(proof, unmatched)

    def test_empty_diagnostics_is_unknown_not_silence(self):
        self.assertIn("no diagnostics (not a proof)", self.body)
        after = self.body.split("no diagnostics (not a proof)", 1)[1][:400]
        self.assertIn("laws::UNKNOWN", self.body)
        self.assertNotIn("laws::CLEAN", after)
        for proof in _PROOF:
            self.assertNotIn(proof, after)

    def test_fake_doctest_is_notrun_never_unknown(self):
        self.assertIn("not cppcheck", self.body)
        self.assertIn("tool_unusable", self.body)
        self.assertIn("laws::NOTRUN", self.body)
        fake = self.body[self.body.find("tool_unusable") : self.body.find("r.rc != 0 && r.rc != 1")]
        self.assertIn("laws::NOTRUN", fake)
        self.assertNotIn("laws::CLEAN", fake)
        self.assertNotIn("laws::UNKNOWN", fake)
        for proof in _PROOF:
            self.assertNotIn(proof, fake)


if __name__ == "__main__":
    unittest.main()
