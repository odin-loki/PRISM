"""Regressions for docs/FUZZ_SELF.md F2-F4: loaders never crash on bad input.

python -m unittest tests.test_fuzz_findings
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import journal
from prism.config import load_manifest
from prism.models import RunReport


class TestLoadersNeverRaise(unittest.TestCase):
    def test_f2_report_of_wrong_shape(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "report.json"
            for text in ('{"stages": "x"}', '{"functions": [1]}', '{"stages": [1]}', '[]', '"x"'):
                p.write_text(text, encoding="utf-8")
                self.assertIsNone(RunReport.load(p), text)

    def test_f3_journal_lines_of_wrong_shape(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            (out / journal.STAGES_JSONL).write_text('""\n[1]\n5\n{"name": "lints", "status": "ok"}\n',
                                                     encoding="utf-8")
            recs = journal.read_stages(out)
            self.assertEqual([r.name for r in recs], ["lints"])
            self.assertIn("lints", journal.completed_ok(out))

    def test_f4_manifest_not_utf8(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "third_party").mkdir()
            (root / "third_party" / "MANIFEST.toml").write_bytes(b'[[component]]\nname = "\xf3"\n')
            self.assertEqual(load_manifest(root), {})
            (root / "third_party" / "MANIFEST.toml").write_bytes(b'component = "x"\n')
            self.assertEqual(load_manifest(root), {})


if __name__ == "__main__":
    unittest.main()
