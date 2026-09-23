"""The conc stage (threads and atomics by lazy sequentialisation, roadmap 2.6;
docs/CONCURRENCY.md).

The C++ engine runs it on tests/conc when PRISM_BIN is set; the Python
engine is frozen (roadmap D8) and records a single NOTRUN row for the stage.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.pipeline import EXEC_STAGES, STAGE_ORDER, run_conc_notrun

ROOT = Path(__file__).resolve().parents[1]
TASKS = ROOT / "tests" / "conc"
PIPELINE_CPP = (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8")

# file -> (status of the main harness, classes that must be among the FAILED rows)
EXPECTED: dict[str, tuple[str, set[str]]] = {
    "racy_counter.c": (laws.FAILED, {"CONC-DATA-RACE"}),
    "mutex_counter.c": (laws.BOUNDED, set()),
    "lost_update.c": (laws.FAILED, {"FUNC-CONTRACT", "CONC-DATA-RACE"}),
    "deadlock.c": (laws.FAILED, {"CONC-DEADLOCK"}),
    "atomic_counter.c": (laws.BOUNDED, set()),
    "join_then_read.c": (laws.BOUNDED, set()),
    "relaxed.c": (laws.NEEDS_HARNESS, set()),
}


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return Path(env)
    return None


def _clang() -> bool:
    return all(shutil.which(t) for t in ("clang", "opt"))


class TestPythonEngineFrozen(unittest.TestCase):
    """D8: the Python engine lists the stage and says it did not run it."""

    def test_stage_after_pir_in_both_engines(self):
        self.assertEqual(STAGE_ORDER[STAGE_ORDER.index("pir") + 1], "conc")
        self.assertIn('stage("conc"', PIPELINE_CPP)
        hpp = (ROOT / "include" / "prism" / "pipeline.hpp").read_text(encoding="utf-8")
        self.assertIn('"pir", "conc", "harness"', hpp)

    def test_notrun_row(self):
        rows = run_conc_notrun()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].stage, "conc")
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertIn("roadmap D8", rows[0].message)

    def test_executes_nothing(self):
        # Law 9: clang/opt compile only, Z3 in-process.
        self.assertNotIn("conc", EXEC_STAGES)

    def test_audit_origin_is_solver(self):
        self.assertEqual(laws.stage_origin("conc"), laws.ORIGIN_SOLVER)

    def test_never_proves(self):
        # Law 2: a context-switch bound is not a proof; no PROVED* status in the stage.
        src = "".join(p.read_text(encoding="utf-8") for p in (ROOT / "src" / "prism" / "conc").glob("*.cpp"))
        self.assertNotIn("laws::PROVED", src)
        self.assertIn("laws::BOUNDED", src)


@unittest.skipUnless(_cpp_prism() and _clang(), "needs PRISM_BIN and clang/opt on PATH")
class TestConcStage(unittest.TestCase):
    report: dict = {}

    @classmethod
    def setUpClass(cls) -> None:
        with tempfile.TemporaryDirectory(prefix="prism_conc_") as td:
            out = Path(td) / "out"
            subprocess.run(
                [str(_cpp_prism()), str(TASKS), "--no-llm", "--stage", "conc", "--out", str(out),
                 "--jobs", "2"],
                cwd=ROOT, capture_output=True, text=True, timeout=1800,
            )
            cls.report = json.loads((out / "report.json").read_text(encoding="utf-8"))

    def _rows(self) -> list[dict]:
        st = next(s for s in self.report["stages"] if s["name"] == "conc")
        self.assertEqual(st["status"], "ok")
        return st["findings"]

    def test_expected_verdicts(self):
        by_file: dict[str, list[dict]] = {}
        for f in self._rows():
            by_file.setdefault(Path(f["file"]).name, []).append(f)
        for name, (status, classes) in EXPECTED.items():
            rows = [f for f in by_file.get(name, []) if f.get("function") == "main"]
            self.assertTrue(rows, name)
            self.assertEqual({r["status"] for r in rows}, {status}, name)
            self.assertLessEqual(classes, {r["cls"] for r in rows}, name)

    def test_no_threads_no_findings(self):
        self.assertFalse([f for f in self._rows() if Path(f["file"]).name == "no_threads.c"])

    def test_never_proved(self):
        for f in self._rows():
            self.assertFalse(laws.is_proof(f["status"]), f)

    def test_failed_rows_carry_a_schedule(self):
        for f in self._rows():
            if f["status"] != laws.FAILED:
                continue
            self.assertTrue(f["counterexample"], f)
            self.assertEqual(f["extra"]["schedule"], f["counterexample"])
            self.assertIn("T0", f["evidence"])
            self.assertEqual(f["extra"]["method"], "lazy sequentialisation (Lazy-CSeq), sequentially consistent")

    def test_relaxed_names_the_order(self):
        rows = [f for f in self._rows() if Path(f["file"]).name == "relaxed.c"]
        self.assertTrue(rows[0]["message"].startswith("UNENCODED: memory_order_relaxed"))

    def test_bounded_names_the_bound(self):
        rows = [f for f in self._rows() if Path(f["file"]).name == "mutex_counter.c"]
        self.assertIn("not a proof", rows[0]["message"])
        self.assertEqual(rows[0]["extra"]["rounds"], "2")


if __name__ == "__main__":
    unittest.main()
