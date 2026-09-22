"""GUI helpers and optional QWidget smoke test. python -m unittest tests.test_gui -q

Headless: finding-row, confidence-label, skip-list, and C++ source-contract
tests never open a window. Widget construction is skipped unless PySide6 is
installed (QT_QPA_PLATFORM=offscreen). Missing PySide is NOTRUN, never CLEAN.
The offscreen window, when present, must show the same statuses as report.json:
PROVED-UNBOUNDED stays a proof, CLEAN fuzz is not a proof, missing ESBMC is
NOTRUN and must not display as proved.
"""

from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.gui import (
    FINDING_COLUMNS,
    HAS_PYSIDE6,
    MISSING_REPORT_LABEL,
    _PROOF_GREEN,
    _STATUS_BG,
    confidence_label,
    confidence_product,
    finding_rows,
    gui_import_status,
    launch,
    missing_display_finding,
    missing_pyside_finding,
    refuse_llm_cover,
    skip_from_checks,
    status_background,
    taxonomy_background,
    taxonomy_rows,
)
from prism.models import Finding, RunReport, StageResult

ROOT = Path(__file__).resolve().parents[1]
CPP_MAIN = ROOT / "src" / "gui" / "MainWindow.cpp"
CPP_HDR = ROOT / "src" / "gui" / "MainWindow.h"


class TestSkipFromChecks(unittest.TestCase):
    def test_none_checked(self):
        self.assertEqual(skip_from_checks(False, False, False), [])

    def test_all_checked(self):
        self.assertEqual(
            skip_from_checks(True, True, True),
            ["fuzz", "repair", "optional"],
        )

    def test_individual(self):
        self.assertEqual(skip_from_checks(True, False, False), ["fuzz"])
        self.assertEqual(skip_from_checks(False, True, False), ["repair"])
        self.assertEqual(skip_from_checks(False, False, True), ["optional"])

    def test_config_skip_matches_checks(self):
        from prism.config import Config

        skip = skip_from_checks(True, False, True)
        cfg = Config(skip=skip, llm=False)
        self.assertFalse(cfg.want("fuzz"))
        self.assertTrue(cfg.want("bmc"))
        self.assertTrue(cfg.want("repair"))
        self.assertFalse(cfg.want("optional"))
        self.assertFalse(cfg.llm)

    def test_helpers_import_without_loading_qt(self):
        """`from prism.gui import skip_from_checks` must not import PySide6 widgets."""
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        code = (
            "import sys\n"
            "from prism.gui import skip_from_checks, finding_rows, confidence_label, taxonomy_rows\n"
            "from prism.models import RunReport\n"
            "assert skip_from_checks(True, False, False) == ['fuzz']\n"
            "assert finding_rows(RunReport(root='mem')) == []\n"
            "assert taxonomy_rows(RunReport(root='mem'))\n"
            "assert 'confidence' in confidence_label(RunReport(root='mem'))\n"
            "qt = [m for m in sys.modules if m.startswith('PySide6.Qt')]\n"
            "assert not qt, qt\n"
        )
        r = subprocess.run(
            [sys.executable, "-c", code],
            cwd=root,
            capture_output=True,
            text=True,
            env={**os.environ, "PYTHONPATH": os.pathsep.join(
                [root, os.environ.get("PYTHONPATH", "")]
            )},
        )
        self.assertEqual(r.returncode, 0, r.stderr or r.stdout)

    def test_named_fuzz_repair_optional(self):
        """skip_from_checks(fuzz, repair, optional) — order is the skip list."""
        self.assertEqual(
            skip_from_checks(fuzz=True, repair=False, optional=True),
            ["fuzz", "optional"],
        )
        self.assertEqual(
            skip_from_checks(fuzz=False, repair=True, optional=False),
            ["repair"],
        )


class TestConfidenceLabel(unittest.TestCase):
    def test_matches_cli_product_line(self):
        report = RunReport(root="mem", visibility=1.0, answer=0.5,
                           resolution=0.5, confidence=0.25)
        self.assertEqual(
            confidence_label(report),
            "confidence 0.25  (vis 1.0 x ans 0.5 x res 0.5)",
        )

    def test_format_is_vis_ans_res_product(self):
        report = RunReport(root="mem", visibility=0.8, answer=0.4,
                           resolution=0.25, confidence=0.08)
        self.assertEqual(
            confidence_label(report),
            f"confidence {report.confidence}  "
            f"(vis {report.visibility} x ans {report.answer} x res {report.resolution})",
        )
        prod = confidence_product(report)
        self.assertEqual(prod["visibility"], 0.8)
        self.assertEqual(prod["answer"], 0.4)
        self.assertEqual(prod["resolution"], 0.25)
        self.assertEqual(prod["confidence"], 0.08)

    def test_zero_is_no_data_not_clean(self):
        report = RunReport(root="mem")
        label = confidence_label(report)
        self.assertEqual(label, "confidence 0.0  (vis 0.0 x ans 0.0 x res 0.0)")
        self.assertNotIn(laws.CLEAN, label)
        self.assertNotIn("n/a", label.lower())
        prod = confidence_product(report)
        self.assertEqual(
            (prod["visibility"], prod["answer"], prod["resolution"], prod["confidence"]),
            (0.0, 0.0, 0.0, 0.0),
        )

    def test_na_placeholder_is_zero_not_string(self):
        """Empty-scope law: n/a / None / NaN display as 0, never n/a."""
        report = RunReport(root="mem")
        report.visibility = "n/a"  # type: ignore[assignment]
        report.answer = None  # type: ignore[assignment]
        report.resolution = float("nan")
        report.confidence = "N/A"  # type: ignore[assignment]
        prod = confidence_product(report)
        self.assertEqual(
            (prod["visibility"], prod["answer"], prod["resolution"], prod["confidence"]),
            (0.0, 0.0, 0.0, 0.0),
        )
        label = confidence_label(report)
        self.assertNotIn("n/a", label.lower())
        self.assertNotIn(laws.CLEAN, label)
        self.assertIn("0.0", label)
        self.assertEqual(MISSING_REPORT_LABEL, "confidence 0  (report.json missing; not a proof)")
        self.assertNotIn("n/a", MISSING_REPORT_LABEL.lower())
        self.assertNotIn(laws.CLEAN, MISSING_REPORT_LABEL)


def _finding(stage: str, status: str, cls: str = "", message: str = "",
             file: str = "a.c", function: str | None = "add",
             line: int | None = 3) -> Finding:
    strength = laws.STRENGTH_PROVES if laws.is_proof(status) else (
        laws.STRENGTH_READS if status == laws.NOTRUN else laws.STRENGTH_FINDS
    )
    return Finding(
        stage=stage, status=status, file=file, function=function, line=line,
        cls=cls, message=message, strength=strength,
    )


def _mixed_same_report() -> RunReport:
    """PLAN window of the same report: unbounded proof, missing ESBMC, CLEAN fuzz."""
    report = RunReport(root="mem", visibility=1.0, answer=0.5,
                       resolution=0.5, confidence=0.25)
    report.stages = [
        StageResult(name="bmc", status="ok", findings=[
            _finding("bmc", laws.PROVED_UNBOUNDED, cls="INT-SIGNED-OVF",
                     message="k-induction closed; unbounded"),
        ]),
        StageResult(name="fuzz", status="ok", findings=[
            _finding("fuzz", laws.CLEAN, cls="",
                     message="no crash (not a proof)"),
        ]),
        StageResult(name="esbmc", status="NOTRUN", findings=[
            _finding("esbmc", laws.NOTRUN, cls="",
                     message="esbmc not on PATH", file="", function=None,
                     line=None),
        ]),
    ]
    return report


def _assert_mixed_honesty(test: unittest.TestCase, rows: list[dict[str, str]]) -> None:
    statuses = [r["status"] for r in rows]
    test.assertEqual(statuses, [laws.PROVED_UNBOUNDED, laws.CLEAN, laws.NOTRUN])
    test.assertEqual(len(set(statuses)), 3)
    esbmc = next(r for r in rows if r["stage"] == "esbmc")
    test.assertEqual(esbmc["status"], laws.NOTRUN)
    test.assertFalse(laws.is_proof(esbmc["status"]))
    test.assertNotEqual(esbmc["status"], laws.PROVED)
    test.assertNotEqual(esbmc["status"], laws.PROVED_UNBOUNDED)
    test.assertNotEqual(esbmc["status"], laws.CLEAN)
    fuzz = next(r for r in rows if r["stage"] == "fuzz")
    test.assertEqual(fuzz["status"], laws.CLEAN)
    test.assertFalse(laws.is_proof(fuzz["status"]))
    bmc = next(r for r in rows if r["stage"] == "bmc")
    test.assertEqual(bmc["status"], laws.PROVED_UNBOUNDED)
    test.assertTrue(laws.is_proof(bmc["status"]))


class TestFindingRows(unittest.TestCase):
    def test_proved_clean_notrun_distinguished(self):
        report = RunReport(root="mem", visibility=1.0, answer=0.5,
                           resolution=0.5, confidence=0.25)
        report.stages = [
            StageResult(name="bmc", status="ok", findings=[
                _finding("bmc", laws.PROVED, cls="INT-SIGNED-OVF",
                         message="holds under k-induction"),
            ]),
            StageResult(name="fuzz", status="ok", findings=[
                _finding("fuzz", laws.CLEAN, cls="",
                         message="no crash (not a proof)"),
            ]),
            StageResult(name="esbmc", status="NOTRUN", findings=[
                _finding("esbmc", laws.NOTRUN, cls="",
                         message="esbmc not on PATH", file="", function=None,
                         line=None),
            ]),
        ]
        rows = finding_rows(report)
        self.assertEqual(len(rows), 3)
        statuses = [r["status"] for r in rows]
        self.assertEqual(statuses, [laws.PROVED, laws.CLEAN, laws.NOTRUN])
        self.assertEqual(len(set(statuses)), 3)
        self.assertNotEqual(laws.PROVED, laws.CLEAN)
        self.assertNotEqual(laws.CLEAN, laws.NOTRUN)
        self.assertNotEqual(laws.PROVED, laws.NOTRUN)
        proved, clean, notrun = rows
        self.assertEqual(proved["stage"], "bmc")
        self.assertEqual(proved["cls"], "INT-SIGNED-OVF")
        self.assertEqual(clean["stage"], "fuzz")
        self.assertEqual(clean["cls"], "")
        self.assertEqual(notrun["stage"], "esbmc")
        self.assertEqual(notrun["file"], "")
        for key in FINDING_COLUMNS:
            self.assertIn(key, proved)
        prod = confidence_product(report)
        self.assertEqual(
            prod["confidence"],
            prod["visibility"] * prod["answer"] * prod["resolution"],
        )
        label = confidence_label(report)
        self.assertEqual(
            label,
            "confidence 0.25  (vis 1.0 x ans 0.5 x res 0.5)",
        )
        self.assertNotEqual(proved["status"], clean["status"])
        self.assertNotEqual(clean["status"], notrun["status"])
        self.assertNotEqual(proved["status"], notrun["status"])
        self.assertNotEqual(proved["message"], clean["message"])

    def test_unify_clean_is_noise_like_cli(self):
        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="unify", status="ok", findings=[
                _finding("unify", laws.CLEAN, cls="", message="taxonomy 0/0"),
            ]),
            StageResult(name="inventory", status="ok", findings=[
                _finding("inventory", laws.CLEAN, cls="", message="parsed"),
            ]),
            StageResult(name="classify", status="ok", findings=[
                _finding("classify", laws.NOTRUN, cls="", message="skip"),
            ]),
            StageResult(name="bmc", status="ok", findings=[
                _finding("bmc", laws.PROVED, cls="INT-DIV-ZERO", message="holds"),
            ]),
        ]
        rows = finding_rows(report)
        self.assertEqual([r["status"] for r in rows], [laws.PROVED])

    def test_proved_unbounded_esbmc_notrun_fuzz_clean_without_qt(self):
        """Same CLI vocabulary in table rows. Missing ESBMC is not a proof."""
        report = _mixed_same_report()
        rows = finding_rows(report)
        _assert_mixed_honesty(self, rows)
        label = confidence_label(report)
        self.assertEqual(
            label,
            "confidence 0.25  (vis 1.0 x ans 0.5 x res 0.5)",
        )
        self.assertNotIn(laws.CLEAN, label)

    def test_report_json_roundtrip_keeps_statuses(self):
        report = _mixed_same_report()
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "report.json"
            report.save(path)
            loaded = RunReport.load(path)
        self.assertIsNotNone(loaded)
        assert loaded is not None
        _assert_mixed_honesty(self, finding_rows(loaded))
        self.assertEqual(confidence_label(loaded), confidence_label(report))

    def test_hypothesis_stays_hypothesis_not_a_proof(self):
        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="llm", status="ok", findings=[
                _finding("llm", laws.HYPOTHESIS, cls="INT-SIGNED-OVF",
                         message="maybe overflow"),
            ]),
        ]
        rows = finding_rows(report)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], laws.HYPOTHESIS)
        self.assertEqual(rows[0]["stage"], "llm")
        self.assertFalse(laws.is_proof(rows[0]["status"]))
        self.assertNotEqual(rows[0]["status"], laws.CLEAN)
        self.assertNotEqual(rows[0]["status"], "COVERED")


class TestTaxonomyRows(unittest.TestCase):
    def test_covered_gap_never_clean(self):
        report = _mixed_same_report()
        rows = taxonomy_rows(report)
        self.assertTrue(rows)
        verdicts = {r["verdict"] for r in rows}
        self.assertIn("COVERED", verdicts)
        self.assertIn("GAP", verdicts)
        self.assertNotIn(laws.CLEAN, verdicts)
        self.assertNotIn(laws.PROVED, verdicts)
        self.assertNotIn(laws.PROVED_UNBOUNDED, verdicts)
        ovf = next(r for r in rows if r["id"] == "INT-SIGNED-OVF")
        self.assertEqual(ovf["verdict"], "COVERED")
        self.assertNotEqual(ovf["verdict"], laws.CLEAN)
        for r in rows:
            self.assertIn(r["verdict"], {"COVERED", "PARTIAL", "GAP"})
            self.assertNotEqual(r["verdict"], laws.CLEAN)

    def test_unify_clean_does_not_cover(self):
        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="unify", status="ok", findings=[
                _finding("unify", laws.CLEAN, cls="",
                         message="taxonomy 0/0 COVERED, 0 GAP (not a proof)"),
            ]),
        ]
        rows = taxonomy_rows(report)
        self.assertTrue(rows)
        self.assertNotIn(laws.CLEAN, {r["verdict"] for r in rows})
        self.assertNotIn("COVERED", {r["verdict"] for r in rows})
        self.assertTrue(all(r["verdict"] in {"GAP", "PARTIAL"} for r in rows))

    def test_llm_hypothesis_never_covers(self):
        """LLM is READS. taxonomy_rows must not paint COVERED from a hypothesis."""
        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="llm", status="ok", findings=[
                _finding("llm", laws.HYPOTHESIS, cls="INT-SIGNED-OVF",
                         message="maybe overflow"),
            ]),
        ]
        rows = taxonomy_rows(report)
        self.assertTrue(rows)
        ovf = next(r for r in rows if r["id"] == "INT-SIGNED-OVF")
        self.assertNotEqual(ovf["verdict"], "COVERED")
        self.assertIn(ovf["verdict"], {"PARTIAL", "GAP"})
        self.assertNotEqual(ovf["verdict"], laws.CLEAN)
        self.assertNotIn("COVERED", {r["verdict"] for r in rows})
        self.assertNotIn(laws.CLEAN, {r["verdict"] for r in rows})
        self.assertEqual(ovf["best"], laws.STRENGTH_READS)

    def test_llm_spoofed_failed_never_covers(self):
        """Even a lied FAILED/FINDS on the llm stage is READS, not COVERED."""
        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="llm", status="ok", findings=[
                Finding(
                    stage="llm", status=laws.FAILED, file="a.c", function="add",
                    line=3, cls="INT-SIGNED-OVF", message="spoof",
                    strength=laws.STRENGTH_FINDS,
                ),
            ]),
        ]
        rows = taxonomy_rows(report)
        ovf = next(r for r in rows if r["id"] == "INT-SIGNED-OVF")
        self.assertNotEqual(ovf["verdict"], "COVERED")
        self.assertEqual(ovf["best"], laws.STRENGTH_READS)
        self.assertNotIn("COVERED", {r["verdict"] for r in rows})

    def test_bmc_cover_survives_llm_hypothesis(self):
        """LLM cannot COVER, and cannot un-cover a real BMC proof."""
        report = _mixed_same_report()
        report.stages.append(StageResult(name="llm", status="ok", findings=[
            _finding("llm", laws.HYPOTHESIS, cls="INT-SIGNED-OVF",
                     message="maybe"),
        ]))
        rows = taxonomy_rows(report)
        ovf = next(r for r in rows if r["id"] == "INT-SIGNED-OVF")
        self.assertEqual(ovf["verdict"], "COVERED")
        self.assertNotEqual(ovf["verdict"], laws.CLEAN)
        findings = finding_rows(report)
        self.assertIn(laws.HYPOTHESIS, [r["status"] for r in findings])
        self.assertIn(laws.PROVED_UNBOUNDED, [r["status"] for r in findings])

    def test_ignores_stale_taxonomy_attribute(self):
        """GUI taxonomy is coverage_from_report, not a planted COVERED key."""
        report = RunReport(root="mem")
        report.taxonomy = [  # type: ignore[attr-defined]
            {"id": "INT-SIGNED-OVF", "verdict": "COVERED", "best": "PROVES"},
        ]
        rows = taxonomy_rows(report)
        ovf = next(r for r in rows if r["id"] == "INT-SIGNED-OVF")
        self.assertEqual(ovf["verdict"], "GAP")
        self.assertNotEqual(ovf["verdict"], "COVERED")
        src = (ROOT / "prism" / "gui.py").read_text(encoding="utf-8")
        body = src.split("def taxonomy_rows", 1)[1].split("def finding_rows", 1)[0]
        self.assertIn("coverage_from_report", body)
        self.assertNotIn("coverage_row(", body)

    def test_clean_palette_is_never_covered_or_proof_green(self):
        """CLEAN is blue. COVERED reuses PROVED green. They are not the same."""
        clean = _STATUS_BG[laws.CLEAN]
        proved = _STATUS_BG[laws.PROVED]
        unbounded = _STATUS_BG["PROVED-UNBOUNDED"]
        assuming = _STATUS_BG["PROVED-ASSUMING"]
        self.assertEqual(clean, "#2a4a6b")
        self.assertEqual(proved, "#2a8148")
        self.assertEqual(unbounded, "#1f6f3a")
        self.assertNotEqual(clean, proved)
        self.assertNotEqual(clean, unbounded)
        self.assertNotEqual(clean, assuming)
        self.assertNotEqual(clean, _STATUS_BG[laws.NOTRUN])
        bounded = _STATUS_BG["BOUNDED"]
        self.assertEqual(bounded, "#6b6b2a")
        self.assertNotEqual(bounded, clean)
        self.assertNotEqual(bounded, proved)
        self.assertEqual(status_background(laws.CLEAN), clean)
        self.assertNotIn(status_background(laws.CLEAN), _PROOF_GREEN)
        self.assertEqual(status_background(laws.PROVED), proved)
        self.assertIn(status_background(laws.PROVED), _PROOF_GREEN)
        self.assertEqual(taxonomy_background("COVERED"), proved)
        self.assertNotEqual(taxonomy_background("COVERED"), clean)
        self.assertEqual(taxonomy_background("PARTIAL"), bounded)
        self.assertNotEqual(taxonomy_background("PARTIAL"), proved)
        self.assertNotEqual(taxonomy_background("PARTIAL"), clean)
        self.assertEqual(taxonomy_background(laws.CLEAN), clean)
        self.assertNotIn(taxonomy_background(laws.CLEAN), _PROOF_GREEN)
        src = Path(ROOT / "prism" / "gui.py").read_text(encoding="utf-8")
        self.assertIn("status_background", src)
        self.assertIn("taxonomy_background", src)
        self.assertIn('_STATUS_BG["PROVED"]', src)
        self.assertIn('_STATUS_BG["BOUNDED"]', src)
        for ln in src.splitlines():
            if "_STATUS_BG[\"CLEAN\"]" in ln or "_STATUS_BG['CLEAN']" in ln:
                self.assertNotIn("COVERED", ln)
                self.assertNotIn("PROVED", ln)
                self.assertNotIn("PARTIAL", ln)

    def test_refuse_llm_cover_demotes_reads_and_only_llm_hits(self):
        empty = RunReport(root="mem")
        self.assertEqual(
            refuse_llm_cover(empty, "INT-SIGNED-OVF", "COVERED", laws.STRENGTH_READS),
            "PARTIAL",
        )
        self.assertEqual(
            refuse_llm_cover(empty, "INT-SIGNED-OVF", "GAP", laws.STRENGTH_READS),
            "GAP",
        )
        llm_only = RunReport(root="mem")
        llm_only.stages = [
            StageResult(name="llm", status="ok", findings=[
                _finding("llm", laws.HYPOTHESIS, cls="INT-SIGNED-OVF",
                         message="maybe"),
            ]),
        ]
        self.assertEqual(
            refuse_llm_cover(llm_only, "INT-SIGNED-OVF", "COVERED", laws.STRENGTH_PROVES),
            "PARTIAL",
        )
        mixed = _mixed_same_report()
        self.assertEqual(
            refuse_llm_cover(mixed, "INT-SIGNED-OVF", "COVERED", laws.STRENGTH_PROVES),
            "COVERED",
        )


class TestMissingPyside(unittest.TestCase):
    def test_import_status_is_never_clean(self):
        st = gui_import_status()
        self.assertNotEqual(st, laws.CLEAN)
        if HAS_PYSIDE6:
            self.assertEqual(st, "available")
            self.assertIsNone(missing_pyside_finding())
        else:
            self.assertEqual(st, laws.NOTRUN)
            miss = missing_pyside_finding()
            self.assertIsNotNone(miss)
            assert miss is not None
            self.assertEqual(miss.status, laws.NOTRUN)
            self.assertNotEqual(miss.status, laws.CLEAN)

    def test_missing_pyside6_is_notrun_never_clean(self):
        """Always asserted — does not depend on whether PySide6 is installed."""
        with mock.patch("prism.gui.HAS_PYSIDE6", False):
            self.assertEqual(gui_import_status(), laws.NOTRUN)
            self.assertNotEqual(gui_import_status(), laws.CLEAN)
            miss = missing_pyside_finding()
            self.assertIsNotNone(miss)
            assert miss is not None
            self.assertEqual(miss.status, laws.NOTRUN)
            self.assertNotEqual(miss.status, laws.CLEAN)
            self.assertEqual(miss.stage, "gui")
            self.assertIn("PySide6", miss.message)
            self.assertNotIn(laws.CLEAN, miss.message)

    def test_launch_without_pyside_writes_notrun(self):
        buf = io.StringIO()
        with mock.patch("prism.gui._ensure_qt",
                        side_effect=ImportError("No module named 'PySide6'")), \
             mock.patch("sys.stdout", buf):
            rc = launch()
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("NOTRUN", text)
        self.assertIn("not a clean window", text)
        self.assertNotIn(laws.CLEAN, text.split())

    def test_cli_main_gui_launch_import_error_is_notrun(self):
        """python -m prism --gui: prism.gui.launch ImportError is NOTRUN, never CLEAN."""
        from prism.__main__ import main

        class _NoLaunch:
            def __getattr__(self, name: str) -> object:
                raise ImportError("No module named 'PySide6'")

        buf = io.StringIO()
        with mock.patch.dict(sys.modules, {"prism.gui": _NoLaunch()}), \
             mock.patch("prism.__main__.run_pipeline",
                        side_effect=AssertionError("gui must not run pipeline")), \
             mock.patch("sys.stdout", buf):
            rc = main(["--gui"])
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("NOTRUN", text)
        self.assertIn("not a clean window", text)
        self.assertIn("PySide6", text)
        self.assertNotIn(laws.CLEAN, text.split())
        self.assertNotIn("confidence", text)

    def test_cli_main_gui_pyside_import_error_is_notrun(self):
        """python -m prism --gui: PySide import failing in launch is NOTRUN, exit 0."""
        from prism.__main__ import main

        buf = io.StringIO()
        with mock.patch("prism.gui._ensure_qt",
                        side_effect=ImportError("No module named 'PySide6'")), \
             mock.patch.dict(sys.modules, {
                 "PySide6": None,
                 "PySide6.QtWidgets": None,
             }), \
             mock.patch("prism.__main__.run_pipeline",
                        side_effect=AssertionError("gui must not run pipeline")), \
             mock.patch("sys.stdout", buf):
            rc = main(["--gui"])
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("NOTRUN", text)
        self.assertIn("not a clean window", text)
        self.assertNotIn(laws.CLEAN, text.split())
        self.assertNotIn("confidence", text)

    def test_missing_display_finding_is_notrun_never_clean(self):
        miss = missing_display_finding("qt.qpa.plugin")
        self.assertEqual(miss.status, laws.NOTRUN)
        self.assertNotEqual(miss.status, laws.CLEAN)
        self.assertEqual(miss.stage, "gui")
        self.assertIn("NOTRUN", miss.message)
        self.assertIn("display", miss.message.lower())
        self.assertNotIn(laws.CLEAN, miss.message)
        self.assertFalse(laws.is_proof(miss.status))

    def test_launch_without_display_writes_notrun(self):
        """Missing display is NOTRUN. Never opens a window. Never CLEAN."""
        import types

        buf = io.StringIO()

        class BoomApp:
            @staticmethod
            def instance():
                return None

            def __init__(self, *a, **k):
                raise RuntimeError("qt.qpa.plugin: Could not load the Qt platform plugin")

        widgets = types.ModuleType("PySide6.QtWidgets")
        widgets.QApplication = BoomApp
        pyside = types.ModuleType("PySide6")
        with mock.patch("prism.gui._ensure_qt"), \
             mock.patch.dict(sys.modules, {
                 "PySide6": pyside,
                 "PySide6.QtWidgets": widgets,
             }), \
             mock.patch("sys.stdout", buf):
            rc = launch()
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("NOTRUN", text)
        self.assertIn("no display", text)
        self.assertIn("not a clean window", text)
        self.assertNotIn(laws.CLEAN, text.split())
        self.assertNotIn("confidence", text)


class TestCppGuiSameReport(unittest.TestCase):
    """C++ prism_gui loads the same JSON as CLI. Source contract, no display."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cpp = CPP_MAIN.read_text(encoding="utf-8")
        cls.hdr = CPP_HDR.read_text(encoding="utf-8")

    def test_q_object_stays_in_header(self):
        self.assertIn("Q_OBJECT", self.hdr)
        self.assertNotIn("Q_OBJECT", self.cpp)

    def test_loads_report_and_taxonomy_from_prism_out_gui(self):
        self.assertIn("prism-out-gui", self.cpp)
        self.assertIn("--out", self.cpp)
        self.assertIn('QStringLiteral("/report.json")', self.cpp)
        self.assertNotIn('QStringLiteral("/taxonomy.json")', self.cpp)
        self.assertIn("loadSameReport", self.cpp)
        self.assertIn("onDone", self.cpp)
        # Same JSON keys the CLI report uses.
        for key in ("visibility", "answer", "resolution", "confidence",
                    "stages", "findings", "status", "stage", "cls"):
            self.assertIn(key, self.cpp)

    def test_confidence_format_matches_cli(self):
        self.assertIn(
            "confidence %1  (vis %2 x ans %3 x res %4)",
            self.cpp,
        )
        # Product string is built from JSON numbers, not a hard-coded CLEAN.
        self.assertIn('obj.value(QStringLiteral("visibility"))', self.cpp)
        self.assertIn('obj.value(QStringLiteral("answer"))', self.cpp)
        self.assertIn('obj.value(QStringLiteral("resolution"))', self.cpp)
        self.assertIn('obj.value(QStringLiteral("confidence"))', self.cpp)
        self.assertIn("visibility %1", self.cpp)
        self.assertIn("answer %1", self.cpp)
        self.assertIn("resolution %1", self.cpp)
        # Empty-scope law: missing report.json is 0, never n/a, never a proof.
        self.assertNotIn("n/a", self.cpp)
        self.assertNotIn("N/A", self.cpp)
        self.assertIn(
            "confidence 0  (vis 0 x ans 0 x res 0) — report.json missing; not a proof",
            self.cpp,
        )

    def test_cpp_taxonomy_from_coverage_from_report(self):
        """COVERED/GAP/PARTIAL come from coverage_from_report, then CLEAN→GAP.

        Empty taxArr clears the table. It must not open taxonomy.json.
        """
        load = self.cpp.split("void MainWindow::loadSameReport", 1)[1]
        load = load.split("void MainWindow::onDone", 1)[0]
        self.assertIn("coverage_from_report", load)
        self.assertNotIn('QStringLiteral("/taxonomy.json")', load)
        self.assertIn("taxArr.isEmpty()", load)
        after_empty = load.split("taxArr.isEmpty()", 1)[1]
        self.assertIn("setRowCount(0)", after_empty)
        self.assertNotIn("QFile", after_empty)
        self.assertIn('QLatin1String("COVERED")', self.cpp)
        self.assertIn('QLatin1String("PARTIAL")', self.cpp)
        self.assertIn('QLatin1String("GAP")', self.cpp)

    def test_keeps_proved_clean_notrun_distinct(self):
        self.assertIn('QLatin1String("NOTRUN")', self.cpp)
        self.assertIn('QLatin1String("CLEAN")', self.cpp)
        self.assertIn("status", self.cpp)
        self.assertNotIn("PROVED = CLEAN", self.cpp)
        self.assertNotIn("CLEAN = NOTRUN", self.cpp)

    def test_finding_rows_match_report_json_cpp_would_load(self):
        report = _mixed_same_report()
        report.stages.insert(0, StageResult(name="unify", status="ok", findings=[
            _finding("unify", laws.CLEAN, message="taxonomy 0/0"),
        ]))
        payload = json.loads(report.dumps())
        py_rows = finding_rows(report)
        cpp_rows = _cpp_style_finding_rows(payload)
        self.assertEqual(py_rows, cpp_rows)
        _assert_mixed_honesty(self, cpp_rows)

    def test_cpp_copies_json_status_verbatim(self):
        """loadSameReport paints status from JSON; it does not invent a proof."""
        self.assertIn('f.value(QStringLiteral("status"))', self.cpp)
        self.assertIn("QStringList cols{", self.cpp)
        # First table column is the JSON status string, not a remapped proof.
        after = self.cpp.split("QStringList cols{", 1)[1]
        first_col = after.split(",", 1)[0]
        self.assertIn("status", first_col)
        self.assertNotIn("PROVED", first_col)
        self.assertNotIn("CLEAN", first_col)

    def test_constructor_loads_existing_report_json(self):
        """Ctor after QProcess connects: loadSameReport if report.json exists.

        Same CLI vocabulary: confidence product line, JSON status copied
        verbatim into the table. Missing report.json is not a proof.
        """
        ctor = self.cpp.split("MainWindow::MainWindow", 1)[1]
        ctor = ctor.split("QString MainWindow::prismBinary", 1)[0]
        marker = "QProcess::readyReadStandardError"
        self.assertIn(marker, ctor)
        after = ctor.split(marker, 1)[1]
        self.assertIn("prism-out-gui", after)
        self.assertIn('QStringLiteral("/report.json")', after)
        self.assertIn("QFileInfo::exists", after)
        self.assertIn("loadSameReport", after)
        self.assertIn(
            "confidence %1  (vis %2 x ans %3 x res %4)",
            self.cpp,
        )
        self.assertIn('f.value(QStringLiteral("status"))', self.cpp)
        cols = self.cpp.split("QStringList cols{", 1)[1]
        first_col = cols.split(",", 1)[0]
        self.assertIn("status", first_col)
        self.assertNotIn("PROVED", first_col)
        self.assertNotIn("CLEAN", first_col)
        self.assertIn(
            "confidence 0  (vis 0 x ans 0 x res 0) — report.json missing; not a proof",
            self.cpp,
        )
        self.assertNotIn("confidence — (report.json missing", self.cpp)
        self.assertNotIn("missing; CLEAN", self.cpp)
        self.assertNotIn("missing; PROVED", self.cpp)

    def test_taxonomy_covered_gap_from_report_json(self):
        """Same COVERED/GAP vocabulary as the Python engine. CLEAN is never COVERED."""
        self.assertIn('QStringLiteral("taxonomy")', self.cpp)
        self.assertNotIn('QStringLiteral("/taxonomy.json")', self.cpp)
        self.assertIn("coverage_from_report", self.cpp)
        self.assertIn('QLatin1String("COVERED")', self.cpp)
        self.assertIn('QLatin1String("GAP")', self.cpp)
        self.assertIn('QLatin1String("PARTIAL")', self.cpp)
        self.assertIn("taxonomyVerdictBg", self.cpp)
        self.assertIn("findingStatusBg", self.cpp)
        # COVERED uses PROVED green 0x2a8148, not CLEAN blue 0x2a4a6b.
        self.assertIn("QColor(0x2a, 0x81, 0x48)", self.cpp)
        self.assertIn("QColor(0x2a, 0x4a, 0x6b)", self.cpp)
        covered = self.cpp.split("taxonomyVerdictBg", 1)[1]
        covered = covered.split("fillTaxonomyTable", 1)[0]
        self.assertIn('QLatin1String("COVERED")', covered)
        self.assertIn('QLatin1String("GAP")', covered)
        self.assertIn("QColor(0x2a, 0x81, 0x48)", covered)
        self.assertIn("QColor(0x6b, 0x5a, 0x2a)", covered)
        self.assertNotIn("QColor(0x8b, 0x2e, 0x2e)", covered)
        # CLEAN finding status is blue, never the COVERED/PROVED green.
        status_fn = self.cpp.split("findingStatusBg", 1)[1]
        status_fn = status_fn.split("taxonomyVerdictBg", 1)[0]
        self.assertIn('QLatin1String("CLEAN")', status_fn)
        clean_line = next(
            ln for ln in status_fn.splitlines() if 'QLatin1String("CLEAN")' in ln
        )
        self.assertIn("QColor(0x2a, 0x4a, 0x6b)", clean_line)
        self.assertNotIn("0x2a, 0x81, 0x48", clean_line)
        self.assertNotIn("0x1f, 0x6f, 0x3a", clean_line)
        self.assertIn('verdict == QLatin1String("CLEAN")', self.cpp)
        self.assertIn('QStringLiteral("GAP")', self.cpp)
        # Taxonomy CLEAN (if it ever arrived) is the same blue as finding CLEAN,
        # never COVERED/PROVED green 0x2a8148.
        verdict_fn = self.cpp.split("taxonomyVerdictBg", 1)[1]
        verdict_fn = verdict_fn.split("fillTaxonomyTable", 1)[0]
        clean_verdict = next(
            ln for ln in verdict_fn.splitlines() if 'QLatin1String("CLEAN")' in ln
        )
        self.assertIn("QColor(0x2a, 0x4a, 0x6b)", clean_verdict)
        self.assertNotIn("0x2a, 0x81, 0x48", clean_verdict)
        self.assertNotIn("0x1f, 0x6f, 0x3a", clean_verdict)
        covered_line = next(
            ln for ln in verdict_fn.splitlines() if 'QLatin1String("COVERED")' in ln
        )
        self.assertIn("QColor(0x2a, 0x81, 0x48)", covered_line)
        self.assertNotIn("0x2a, 0x4a, 0x6b", covered_line)
        # fillTaxonomyTable remaps a CLEAN verdict to GAP before paint.
        fill = self.cpp.split("fillTaxonomyTable", 1)[1]
        fill = fill.split("MainWindow::MainWindow", 1)[0]
        self.assertIn('verdict == QLatin1String("CLEAN")', fill)
        remap = fill.split('verdict == QLatin1String("CLEAN")', 1)[1]
        remap = remap.split("const int row", 1)[0]
        self.assertIn('QStringLiteral("GAP")', remap)
        self.assertNotIn("COVERED", remap)


def _cpp_style_finding_rows(obj: dict) -> list[dict[str, str]]:
    """Mirror src/gui/MainWindow.cpp loadSameReport finding filter. No Qt."""
    noise = {"inventory", "classify", "unify"}
    rows: list[dict[str, str]] = []
    for s in obj.get("stages") or []:
        name = s.get("name") or ""
        for f in s.get("findings") or []:
            status = f.get("status") or ""
            if status in {laws.NOTRUN, laws.CLEAN} and name in noise:
                continue
            line = f.get("line")
            rows.append({
                "status": status,
                "stage": f.get("stage") or name,
                "cls": f.get("cls") or "",
                "file": f.get("file") or "",
                "line": "" if line is None else str(int(line)),
                "function": f.get("function") or "",
                "message": (f.get("message") or "")[:200],
            })
    return rows


@unittest.skipUnless(HAS_PYSIDE6, "PySide6 not installed")
class TestMainWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
        from PySide6.QtWidgets import QApplication

        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as ex:
            raise unittest.SkipTest(f"NOTRUN gui display: {ex}") from ex

    def test_main_window_constructs(self):
        from prism.gui import MainWindow

        with tempfile.TemporaryDirectory() as td:
            prev = os.getcwd()
            os.chdir(td)
            try:
                w = MainWindow()
                try:
                    self.assertFalse(w.isVisible())
                    self.assertFalse(w.skip_fuzz_ck.isChecked())
                    self.assertFalse(w.skip_repair_ck.isChecked())
                    self.assertFalse(w.skip_optional_ck.isChecked())
                    self.assertTrue(w.llm_ck.isChecked())
                    self.assertEqual(
                        [w.findings.horizontalHeaderItem(i).text()
                         for i in range(w.findings.columnCount())],
                        list(FINDING_COLUMNS),
                    )
                    self.assertEqual(w.s_conf.text(), MISSING_REPORT_LABEL)
                    self.assertNotIn("n/a", w.s_conf.text().lower())
                    self.assertNotIn(laws.CLEAN, w.s_conf.text())
                    self.assertIn("0", w.s_vis.text())
                finally:
                    w.close()
            finally:
                os.chdir(prev)

    def test_offscreen_window_shows_same_report_statuses(self):
        """Tiny report.json: PROVED-UNBOUNDED, CLEAN fuzz, NOTRUN esbmc.

        Missing ESBMC must not display as proved. No display required.
        """
        from prism.gui import MainWindow

        report = _mixed_same_report()
        expected = finding_rows(report)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            report.save(root / "prism-out" / "report.json")
            loaded = RunReport.load(root / "prism-out" / "report.json")
            self.assertIsNotNone(loaded)
            assert loaded is not None
            prev = os.getcwd()
            os.chdir(td)
            try:
                w = MainWindow()
                try:
                    self._app.processEvents()
                    # Constructor loads prism-out/report.json via _load_last.
                    # Apply again so the test does not depend on cwd side effects.
                    w._on_done(loaded)
                    self._app.processEvents()
                    rows = self._finding_table_rows(w)
                    _assert_mixed_honesty(self, rows)
                    self.assertEqual(rows, expected)
                    self.assertEqual(w.s_conf.text(), confidence_label(report))
                    tax = self._taxonomy_table_rows(w)
                    self.assertTrue(tax)
                    verdicts = {r["verdict"] for r in tax}
                    self.assertIn("COVERED", verdicts)
                    self.assertIn("GAP", verdicts)
                    self.assertNotIn(laws.CLEAN, verdicts)
                    log = w.log.toPlainText()
                    self.assertIn("esbmc", log.lower() + "".join(r["stage"] for r in rows))
                    self.assertIn("NOTRUN", log)
                    self.assertNotIn("esbmc PROVED", log)
                    from PySide6.QtGui import QColor
                    proof_green = QColor(_STATUS_BG[laws.PROVED])
                    clean_blue = QColor(_STATUS_BG[laws.CLEAN])
                    self.assertNotEqual(clean_blue, proof_green)
                    for r in range(w.findings.rowCount()):
                        cell = w.findings.item(r, 0)
                        status = cell.text() if cell else ""
                        bg = cell.background().color() if cell else QColor()
                        if status == laws.CLEAN:
                            self.assertEqual(bg, clean_blue)
                            self.assertNotEqual(bg, proof_green)
                            self.assertNotEqual(bg, QColor(_STATUS_BG["PROVED-UNBOUNDED"]))
                        if status in {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}:
                            self.assertNotEqual(bg, clean_blue)
                    for r in range(w.taxonomy.rowCount()):
                        cell = w.taxonomy.item(r, 1)
                        verdict = cell.text() if cell else ""
                        bg = cell.background().color() if cell else QColor()
                        self.assertNotEqual(verdict, laws.CLEAN)
                        if verdict == "COVERED":
                            self.assertEqual(bg, proof_green)
                            self.assertNotEqual(bg, clean_blue)
                        if verdict == "PARTIAL":
                            self.assertEqual(bg, QColor(_STATUS_BG["BOUNDED"]))
                            self.assertNotEqual(bg, clean_blue)
                            self.assertNotEqual(bg, proof_green)
                finally:
                    w.close()
            finally:
                os.chdir(prev)

    def test_offscreen_llm_does_not_cover_or_paint_clean_green(self):
        """LLM hypothesis is PARTIAL/READS. Never COVERED, never proof-green."""
        from prism.gui import MainWindow
        from PySide6.QtGui import QColor

        report = RunReport(root="mem")
        report.stages = [
            StageResult(name="llm", status="ok", findings=[
                _finding("llm", laws.HYPOTHESIS, cls="INT-SIGNED-OVF",
                         message="maybe overflow"),
            ]),
        ]
        with tempfile.TemporaryDirectory() as td:
            prev = os.getcwd()
            os.chdir(td)
            try:
                w = MainWindow()
                try:
                    w._on_done(report)
                    self._app.processEvents()
                    self.assertEqual(w.s_conf.text(), confidence_label(report))
                    self.assertNotIn("n/a", w.s_conf.text().lower())
                    rows = self._finding_table_rows(w)
                    self.assertEqual([r["status"] for r in rows], [laws.HYPOTHESIS])
                    tax = self._taxonomy_table_rows(w)
                    ovf = next(r for r in tax if r["id"] == "INT-SIGNED-OVF")
                    self.assertNotEqual(ovf["verdict"], "COVERED")
                    self.assertNotEqual(ovf["verdict"], laws.CLEAN)
                    self.assertIn(ovf["verdict"], {"PARTIAL", "GAP"})
                    proof_green = QColor(_STATUS_BG[laws.PROVED])
                    clean_blue = QColor(_STATUS_BG[laws.CLEAN])
                    for r in range(w.taxonomy.rowCount()):
                        cell = w.taxonomy.item(r, 1)
                        verdict = cell.text() if cell else ""
                        bg = cell.background().color() if cell else QColor()
                        self.assertNotEqual(verdict, laws.CLEAN)
                        self.assertNotEqual(verdict, "COVERED")
                        if verdict == "PARTIAL":
                            self.assertEqual(bg, QColor(_STATUS_BG["BOUNDED"]))
                            self.assertNotEqual(bg, proof_green)
                            self.assertNotEqual(bg, clean_blue)
                finally:
                    w.close()
            finally:
                os.chdir(prev)

    @staticmethod
    def _finding_table_rows(w) -> list[dict[str, str]]:
        rows: list[dict[str, str]] = []
        for r in range(w.findings.rowCount()):
            item = {
                key: (w.findings.item(r, c).text() if w.findings.item(r, c) else "")
                for c, key in enumerate(FINDING_COLUMNS)
            }
            rows.append(item)
        return rows

    @staticmethod
    def _taxonomy_table_rows(w) -> list[dict[str, str]]:
        rows: list[dict[str, str]] = []
        for r in range(w.taxonomy.rowCount()):
            rows.append({
                "id": w.taxonomy.item(r, 0).text() if w.taxonomy.item(r, 0) else "",
                "verdict": w.taxonomy.item(r, 1).text() if w.taxonomy.item(r, 1) else "",
                "best": w.taxonomy.item(r, 2).text() if w.taxonomy.item(r, 2) else "",
            })
        return rows


if __name__ == "__main__":
    unittest.main()
