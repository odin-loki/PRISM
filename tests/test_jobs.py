"""--jobs reaches Config. python -m unittest tests.test_jobs"""

from __future__ import annotations

import unittest
from unittest.mock import patch

from prism.__main__ import main
from prism.checkers import run_lints
from prism.config import Config


class TestJobs(unittest.TestCase):
    def test_cli_jobs_flag(self):
        captured: list[Config] = []

        def fake_run(cfg: Config):
            captured.append(cfg)

            class R:
                confidence = 0.0
                visibility = 0.0
                answer = 0.0
                resolution = 0.0
                stages = []

            return R()

        with patch("prism.__main__.run_pipeline", side_effect=fake_run):
            rc = main(["testdata/abs_ok.c", "--no-llm", "--jobs", "3",
                       "--skip", "fuzz,llm,repair,optional,bmc"])
        self.assertEqual(rc, 0)
        self.assertTrue(captured)
        self.assertEqual(captured[0].jobs, 3)

    def test_run_lints_jobs_one_matches_serial(self):
        from pathlib import Path
        root = Path(__file__).resolve().parents[1] / "testdata"
        paths = [root / "abs_ok.c"]
        a = run_lints(paths, root, jobs=1)
        b = run_lints(paths, root, jobs=2)
        self.assertEqual([f.cls for f in a], [f.cls for f in b])
