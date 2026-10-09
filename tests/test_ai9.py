"""Roadmap 9.2 / 9.3 / 4.2 review + Lean prove: C++ in tests/cpp/test_ai9.cpp.

Only checks that need the Python engine reference implementation."""

from __future__ import annotations

import unittest
from pathlib import Path

from prism import laws
from prism.pipeline import STAGE_ORDER, run_review_notrun
from prism.taxonomy import CLASSES

ROOT = Path(__file__).resolve().parents[1]


class TestPythonEngineOnly(unittest.TestCase):
    def test_review_stage_order_and_python_notrun(self):
        hpp = (ROOT / "include" / "prism" / "pipeline.hpp").read_text(encoding="utf-8")
        self.assertIn('"harness", "review", "concolic"', hpp)
        i = STAGE_ORDER.index("review")
        self.assertEqual(STAGE_ORDER[i - 1], "harness")
        self.assertEqual(STAGE_ORDER[i + 1], "concolic")
        rows = run_review_notrun()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertIn("C++ engine only", rows[0].message)

    def test_taxonomy_review_classes_match_cpp(self):
        ids = {c["id"] for c in CLASSES}
        cpp = (ROOT / "src" / "prism" / "taxonomy.cpp").read_text(encoding="utf-8")
        for cls in ("VACUOUS-ASSUMPTION", "PROOF-REGRESSION"):
            self.assertIn(cls, ids)
            self.assertIn(f'{{"{cls}"', cpp)


if __name__ == "__main__":
    unittest.main()
