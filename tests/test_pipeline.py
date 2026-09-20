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
            for name in ("harness", "rapid", "muttest", "diff", "fuzz", "bmc", "execute",
                         "concolic", "sanitize", "taint", "thread", "interval", "wp"):
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
        # testdata/ is 1000+ TUs. A full pipeline there is `python -m helix testdata`,
        # not a unit test. Copy the planted oracles only.
        planted = (
            "add_overflow.c", "div_param.c", "oob_write.c",
            "shift_ub.c", "null_branch.c",
        )
        slow = [
            "sanitize", "cppcheck", "esbmc", "dafny", "pbsd", "lints",
            "warnings", "concolic", "rapid", "muttest", "diff", "ltl",
            "execute", "harness", "wp", "contracts", "taint", "thread",
            "interval",
        ]
        with tempfile.TemporaryDirectory(prefix="helix_pipe_") as td:
            root = Path(td) / "src"
            root.mkdir()
            for name in planted:
                (root / name).write_bytes((TD / name).read_bytes())
            out = Path(td) / "out"
            report = run_pipeline(_cfg(
                out, root=root, timeout=5.0, fuzz_budget=0.5, fuzz_iters=32,
                skip=["llm", "repair", *slow],
            ))
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

    def test_repair_no_llm_is_notrun_never_clean(self):
        with tempfile.TemporaryDirectory(prefix="helix_repair_") as td:
            out = Path(td)
            report = run_pipeline(_cfg(out, llm=False, stages=["repair"], skip=[]))
            rec = next(s for s in report.stages if s.name == "repair")
            self.assertEqual(rec.status, "NOTRUN")
            self.assertTrue(rec.findings)
            self.assertTrue(all(f.status == laws.NOTRUN for f in rec.findings))
            self.assertFalse(any(laws.is_proof(f.status) for f in rec.findings))
            self.assertNotEqual(rec.findings[0].status, laws.CLEAN)

    def test_execute_without_cex_is_notrun(self):
        with tempfile.TemporaryDirectory(prefix="helix_exec_") as td:
            out = Path(td)
            report = run_pipeline(_cfg(out, llm=False, stages=["execute"], skip=[]))
            rec = next(s for s in report.stages if s.name == "execute")
            self.assertEqual(rec.status, "NOTRUN")
            self.assertTrue(all(f.status == laws.NOTRUN for f in rec.findings))
            self.assertFalse(any(f.status == laws.CLEAN for f in rec.findings))
            self.assertFalse(any(laws.is_proof(f.status) for f in rec.findings))

    def test_resume_reuses_ok_stages(self):
        from unittest.mock import patch

        from helix.models import Finding

        # Focused resume check: tiny tree + mocked BMC. Do not run testdata.
        bmc_hit = [Finding(
            stage="bmc", status=laws.FAILED, file="add.c", function="add",
            line=1, cls="INT-SIGNED-OVF", message="overflow",
            strength=laws.STRENGTH_PROVES,
        )]
        with tempfile.TemporaryDirectory(prefix="helix_resume_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text(
                "int add(int x) { return x + 1; }\n", encoding="utf-8",
            )
            out = Path(td) / "out"
            base = dict(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                stages=["inventory", "classify", "bmc"], skip=[],
            )
            with patch("helix.pipeline.run_bmc", return_value=bmc_hit):
                first = run_pipeline(Config(**base))
            bmc = next(s for s in first.stages if s.name == "bmc")
            self.assertEqual(bmc.status, "ok")
            with patch("helix.pipeline.run_bmc") as mocked:
                second = run_pipeline(Config(**base, resume=True))
            mocked.assert_not_called()
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

    def test_write_md_keeps_unknown_and_timeout(self):
        from helix.models import Finding, StageResult
        from helix.pipeline import _write_md

        rec = RunReport(root="x")
        rec.stages.append(StageResult(
            name="esbmc", status="ok", records=1,
            findings=[Finding(
                stage="esbmc", status=laws.UNKNOWN, file="", function=None,
                line=None, cls="", message="esbmc present; no .c files in scope",
                strength=laws.STRENGTH_PROVES,
            )],
        ))
        rec.stages.append(StageResult(
            name="cppcheck", status="ok", records=1,
            findings=[Finding(
                stage="cppcheck", status=laws.TIMEOUT, file="a.c", function=None,
                line=None, cls="", message="cppcheck timeout",
                strength=laws.STRENGTH_FINDS,
            )],
        ))
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "report.md"
            _write_md(rec, p)
            text = p.read_text(encoding="utf-8")
        self.assertIn("`UNKNOWN`", text)
        self.assertIn("`TIMEOUT`", text)
        self.assertIn("no .c files", text)
        self.assertIn("cppcheck timeout", text)
        self.assertNotIn("`PROVED`", text)

    def test_write_md_empty_scope_confidence_is_zero_not_na(self):
        from helix.confidence import apply
        from helix.pipeline import _write_md

        rec = RunReport(root="empty")
        rec.visibility = rec.answer = rec.resolution = rec.confidence = 1.0
        apply(rec)
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "report.md"
            _write_md(rec, p)
            text = p.read_text(encoding="utf-8")
        self.assertEqual(rec.confidence, 0.0)
        self.assertIn("**0.0**", text)
        self.assertNotIn("n/a", text.lower())
        self.assertNotIn("None", text)

    def test_llm_stage_forces_reads_so_it_cannot_cover(self):
        from unittest.mock import patch

        from helix.models import Finding
        from helix.pipeline import Pipeline, llm_forced_reads
        from helix.taxonomy import coverage_from_report

        lie = Finding(
            stage="bmc", status=laws.PROVED, file="a.c", function="add",
            line=1, cls="INT-SIGNED-OVF", message="spoofed proof",
            strength=laws.STRENGTH_PROVES,
        )
        forced = llm_forced_reads([
            Finding(
                stage="bmc", status=laws.PROVED, file="a.c", function="add",
                line=1, cls="INT-SIGNED-OVF", message="spoofed proof",
                strength=laws.STRENGTH_PROVES,
            ),
        ])
        self.assertEqual(forced[0].stage, "llm")
        self.assertEqual(forced[0].strength, laws.STRENGTH_READS)
        self.assertEqual(forced[0].status, laws.HYPOTHESIS)
        self.assertFalse(laws.is_proof(forced[0].status))

        with tempfile.TemporaryDirectory(prefix="helix_llm_reads_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text("int add(int x) { return x; }\n", encoding="utf-8")
            out = Path(td) / "out"
            with patch.object(Pipeline, "_engine", return_value=object()), patch(
                "helix.pipeline.hypothesize", return_value=[lie],
            ):
                report = run_pipeline(Config(
                    root=root, out=out, llm=True, unwind=2,
                    fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                    stages=["llm"], skip=[],
                ))
        rec = next(s for s in report.stages if s.name == "llm")
        self.assertTrue(rec.findings)
        f = rec.findings[0]
        self.assertEqual(f.stage, "llm")
        self.assertEqual(f.strength, laws.STRENGTH_READS)
        self.assertFalse(laws.is_proof(f.status))
        self.assertNotEqual(f.status, laws.CLEAN)
        rows = {r["id"]: r for r in coverage_from_report(report)}
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")
        self.assertEqual(rows["INT-SIGNED-OVF"]["best"], laws.STRENGTH_READS)

    def test_missing_esbmc_pipeline_is_never_a_proof(self):
        from unittest.mock import patch

        from helix.models import Finding

        missing = Finding(
            stage="esbmc", status=laws.NOTRUN, file="", function=None,
            line=None, cls="", message="esbmc not found (config, vendored tree, PATH)",
            strength=laws.STRENGTH_FINDS, extra={"install": "build from vendored"},
        )
        with tempfile.TemporaryDirectory(prefix="helix_esbmc_nr_") as td:
            root = Path(td) / "src"
            root.mkdir()
            (root / "add.c").write_text("int add(int x) { return x; }\n", encoding="utf-8")
            out = Path(td) / "out"
            with patch("helix.pipeline.run_esbmc", return_value=[missing]):
                report = run_pipeline(Config(
                    root=root, out=out, llm=False, unwind=2,
                    fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                    stages=["esbmc"], skip=[],
                ))
        rec = next(s for s in report.stages if s.name == "esbmc")
        self.assertEqual(rec.status, "NOTRUN")
        self.assertTrue(rec.findings)
        self.assertTrue(all(f.status == laws.NOTRUN for f in rec.findings))
        self.assertFalse(any(laws.is_proof(f.status) for f in rec.findings))
        self.assertNotEqual(rec.findings[0].status, laws.CLEAN)

    def test_unify_clean_is_not_a_proof_and_empty_scope_confidence_zero(self):
        with tempfile.TemporaryDirectory(prefix="helix_unify_conf_") as td:
            root = Path(td) / "empty"
            root.mkdir()
            out = Path(td) / "out"
            report = run_pipeline(Config(
                root=root, out=out, llm=False, unwind=2,
                fuzz_budget=0.1, fuzz_iters=1, repair_rounds=1,
                stages=["unify"], skip=[],
            ))
            rec = next(s for s in report.stages if s.name == "unify")
            self.assertTrue(rec.findings)
            f = rec.findings[0]
            self.assertEqual(f.status, laws.CLEAN)
            self.assertFalse(laws.is_proof(f.status))
            self.assertIn("not a proof", f.message)
            self.assertEqual((f.extra or {}).get("not_a_proof"), "true")
            self.assertEqual(report.confidence, 0.0)
            self.assertEqual(report.visibility, 0.0)
            md = (out / "report.md").read_text(encoding="utf-8")
            self.assertIn("**0.0**", md)
            self.assertNotIn("n/a", md.lower())


if __name__ == "__main__":
    unittest.main()
