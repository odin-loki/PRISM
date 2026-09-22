"""Stage JSONL journal. python -m unittest tests.test_journal -v"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import journal
from prism.models import FunctionInfo, StageResult


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

    def test_completed_ok_last_wins_and_ignores_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            journal.append_stage(out, StageResult(name="bmc", status="ok", records=2))
            journal.append_stage(out, StageResult(
                name="llm", status="skipped", detail="excluded by --stage/--skip",
            ))
            journal.append_stage(out, StageResult(name="bmc", status="ok", records=9))
            journal.append_stage(out, StageResult(name="fuzz", status="failed", records=0))
            journal.append_stage(out, StageResult(name="ltl", status="NOTRUN", records=1))
            recs = journal.completed_ok(out)
            self.assertEqual(set(recs), {"bmc", "ltl"})
            self.assertEqual(recs["bmc"].records, 9)
            self.assertEqual(recs["ltl"].status, "NOTRUN")
            self.assertNotIn("llm", recs)
            self.assertNotIn("fuzz", recs)

    def test_functions_roundtrip_and_reset(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            fn = FunctionInfo(
                file="a.c", name="add", kind="SCALAR", line=1,
                signature="int add(int x)", params=[("int", "x")],
                body="return x;",
            )
            journal.write_functions(out, [fn])
            loaded = journal.read_functions(out)
            self.assertEqual(len(loaded), 1)
            self.assertEqual(loaded[0].name, "add")
            self.assertEqual(loaded[0].body, "return x;")
            self.assertEqual(loaded[0].params, [("int", "x")])
            journal.reset(out)
            self.assertEqual(journal.read_functions(out), [])
            self.assertEqual(journal.read_stages(out), [])

    def test_resume_from_jsonl_without_report_json(self):
        """A killed run has stages.jsonl but no report.json. Resume must not
        skip classify into an empty function list (quiet skip)."""
        from prism.config import Config
        from prism.pipeline import run_pipeline

        with tempfile.TemporaryDirectory(prefix="prism_jsonl_resume_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            first = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                stages=["inventory", "classify"], skip=[],
            ))
            names = {f.name for f in first.functions}
            self.assertIn("add", names)
            report = out / "report.json"
            self.assertTrue(report.is_file())
            report.unlink()
            self.assertTrue((out / journal.STAGES_JSONL).is_file())
            self.assertTrue((out / journal.FUNCTIONS_JSON).is_file())

            second = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["inventory", "classify", "lints"], skip=[],
            ))
            self.assertTrue(any("stages.jsonl" in n for n in second.notes))
            cls = next(s for s in second.stages if s.name == "classify")
            self.assertEqual(cls.status, "ok")
            self.assertTrue(any(f.name == "add" for f in second.functions))
            cls1 = next(s for s in first.stages if s.name == "classify")
            self.assertEqual(cls.records, cls1.records)

    def test_resume_does_not_skip_classify_without_functions(self):
        from prism.config import Config
        from prism.pipeline import run_pipeline

        with tempfile.TemporaryDirectory(prefix="prism_resume_empty_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            out.mkdir()
            journal.append_stage(out, StageResult(name="inventory", status="ok", records=1))
            journal.append_stage(out, StageResult(name="classify", status="ok", records=1))
            report = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["inventory", "classify"], skip=[],
            ))
            self.assertTrue(any(f.name == "add" for f in report.functions))
            cls = next(s for s in report.stages if s.name == "classify")
            self.assertEqual(cls.status, "ok")
            self.assertGreaterEqual(cls.records, 1)

    def test_resume_reuses_notrun_and_not_failed(self):
        from prism.config import Config
        from prism.pipeline import run_pipeline

        with tempfile.TemporaryDirectory(prefix="prism_resume_nr_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            first = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                stages=["llm"], skip=[],
            ))
            llm1 = next(s for s in first.stages if s.name == "llm")
            self.assertEqual(llm1.status, "NOTRUN")
            second = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["llm"], skip=[],
            ))
            self.assertTrue(any("resumed" in n for n in second.notes))
            llm2 = next(s for s in second.stages if s.name == "llm")
            self.assertEqual(llm2.status, "NOTRUN")
            self.assertEqual(llm2.records, llm1.records)

        with tempfile.TemporaryDirectory(prefix="prism_resume_fail_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            out.mkdir()
            journal.append_stage(out, StageResult(name="inventory", status="failed", records=0))
            journal.write_functions(out, [])
            report = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["inventory"], skip=[],
            ))
            inv = next(s for s in report.stages if s.name == "inventory")
            self.assertEqual(inv.status, "ok")
            self.assertGreaterEqual(inv.records, 1)

    def test_resume_falls_back_to_report_json_without_jsonl(self):
        from prism.config import Config
        from prism.pipeline import run_pipeline

        with tempfile.TemporaryDirectory(prefix="prism_resume_rpt_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            first = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                stages=["inventory", "classify"], skip=[],
            ))
            jsonl = out / journal.STAGES_JSONL
            self.assertTrue(jsonl.is_file())
            jsonl.unlink()
            self.assertTrue((out / "report.json").is_file())
            second = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["inventory", "classify"], skip=[],
            ))
            self.assertTrue(any("report.json" in n for n in second.notes))
            names = {f.name for f in second.functions}
            self.assertIn("add", names)
            cls = next(s for s in second.stages if s.name == "classify")
            self.assertEqual(cls.status, "ok")
            cls1 = next(s for s in first.stages if s.name == "classify")
            self.assertEqual(cls.records, cls1.records)

    def test_stages_present_distinguishes_missing_from_failed_log(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            self.assertFalse(journal.stages_present(out))
            journal.reset(out)
            self.assertFalse(journal.stages_present(out))
            journal.append_stage(out, StageResult(name="inventory", status="failed"))
            self.assertTrue(journal.stages_present(out))
            self.assertEqual(journal.completed_ok(out), {})

    def test_resume_failed_jsonl_does_not_revive_stale_report_json(self):
        """A new run's failed stages.jsonl is the truth. Stale report.json
        ok/NOTRUN rows must not be resumed just because completed_ok is empty."""
        from prism.config import Config
        from prism.models import FunctionInfo, RunReport
        from prism.pipeline import run_pipeline

        with tempfile.TemporaryDirectory(prefix="prism_resume_stale_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            out.mkdir()
            stale = RunReport(root=str(root))
            stale.functions = [FunctionInfo(
                file="stale.c", name="stale_fn", kind="SCALAR", line=1,
                signature="int stale_fn(void)",
            )]
            stale.stages.append(StageResult(name="inventory", status="ok", records=99))
            stale.stages.append(StageResult(name="classify", status="ok", records=99))
            stale.save(out / "report.json")
            journal.append_stage(out, StageResult(name="inventory", status="failed", records=0))
            self.assertTrue(journal.stages_present(out))
            self.assertEqual(journal.completed_ok(out), {})

            report = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                resume=True, stages=["inventory", "classify"], skip=[],
            ))
            self.assertFalse(any("report.json" in n for n in report.notes))
            inv = next(s for s in report.stages if s.name == "inventory")
            self.assertEqual(inv.status, "ok")
            self.assertNotEqual(inv.records, 99)
            self.assertTrue(any(f.name == "add" for f in report.functions))
            self.assertFalse(any(f.name == "stale_fn" for f in report.functions))


if __name__ == "__main__":
    unittest.main()
