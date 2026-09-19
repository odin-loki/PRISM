"""Stage JSONL journal. python -m unittest tests.test_journal -v"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from helix import journal
from helix.models import StageResult


class TestJournal(unittest.TestCase):
    def test_append_and_read(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            journal.reset(out)
            self.assertEqual(journal.read_stages(out), [])
            journal.append_stage(out, StageResult(name="bmc", status="ok", records=3))
            journal.append_stage(out, StageResult(name="fuzz", status="ok", records=4))
            recs = journal.read_stages(out)
            self.assertEqual([r.name for r in recs], ["bmc", "fuzz"])
            self.assertEqual(recs[0].records, 3)
            journal.reset(out)
            self.assertEqual(journal.read_stages(out), [])

    def test_corrupt_line_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            p = out / journal.STAGES_JSONL
            p.write_text("{not json\n{\"name\":\"lints\",\"status\":\"ok\",\"records\":1}\n",
                         encoding="utf-8")
            recs = journal.read_stages(out)
            self.assertEqual(len(recs), 1)
            self.assertEqual(recs[0].name, "lints")


if __name__ == "__main__":
    unittest.main()
