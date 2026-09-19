"""End-to-end Helix on testdata. python -m unittest tests.test_pipeline -v"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from helix import laws
from helix.config import Config
from helix.models import RunReport
from helix.pipeline import STAGE_ORDER, run_pipeline

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def _cfg(out: Path, **kw) -> Config:
    opts = dict(
        root=TD,
        out=out,
        llm=False,
        unwind=8,
        fuzz_budget=1.0,
        fuzz_iters=64,
        repair_rounds=1,
        skip=["llm", "repair"],
    )
    opts.update(kw)
    return Config(**opts)


class TestPipeline(unittest.TestCase):
    def test_stage_order_has_new_stages(self):
            for name in ("harness", "rapid", "muttest", "fuzz", "bmc", "execute",
                         "concolic", "sanitize", "taint", "thread", "interval"):
                self.assertIn(name, STAGE_ORDER)

    def test_inventory_empty_tu(self):
        with tempfile.TemporaryDirectory(prefix="helix_inv_") as td:
            out = Path(td)
            report = run_pipeline(_cfg(out, stages=["inventory", "classify"]))
            inv = next(s for s in report.stages if s.name == "inventory")
            self.assertEqual(inv.status, "ok")
            empty = [f for f in inv.findings if f.file == "empty_tu.c"]
            self.assertEqual(len(empty), 1)
            self.assertEqual(empty[0].status, laws.ERROR)
            self.assertEqual(empty[0].cls, "EMPTY-TU")
            self.assertIn("not a clean unit", empty[0].message)
            add = [f for f in inv.findings if f.file == "add_overflow.c"]
            self.assertEqual(len(add), 1)
            self.assertEqual(add[0].status, laws.CLEAN)

    def test_testdata_crashes_planted_bugs(self):
        with tempfile.TemporaryDirectory(prefix="helix_pipe_") as td:
            out = Path(td)
            report = run_pipeline(_cfg(out))
            report.save(out / "report.json")
            crashes = {
                f.function
                for s in report.stages
                for f in s.findings
                if f.status == laws.CRASH and f.function
            }
            for name in ("add_overflow", "div_param", "oob_write", "shift_ub"):
                self.assertIn(name, crashes, f"expected CRASH for {name}, got {sorted(crashes)}")
            proved_ptr = [
                f for s in report.stages for f in s.findings
                if f.status in {laws.PROVED, laws.PROVED_UNBOUNDED}
                and f.function == "null_branch"
            ]
            self.assertEqual(proved_ptr, [])
            skipped = [s.name for s in report.stages if s.status == "skipped"]
            self.assertIn("llm", skipped)
            self.assertIn("repair", skipped)
            opt = next(s for s in report.stages if s.name == "optional")
            self.assertEqual(opt.status, "NOTRUN")
            self.assertIn("klee", opt.detail.lower())
            self.assertGreaterEqual(opt.records, 8)
            self.assertIn("spatch", opt.detail.lower())
            self.assertFalse(any(laws.is_proof(f.status) for s in report.stages
                                 if s.name == "fuzz" for f in s.findings))

    def test_resume_reuses_ok_stages(self):
        with tempfile.TemporaryDirectory(prefix="helix_resume_") as td:
            out = Path(td)
            first = run_pipeline(_cfg(out, skip=["llm", "repair", "fuzz", "rapid", "muttest", "execute"]))
            bmc = next(s for s in first.stages if s.name == "bmc")
            self.assertEqual(bmc.status, "ok")
            second = run_pipeline(_cfg(
                out, resume=True,
                skip=["llm", "repair", "fuzz", "rapid", "muttest", "execute"],
            ))
            bmc2 = next(s for s in second.stages if s.name == "bmc")
            self.assertEqual(bmc2.status, "ok")
            self.assertEqual(bmc2.records, bmc.records)
            self.assertTrue(any("resumed" in n for n in second.notes))

    def test_report_roundtrip(self):
        rec = RunReport(root="x")
        rec.notes.append("hi")
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "report.json"
            rec.save(p)
            loaded = RunReport.load(p)
            self.assertIsNotNone(loaded)
            self.assertEqual(loaded.root, "x")
            self.assertEqual(loaded.notes, ["hi"])
            self.assertIsNone(RunReport.load(Path(td) / "missing.json"))
            p.write_text("{not json", encoding="utf-8")
            self.assertIsNone(RunReport.load(p))


if __name__ == "__main__":
    unittest.main()
