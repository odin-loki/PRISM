"""Self-scan SARIF triage helper (docs/VERIFICATION_PLAN.md Step 1)."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "data" / "selfscan_min"


class TestTriageSelfscan(unittest.TestCase):
    def test_fixture_buckets(self):
        out = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "triage_selfscan.py"), str(FIXTURE)],
            capture_output=True,
            text=True,
            check=True,
            cwd=ROOT,
        )
        text = out.stdout
        self.assertIn("total FAILED: 4", text)
        self.assertIn("third_party (out of scope): 1", text)
        self.assertIn("testdata/tests (false alarm corpus): 1", text)
        self.assertIn("src/prism|gui (manual review): 1", text)
        self.assertIn(".github (false alarm LANG-LINT): 1", text)
        self.assertIn("summary: 1 / 3 / 1", text)


if __name__ == "__main__":
    unittest.main()
