"""CBMC adapter check-polarity: never pass a flag that silently disables a check.

Python engine `_run_cbmc` is law. Missing cbmc is NOTRUN (the binary did not run).
A present CBMC that prints VERIFICATION SUCCESSFUL is recorded as the
adapter maps it — currently BOUNDED, unwind-limited, not a proof — and
must never merge with in-tree PROVED-UNBOUNDED.

Mapping in prism/adapters_extra.py `_run_cbmc` (test the code, not a wish):
    VERIFICATION SUCCESSFUL → laws.BOUNDED
        message: "CBMC: BOUNDED (unwind limited; not a proof)"
    VERIFICATION FAILED     → laws.FAILED
    VERIFICATION UNKNOWN    → laws.UNKNOWN
    else                    → laws.ERROR

If SUCCESSFUL mapped to PROVED that would be allowed for this adapter
because CBMC actually ran. Missing cbmc must not. BOUNDED is what the
source currently emits; PROVED-UNBOUNDED is BMC k-induction, not CBMC.

python -m unittest tests.test_cbmc
"""

from __future__ import annotations

import inspect
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters_extra import _run_cbmc, run_optional_tools
from prism.config import Config, adapter_install
from prism.laws import refuse_merge

EXE = r"C:\tools\cbmc.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}
_DISABLE = ("--no-bounds-check", "--no-div-by-zero-check")


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestCbmcAdapter(unittest.TestCase):
    def _never_proof(self, findings):
        self.assertTrue(findings)
        for f in findings:
            self.assertEqual(f.stage, "cbmc", f.stage)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _cbmc(self, paths, run_side_effect, exe=EXE):
        with mock.patch("prism.adapters_extra._run", side_effect=run_side_effect) as run:
            out = _run_cbmc(exe, paths, Config())
        return out, run

    def test_missing_cbmc_is_notrun_via_run_optional_tools(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None), \
             mock.patch("prism.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        cbmc = next(f for f in findings if f.stage == "cbmc")
        self.assertEqual(cbmc.status, laws.NOTRUN)
        self.assertIn("not found", cbmc.message)
        self.assertNotEqual(cbmc.status, laws.BOUNDED)
        self.assertNotEqual(cbmc.status, laws.PROVED)
        self.assertNotEqual(cbmc.status, laws.PROVED_UNBOUNDED)
        self.assertFalse(laws.is_proof(cbmc.status))
        self.assertFalse(any(f.stage == "bmc" for f in findings))

    def test_present_no_c_is_unknown_strength_proves_never_proved(self):
        with mock.patch("prism.adapters_extra._run") as run:
            out = _run_cbmc(EXE, [Path("unit.cpp"), Path("hdr.h")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertIn("no .c files", f.message)
        self.assertIn(EXE, f.message)
        self._never_proof(out)

        empty, run2 = self._cbmc([], lambda *_a, **_k: _proc())
        run2.assert_not_called()
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self.assertEqual(empty[0].strength, laws.STRENGTH_PROVES)
        self._never_proof(empty)

    def test_verification_successful_maps_to_bounded_never_proved_unbounded(self):
        # Python engine records the CBMC binary's claim through THIS mapping:
        # SUCCESSFUL → BOUNDED (unwind limited; not a proof). Allowed because
        # CBMC ran. Must not be treated as in-tree BMC PROVED-UNBOUNDED.
        out, run = self._cbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="VERIFICATION SUCCESSFUL\n"),
        )
        run.assert_called_once()
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], EXE)
        self.assertIn(str(C_FILE), cmd)
        self.assertIn("--unwind", cmd)
        self.assertIn("--timeout", cmd)
        for flag in cmd:
            self.assertFalse(
                flag.startswith("--no-") and flag.endswith("-check"),
                msg=f"cbmc must not pass check-disabling flags: {flag}",
            )

        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "cbmc")
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertIn("BOUNDED", f.message)
        self.assertIn("not a proof", f.message.lower())
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertFalse(laws.is_proof(f.status))
        with self.assertRaises(ValueError) as ctx:
            refuse_merge(f.status, laws.PROVED_UNBOUNDED)
        self.assertIn("refusing to merge", str(ctx.exception))
        self._never_proof(out)

    def test_refuses_no_bounds_check_and_no_div_by_zero_check(self):
        # Weak: the refusal is still inline in `_run_cbmc` (the Python engine is law).
        src = inspect.getsource(_run_cbmc)
        self.assertIn('startswith("--no-")', src)
        self.assertIn('endswith("-check")', src)
        self.assertIn("refusing to disable a check", src)

        # Execute that loop: cmd is `[exe, path, "--unwind", str(cfg.unwind),
        # "--timeout", ...]`. A disabling flag in unwind becomes a cmd token.
        for flag in _DISABLE:
            with self.subTest(flag=flag):
                cfg = mock.Mock(unwind=flag, timeout=30.0)
                with mock.patch("prism.adapters_extra._run") as run:
                    with self.assertRaises(ValueError) as ctx:
                        _run_cbmc(EXE, [C_FILE], cfg)
                run.assert_not_called()
                self.assertIn("refusing to disable a check", str(ctx.exception))
                self.assertIn(flag, str(ctx.exception))

    def test_timeout_is_timeout_never_silent_skip(self):
        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        out, run = self._cbmc([C_FILE], boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertEqual(f.stage, "cbmc")
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertIn("timeout", f.message.lower())
        self.assertNotEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self._never_proof(out)

    def test_cpp_verification_successful_is_bounded_never_proved(self):
        text = Path(__file__).resolve().parents[1].joinpath(
            "src", "prism", "adapters.cpp"
        ).read_text(encoding="utf-8")
        start = text.find("std::vector<Finding> run_cbmc(")
        if start < 0:
            self.skipTest("adapters.cpp has no run_cbmc")
        stub = text[start:text.find("Finding libfuzzer_probe(", start)]
        self.assertIn("VERIFICATION SUCCESSFUL", stub)
        self.assertIn("laws::BOUNDED", stub)
        self.assertIn("not a proof", stub)
        self.assertIn("no .c files in scope", stub)
        self.assertIn("laws::UNKNOWN", stub)
        self.assertNotIn("laws::CLEAN", stub)
        successful = stub[stub.find("VERIFICATION SUCCESSFUL"):stub.find("VERIFICATION FAILED")]
        self.assertIn("laws::BOUNDED", successful)
        self.assertNotIn("laws::PROVED", successful)

    def test_catch2_or_unknown_option_is_notrun_never_bounded(self):
        out, run = self._cbmc(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="Catch2 v3.5.0\nUnknown option: --timeout\n"),
        )
        run.assert_called_once()
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not CBMC", out[0].message)
        self.assertNotEqual(out[0].status, laws.BOUNDED)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertEqual((out[0].extra or {}).get("install"), adapter_install("cbmc"))
        self._never_proof(out)


if __name__ == "__main__":
    unittest.main()
