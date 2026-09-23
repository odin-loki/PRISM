"""The Clang-AST lint layer's discarded-return table (roadmap 2.8) is generated
from the regex lints' lint_discarded* rules; it must not drift from them."""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TestAstlintDiscardTable(unittest.TestCase):
    def test_generated_table_is_current(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_astlint_discard.py"), "--check"],
                           cwd=ROOT, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_every_table_class_is_in_the_taxonomy(self):
        from prism.taxonomy import CLASSES
        ids = {c["id"] for c in CLASSES}
        inc = (ROOT / "src" / "prism" / "astlint_discard.inc").read_text(encoding="utf-8")
        for cls in set(re.findall(r'\{"([A-Z][A-Z0-9-]+)", (?:true|false)\}', inc)):
            self.assertIn(cls, ids)


if __name__ == "__main__":
    unittest.main()
