"""spatch / Coccinelle adapter honesty: missing binary is NOTRUN, never CLEAN.

Helix `_run_spatch` is law. Shipped rules live in helix/cocci/. File
presence is locked in tests/test_cocci.py; strcpy/strcat/sprintf staying
out of BMC `unencoded_syntax_reason` is locked in
tests/test_unencoded_contract.py.

C++ `run_spatch` in src/prism/adapters.cpp must match Helix: missing
spatch is NOTRUN via `run_optional_tools` / `notrun`, never CLEAN.

python -m unittest tests.test_spatch
"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import (
    OPTIONAL_TOOLS,
    _cocci_rules,
    _run_spatch,
    run_optional_tools,
)
from helix.config import Config, adapter_install
from helix.models import Finding

ROOT = Path(__file__).resolve().parents[1]
COCCI = ROOT / "helix" / "cocci"
_CPP = ROOT / "src" / "prism" / "adapters.cpp"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}

SHIPPED = (
    "memcpy_self",
    "realloc_self",
    "shift_bit31",
    "getenv_null",
    "strcpy_self",
    "sprintf_unbounded",
    "strcat_self",
    "strncpy_self",
)


def _no_adapter(*_a, **_k):
    return None


def _cpp_between(start_token: str, end_token: str) -> str:
    text = _CPP.read_text(encoding="utf-8")
    start = text.find(start_token)
    if start < 0:
        return ""
    rest = text[start:]
    end = rest.find(end_token)
    return rest if end < 0 else rest[:end]


class TestSpatchHonesty(unittest.TestCase):
    def _never_proof(self, findings: list[Finding]) -> None:
        self.assertTrue(findings)
        self.assertTrue(all(isinstance(f, Finding) for f in findings))
        for f in findings:
            self.assertEqual(f.stage, "spatch", f.stage)
            self.assertEqual(f.strength, laws.STRENGTH_FINDS, f.strength)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def test_cocci_rules_discovers_all_shipped_stems(self):
        names = {p.stem for p in _cocci_rules([])}
        for stem in SHIPPED:
            self.assertIn(stem, names, msg=f"helix/cocci/{stem}.cocci not discovered")
        self.assertTrue(COCCI.is_dir())

    def test_missing_spatch_via_run_optional_tools_is_notrun(self):
        with mock.patch("helix.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("helix.adapters_extra.shutil.which", return_value=None), \
             mock.patch("helix.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        self.assertIn("spatch", [spec[0] for spec in OPTIONAL_TOOLS])
        spatch = next(f for f in findings if f.stage == "spatch")
        self.assertEqual(spatch.status, laws.NOTRUN)
        self.assertNotEqual(spatch.status, laws.CLEAN)
        self.assertNotEqual(spatch.status, laws.PROVED)
        self.assertFalse(laws.is_proof(spatch.status))
        self.assertIn("not found", spatch.message)
        install = (spatch.extra or {}).get("install", "")
        self.assertEqual(install, adapter_install("spatch"))
        self.assertIn("SOURCES.md", install)
        self.assertIn("coccinelle", install)
        self._never_proof([spatch])

    def test_present_no_c_files_is_unknown_not_clean(self):
        out = _run_spatch(r"C:\tools\spatch.exe", [Path("notes.md")], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no .c files", out[0].message)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self._never_proof(out)

    def test_doctest_binary_is_notrun(self):
        c_files = list((ROOT / "testdata").glob("*.c"))[:1]
        self.assertTrue(c_files)
        proc = mock.Mock(
            returncode=0,
            stdout="[doctest] doctest version is 2.4.11\nUnknown option: --sp-file\n",
            stderr="",
        )
        with mock.patch("helix.adapters_extra._run", return_value=proc):
            out = _run_spatch(r"C:\tools\spatch.exe", c_files, Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.NOTRUN for f in out))
        self.assertIn("not coccinelle", out[0].message.lower())
        self._never_proof(out)

    def test_helix_run_spatch_source_never_clean_or_proved(self):
        py = inspect.getsource(_run_spatch)
        self.assertIn("laws.UNKNOWN", py)
        self.assertIn("laws.FAILED", py)
        self.assertIn("laws.TIMEOUT", py)
        self.assertIn("laws.ERROR", py)
        self.assertIn("STRENGTH_FINDS", py)
        self.assertIn("not a proof", py)
        self.assertIn("not a verdict", py)
        self.assertIn("no rules apply", py.lower())
        self.assertNotIn("laws.CLEAN", py)
        self.assertNotIn("laws.PROVED", py)
        opt = inspect.getsource(run_optional_tools)
        self.assertIn("_not_run(stage, names[0], install)", opt)
        self.assertIn("Never maps a missing binary to CLEAN", opt)

    def test_cpp_run_spatch_missing_binary_is_notrun_never_clean(self):
        stub = _cpp_between(
            "std::vector<Finding> run_spatch(",
            "std::string extract_json_object(",
        )
        self.assertTrue(stub, "adapters.cpp must define run_spatch")
        self.assertIn("laws::UNKNOWN", stub)
        self.assertIn("laws::FAILED", stub)
        self.assertIn("laws::TIMEOUT", stub)
        self.assertIn("laws::ERROR", stub)
        self.assertIn("STRENGTH_FINDS", stub)
        self.assertIn("not a proof", stub)
        self.assertIn("not a verdict", stub)
        self.assertIn("no rules apply", stub.lower())
        self.assertNotIn("laws::CLEAN", stub)
        self.assertNotIn("laws::PROVED", stub)
        self.assertNotIn("PROVED-UNBOUNDED", stub)

        dispatch = _cpp_between(
            "std::vector<Finding> dispatch_optional(",
            "struct OptionalTool",
        )
        self.assertTrue(dispatch, "adapters.cpp must define dispatch_optional")
        self.assertIn('if (stage == "spatch") return run_spatch(exe, paths, cfg);', dispatch)

        opt = _cpp_between(
            "std::vector<Finding> run_optional_tools(",
            "namespace {\n\n// Helix helix/pbsd.py",
        )
        self.assertTrue(opt, "adapters.cpp must define run_optional_tools")
        self.assertIn('{"spatch", {"spatch", "spatch.exe", nullptr}}', opt)
        self.assertIn("if (!exe)", opt)
        self.assertIn("notrun(tool.stage, first_name, install)", opt)
        self.assertNotIn("laws::CLEAN", opt)
        self.assertNotIn("laws::PROVED", opt)

        notrun = _cpp_between("Finding notrun(", "bool write_text(")
        self.assertTrue(notrun, "adapters.cpp must define notrun")
        self.assertIn("laws::NOTRUN", notrun)
        self.assertNotIn("laws::CLEAN", notrun)
        self.assertIn("not found", notrun)

        cocci = _cpp_between(
            "std::vector<fs::path> cocci_rules(",
            "struct SpatchHit",
        )
        self.assertTrue(cocci, "adapters.cpp must define cocci_rules")
        self.assertIn('"helix"', cocci)
        self.assertIn('"cocci"', cocci)
        self.assertIn(".cocci", cocci)


if __name__ == "__main__":
    unittest.main()
