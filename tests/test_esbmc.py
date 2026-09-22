"""ESBMC adapter: missing binary is NOTRUN, never a proof.

Python engine `run_esbmc` is law. Missing esbmc is NOTRUN (the binary did not
run). A present ESBMC that prints VERIFICATION SUCCESSFUL is recorded
as the adapter maps it — never folded into in-tree BMC PROVED-UNBOUNDED.

Mapping in prism/adapters.py `run_esbmc` (test the code, not a wish):
    VERIFICATION SUCCESSFUL, default cmd (no --k-induction):
        UNWINDING ASSERTION in output → laws.BOUNDED
        else → laws.PROVED
    VERIFICATION FAILED     → laws.FAILED
    VERIFICATION UNKNOWN    → laws.UNKNOWN
    else                    → laws.ERROR

SUCCESSFUL → PROVED is allowed for this adapter because ESBMC actually
ran. Missing esbmc must not. PROVED-UNBOUNDED is in-tree BMC k-induction
unless the cmd actually carries --k-induction (it does not).

python -m unittest tests.test_esbmc
"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters import run_esbmc
from prism.config import Config
from prism.laws import refuse_merge

EXE = r"C:\tools\esbmc.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}
_DISABLE = ("--no-bounds-check", "--no-div-by-zero-check")


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestEsbmcAdapter(unittest.TestCase):
    def _esbmc(self, paths, run_side_effect, exe=EXE, cfg=None):
        cfg = cfg or Config()
        with mock.patch("prism.adapters.resolve_adapter", return_value=exe), \
             mock.patch("prism.adapters.subprocess.run", side_effect=run_side_effect) as run:
            out = run_esbmc(paths, cfg)
        return out, run

    def test_missing_esbmc_is_notrun_never_proved(self):
        with mock.patch("prism.adapters.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters.subprocess.run") as run:
            findings = run_esbmc([C_FILE], Config())
        run.assert_not_called()
        self.assertTrue(findings)
        f = findings[0]
        self.assertEqual(f.stage, "esbmc")
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertIn("not found", f.message)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(f.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotIn(f.status, _PROOF)
        self.assertFalse(laws.is_proof(f.status))

    def test_verification_successful_maps_prism_never_proved_unbounded(self):
        # Default cmd has --unwind and no --k-induction. Python engine therefore
        # records SUCCESSFUL as PROVED, not in-tree BMC PROVED-UNBOUNDED.
        out, run = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="VERIFICATION SUCCESSFUL\n"),
        )
        run.assert_called_once()
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], EXE)
        self.assertIn(str(C_FILE), cmd)
        self.assertIn("--unwind", cmd)
        self.assertNotIn("--k-induction", cmd)
        self.assertIn("--overflow-check", cmd)
        self.assertIn("--memory-leak-check", cmd)
        for flag in cmd:
            self.assertFalse(
                flag.startswith("--no-") and flag.endswith("-check"),
                msg=f"esbmc must not pass check-disabling flags: {flag}",
            )
        for bad in _DISABLE:
            self.assertNotIn(bad, cmd)

        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "esbmc")
        self.assertEqual(f.status, laws.PROVED)
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertEqual(f.message, "ESBMC: " + laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(f.status, laws.BOUNDED)
        with self.assertRaises(ValueError) as ctx:
            refuse_merge(f.status, laws.PROVED_UNBOUNDED)
        self.assertIn("refusing to merge", str(ctx.exception))

    def test_present_no_c_files_is_unknown_never_clean_or_proved(self):
        # Present binary, nothing to analyse: UNKNOWN (empty success), not
        # silence, not CLEAN, not PROVED. Same honesty as the Python engine `_run_cbmc`.
        with mock.patch("prism.adapters.resolve_adapter", return_value=EXE), \
             mock.patch("prism.adapters.subprocess.run") as run:
            out = run_esbmc([Path("unit.cpp"), Path("hdr.h")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "esbmc")
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertIn("no .c files", f.message)
        self.assertIn(EXE, f.message)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.NOTRUN)
        self.assertFalse(laws.is_proof(f.status))

        empty, run2 = self._esbmc([], lambda *_a, **_k: _proc())
        run2.assert_not_called()
        self.assertEqual(len(empty), 1)
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self.assertIn("no .c files", empty[0].message)
        self.assertNotEqual(empty[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(empty[0].status))

    def test_verification_failed_is_failed(self):
        out, run = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="VERIFICATION FAILED\n", rc=1),
        )
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "esbmc")
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.message, "ESBMC verification failed")
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertFalse(laws.is_proof(f.status))

    def test_refuses_no_bounds_check_and_no_div_by_zero_check(self):
        src = inspect.getsource(run_esbmc)
        self.assertIn('startswith("--no-")', src)
        self.assertIn('endswith("-check")', src)
        self.assertIn("refusing to disable a check", src)
        self.assertIn("--no-bounds-check", src)

        # Execute that loop: cmd is `[exe, path, "--unwind", str(cfg.unwind),
        # "--overflow-check", ...]`. A disabling flag in unwind becomes a
        # cmd token and must abort before subprocess.run.
        for flag in _DISABLE:
            with self.subTest(flag=flag):
                cfg = mock.Mock(unwind=flag, timeout=30.0)
                with mock.patch("prism.adapters.resolve_adapter", return_value=EXE), \
                     mock.patch("prism.adapters.subprocess.run") as run:
                    with self.assertRaises(ValueError) as ctx:
                        run_esbmc([C_FILE], cfg)
                run.assert_not_called()
                self.assertIn("refusing to disable a check", str(ctx.exception))
                self.assertIn(flag, str(ctx.exception))

    def test_doctest_or_catch2_masquerade_is_notrun_never_error(self):
        cases = (
            ('[doctest] doctest version is "2.4.11"\n', 0),
            ("Catch2 v3.5.0\n", 0),
            ("sh: 1: esbmc: not found\n", 127),
        )
        for blob, rc in cases:
            with self.subTest(blob=blob[:20], rc=rc):
                def fake(*_a, _blob=blob, _rc=rc, **_k):
                    return _proc(stderr=_blob, rc=_rc)
                out, _ = self._esbmc([C_FILE], fake)
                self.assertEqual(len(out), 1)
                f = out[0]
                self.assertEqual(f.status, laws.NOTRUN)
                self.assertNotEqual(f.status, laws.ERROR)
                self.assertNotEqual(f.status, laws.PROVED)
                self.assertNotEqual(f.status, laws.CLEAN)
                self.assertFalse(laws.is_proof(f.status))
                self.assertIn("not ESBMC", f.message)

    def test_oserror_is_notrun_never_error(self):
        out, _ = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: (_ for _ in ()).throw(OSError("exec format error")),
        )
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertIn("unusable", out[0].message)
        self.assertFalse(laws.is_proof(out[0].status))

    def test_doctest_binary_is_notrun_never_error_or_proved(self):
        out, run = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(
                stdout="",
                stderr="Unknown option: --timeout\n[doctest] doctest version is 2.4.11\n",
                rc=1,
            ),
        )
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertIn("not ESBMC", f.message)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self.assertIn("third_party/esbmc", (f.extra or {}).get("install", ""))

    def test_verification_unknown_and_no_verdict_are_not_proofs(self):
        unknown, _ = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="VERIFICATION UNKNOWN\n"),
        )
        self.assertEqual(unknown[0].status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(unknown[0].status))
        err, _ = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="solver exploded\n", rc=2),
        )
        self.assertEqual(err[0].status, laws.ERROR)
        self.assertIn("solver exploded", err[0].message.lower())
        self.assertNotEqual(err[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(err[0].status))
        empty, _ = self._esbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="", stderr="", rc=2),
        )
        self.assertEqual(empty[0].status, laws.ERROR)
        self.assertIn("not a proof", empty[0].message.lower())

    def test_cpp_run_esbmc_never_clean_missing_is_notrun(self):
        text = Path(__file__).resolve().parents[1].joinpath(
            "src", "prism", "adapters.cpp"
        ).read_text(encoding="utf-8")
        start = text.find("std::vector<Finding> run_esbmc(")
        if start < 0:
            self.skipTest("adapters.cpp has no run_esbmc")
        stub = text[start:text.find("std::vector<Finding> run_dafny(", start)]
        self.assertIn('notrun("esbmc"', stub)
        self.assertIn("no .c files in scope", stub)
        self.assertIn("laws::UNKNOWN", stub)
        self.assertIn("laws::PROVED", stub)
        self.assertIn("VERIFICATION SUCCESSFUL", stub)
        self.assertNotIn("laws::CLEAN", stub)


if __name__ == "__main__":
    unittest.main()
