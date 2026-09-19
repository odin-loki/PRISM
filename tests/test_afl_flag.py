"""AFL availability flag and HELIX_AFL opt-in. python -m unittest tests.test_afl_flag"""

from __future__ import annotations

import os
import unittest
from pathlib import Path
from unittest.mock import patch

from helix import laws
from helix.cparse import extract_functions
from helix.fuse import run_fuse
from helix.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestAflFlag(unittest.TestCase):
    def test_afl_available_fuzz_still_called(self):
        f, p = fn("saturate")
        called = []

        def fake_fuzz(fn_, src, budget=0, iters=0, seeds=None):
            called.append(fn_.name)
            return Finding(
                stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
                line=f.line, cls="", message="no crash (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"new_cov": 0, "iters": 1, "corpus": 1},
            )

        env = {k: v for k, v in os.environ.items() if k != "HELIX_AFL"}
        with patch.dict(os.environ, env, clear=True):
            with patch("helix.fuse.afl_available", return_value="/fake/afl-fuzz.exe"):
                with patch("helix.fuse.fuzz_function", side_effect=fake_fuzz):
                    recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        self.assertTrue(called)
        self.assertTrue(all(c == "saturate" for c in called))
        self.assertTrue(recs[0].extra.get("afl_available"))
        self.assertNotIn("engine", recs[0].extra)

    def test_helix_afl_mocks_subprocess(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        afl_clean = Finding(
            stage="fuse", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no AFL crash in 2s (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"engine": "afl"},
        )

        with patch.dict(os.environ, {"HELIX_AFL": "1"}, clear=False):
            with patch("helix.fuse.afl_available", return_value="/fake/afl-fuzz.exe"):
                with patch("helix.fuse.fuzz_function", return_value=clean):
                    with patch("helix.fuse.run_afl_fuzz", return_value=afl_clean) as mock_afl:
                        recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        mock_afl.assert_called_once()
        self.assertEqual(recs[0].extra.get("engine"), "afl")
        self.assertNotIn("afl_available", recs[0].extra)


if __name__ == "__main__":
    unittest.main()
