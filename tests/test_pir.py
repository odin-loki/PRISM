"""The pir stage (Clang -> LLVM IR -> PIR -> Z3, roadmap Part 2; docs/PIR.md).

The C++ engine runs it on tests/pir (true and false variants per construct)
when PRISM_BIN is set; the Python engine is frozen (roadmap D8) and records a
single NOTRUN row for the stage.
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
from prism.pipeline import EXEC_STAGES, STAGE_ORDER, run_pir_notrun

ROOT = Path(__file__).resolve().parents[1]
TASKS = ROOT / "tests" / "pir"
PIPELINE_CPP = (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8")

# (file, function) -> expected status. *_bad = a real defect (FAILED with a
# counterexample), *_ok = PROVED, loops that do not close = BOUNDED, closed
# k-induction = PROVED-UNBOUNDED, unmodelled constructs = NEEDS-HARNESS.
EXPECTED: dict[tuple[str, str], str] = {
    ("overflow.c", "add_bad"): laws.FAILED,
    ("overflow.c", "add_ok"): laws.PROVED,
    ("overflow.c", "add_unsigned_ok"): laws.PROVED,
    ("overflow.c", "mul_bad"): laws.FAILED,
    ("overflow.c", "neg_bad"): laws.FAILED,
    ("div0.c", "div_bad"): laws.FAILED,
    ("div0.c", "div_ok"): laws.PROVED,
    ("div0.c", "mod_min_bad"): laws.FAILED,
    ("div0.c", "urem_bad"): laws.FAILED,
    ("shift.c", "shl_amount_bad"): laws.FAILED,
    ("shift.c", "shl_masked_ok"): laws.PROVED,
    ("shift.c", "shl_sign_bad"): laws.FAILED,
    ("shift.c", "shl_sign_ok"): laws.PROVED,
    ("shift.c", "ashr_ok"): laws.PROVED,
    ("loops.c", "count4_ok"): laws.PROVED,
    ("loops.c", "loop_ovf_bad"): laws.FAILED,
    ("loops.c", "loop_long_bounded"): laws.BOUNDED,
    ("loops.c", "nested_ok"): laws.PROVED,
    ("kinduct.c", "kind_closed"): laws.PROVED_UNBOUNDED,
    ("kinduct.c", "kind_countdown"): laws.PROVED_UNBOUNDED,
    ("kinduct.c", "kind_open"): laws.BOUNDED,
    ("switch.c", "sw_bad"): laws.FAILED,
    ("switch.c", "sw_ok"): laws.PROVED,
    ("select.c", "max_ok"): laws.PROVED,
    ("select.c", "sel_div_bad"): laws.FAILED,
    ("select.c", "abs_bad"): laws.FAILED,
    ("select.c", "abs_ok"): laws.PROVED,
    ("casts.c", "trunc_ok"): laws.PROVED,
    ("casts.c", "widen_bad"): laws.FAILED,
    ("casts.c", "widen_ok"): laws.PROVED,
    ("casts.c", "uchar_ok"): laws.PROVED,
    ("casts.c", "sext_mul_ok"): laws.PROVED,
    ("uninit.c", "uninit_bad"): laws.FAILED,
    ("uninit.c", "uninit_ok"): laws.PROVED,
    ("assert.c", "assert_bad"): laws.FAILED,
    ("assert.c", "assert_ok"): laws.PROVED,
    ("unencoded.c", "deref_ptr"): laws.NEEDS_HARNESS,
    ("unencoded.c", "local_array"): laws.NEEDS_HARNESS,
    ("unencoded.c", "twice_double"): laws.NEEDS_HARNESS,
    ("unencoded.c", "recurse"): laws.NEEDS_HARNESS,
    ("calls.c", "helper"): laws.FAILED,
    ("calls.c", "caller_ok"): laws.PROVED,
    ("calls.c", "caller_bad"): laws.FAILED,
    ("folded.c", "fold_shift_bad"): laws.FAILED,
    ("folded.c", "fold_div_bad"): laws.FAILED,
    ("folded.c", "fold_ok"): laws.PROVED,
    ("cxx.cpp", "cxx_add_bad"): laws.FAILED,
    ("cxx.cpp", "use_twice_ok"): laws.PROVED,
    ("cxx.cpp", "get"): laws.NEEDS_HARNESS,  # `this` is a pointer (Law 6)
    ("cxx.cpp", "cxx_shl_ok"): laws.PROVED,  # C++20+: signed << is defined
    ("cxx.cpp", "constexpr_ok"): laws.PROVED,
}

CLASSES = {
    ("overflow.c", "add_bad"): "INT-SIGNED-OVF",
    ("div0.c", "div_bad"): "INT-DIV-ZERO",
    ("shift.c", "shl_amount_bad"): "INT-SHIFT-UB",
    ("shift.c", "shl_sign_bad"): "INT-SHIFT-UB",
    ("uninit.c", "uninit_bad"): "UNINIT-READ",
    ("assert.c", "assert_bad"): "FUNC-CONTRACT",
    ("switch.c", "sw_bad"): "INT-DIV-ZERO",
}


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return Path(env)
    return None


def _clang() -> bool:
    return all(shutil.which(t) for t in ("clang", "clang++", "opt"))


class TestPythonEngineFrozen(unittest.TestCase):
    """D8: the Python engine lists the stage and says it did not run it."""

    def test_stage_after_bmc_in_both_engines(self):
        self.assertEqual(STAGE_ORDER[STAGE_ORDER.index("bmc") + 1], "pir")
        self.assertIn('stage("pir"', PIPELINE_CPP)

    def test_notrun_row(self):
        rows = run_pir_notrun()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].stage, "pir")
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertIn("roadmap D8", rows[0].message)

    def test_exec_table(self):
        # Law 9: the lli translation-validation half is the executing part.
        self.assertEqual(EXEC_STAGES["pir"], "part")


@unittest.skipUnless(_cpp_prism() and _clang(), "needs PRISM_BIN and clang/clang++/opt on PATH")
class TestPirStage(unittest.TestCase):
    report: dict = {}
    report_exec: dict = {}

    @classmethod
    def _run(cls, *extra: str) -> dict:
        with tempfile.TemporaryDirectory(prefix="prism_pir_") as td:
            out = Path(td) / "out"
            subprocess.run(
                [str(_cpp_prism()), str(TASKS), "--no-llm", "--stage", "pir", "--out", str(out),
                 "--jobs", "2", *extra],
                cwd=ROOT, capture_output=True, text=True, timeout=1800,
            )
            return json.loads((out / "report.json").read_text(encoding="utf-8"))

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = cls._run()

    def _pir(self, report: dict) -> list[dict]:
        st = next(s for s in report["stages"] if s["name"] == "pir")
        return st["findings"]

    def _verdicts(self, report: dict) -> dict[tuple[str, str], dict]:
        out: dict[tuple[str, str], dict] = {}
        for f in self._pir(report):
            if f.get("function"):
                out.setdefault((Path(f["file"]).name, f["function"]), f)
        return out

    def test_expected_verdicts(self):
        got = self._verdicts(self.report)
        wrong = {k: (v, got.get(k, {}).get("status")) for k, v in EXPECTED.items()
                 if got.get(k, {}).get("status") != v}
        self.assertEqual(wrong, {})

    def test_failed_carries_counterexample_and_class(self):
        got = self._verdicts(self.report)
        for k, v in EXPECTED.items():
            if v != laws.FAILED:
                continue
            f = got[k]
            self.assertTrue(f["counterexample"], k)
            self.assertEqual(f["extra"]["cex"], f["counterexample"])
        for k, cls in CLASSES.items():
            self.assertEqual(got[k]["cls"], cls, k)

    def test_frontend_and_pir_hash(self):
        got = self._verdicts(self.report)
        f = got[("overflow.c", "add_ok")]
        self.assertTrue(f["extra"]["frontend"].startswith("clang-"))
        self.assertRegex(f["extra"]["pir_hash"], r"^[0-9a-f]{64}$")

    def test_unencoded_is_named(self):
        got = self._verdicts(self.report)
        self.assertIn("Law 6", got[("unencoded.c", "deref_ptr")]["message"])
        self.assertTrue(got[("unencoded.c", "local_array")]["message"].startswith("UNENCODED: "))
        self.assertEqual(got[("unencoded.c", "twice_double")]["message"],
                         "UNENCODED: parameter type double")

    def test_never_merged(self):
        # Law 2: a loop cut at the bound is BOUNDED, never PROVED.
        got = self._verdicts(self.report)
        f = got[("loops.c", "loop_long_bounded")]
        self.assertEqual(f["extra"]["unwind_closed"], "false")
        self.assertEqual(f["extra"]["k_induction"], "step-open")
        self.assertEqual(got[("kinduct.c", "kind_closed")]["extra"]["k_induction"], "closed")

    def test_law9_validation_held_back(self):
        rows = self._pir(self.report)
        got = self._verdicts(self.report)
        self.assertEqual(got[("overflow.c", "add_ok")]["extra"]["tv"], "NOTRUN (needs --allow-exec)")
        notes = [r for r in rows if r["status"] == laws.NOTRUN]
        self.assertEqual(len(notes), 1)
        self.assertEqual(notes[0]["extra"]["reason"], "executes-scanned-code")

    @unittest.skipUnless(shutil.which("lli"), "lli not on PATH")
    def test_translation_validation_with_allow_exec(self):
        rep = self._run("--allow-exec")
        got = self._verdicts(rep)
        for k, v in EXPECTED.items():
            f = got[k]
            self.assertEqual(f["status"], v, (k, f.get("message")))
            if v in (laws.NEEDS_HARNESS,):
                continue
            # PASS, or NONE when every input hits the UB (e.g. fold_shift_bad
            # takes no parameters and always shifts into the sign bit).
            tv = f["extra"]["tv"]
            self.assertTrue(tv.startswith("PASS") or tv.startswith("NONE"), (k, tv))
            if v != laws.FAILED:
                self.assertTrue(tv.startswith("PASS"), (k, tv))


if __name__ == "__main__":
    unittest.main()
