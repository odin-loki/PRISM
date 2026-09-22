"""Confidence product: empty scope scores 0, not n/a.

Law 5: visibility × answer × resolution. No parsed functions is 0
on every factor, never None / skipped / CLEAN. apply() writes the
honesty note once.

python -m unittest tests.test_confidence
"""

from __future__ import annotations

import unittest

from prism import laws
from prism.confidence import apply, score
from prism.models import Finding, FunctionInfo, RunReport, StageResult


def _fn(name: str, kind: str = "SCALAR", file: str = "a.c") -> FunctionInfo:
    return FunctionInfo(
        file=file, name=name, kind=kind, line=1, signature=f"int {name}()",
    )


def _finding(
    status: str,
    function: str | None = "add",
    file: str = "a.c",
    cex: str = "",
    extra: dict | None = None,
) -> Finding:
    return Finding(
        stage="bmc", status=status, file=file, function=function,
        line=1, cls="INT-SIGNED-OVF", message=status,
        strength=laws.STRENGTH_PROVES, counterexample=cex,
        extra=extra or {},
    )


def _report(*fns: FunctionInfo, findings: list[Finding] | None = None) -> RunReport:
    rec = RunReport(root="x")
    rec.functions = list(fns)
    if findings is not None:
        rec.stages.append(StageResult(
            name="bmc", status="ok", findings=list(findings),
            records=len(findings),
        ))
    return rec


class TestConfidenceEmptyScope(unittest.TestCase):
    def test_empty_functions_product_is_zero_not_na(self):
        vis, ans, res, conf = score(RunReport(root="empty"))
        self.assertEqual((vis, ans, res, conf), (0.0, 0.0, 0.0, 0.0))
        self.assertIsInstance(conf, float)
        self.assertEqual(vis * ans * res, 0.0)
        self.assertNotEqual(conf, None)

    def test_empty_scope_ignores_stray_bmc_rows(self):
        rec = _report(findings=[_finding(laws.PROVED)])
        self.assertEqual(rec.functions, [])
        vis, ans, res, conf = score(rec)
        self.assertEqual((vis, ans, res, conf), (0.0, 0.0, 0.0, 0.0))

    def test_apply_zeros_stale_fields_and_notes_once(self):
        rec = RunReport(root="empty", visibility=1.0, answer=1.0,
                        resolution=1.0, confidence=1.0)
        apply(rec)
        self.assertEqual(
            (rec.visibility, rec.answer, rec.resolution, rec.confidence),
            (0.0, 0.0, 0.0, 0.0),
        )
        self.assertEqual(
            rec.notes,
            ["confidence 0: no functions parsed (no data, not clean)"],
        )
        apply(rec)
        self.assertEqual(rec.notes.count(
            "confidence 0: no functions parsed (no data, not clean)",
        ), 1)
        self.assertNotIn("n/a", " ".join(rec.notes).lower())
        self.assertNotIn(laws.CLEAN, " ".join(rec.notes))

    def test_no_bmc_stage_product_is_zero(self):
        rec = _report(_fn("add"))
        vis, ans, res, conf = score(rec)
        self.assertEqual(vis, 1.0)
        self.assertEqual(ans, 0.0)
        self.assertEqual(res, 0.0)
        self.assertEqual(conf, 0.0)
        apply(rec)
        self.assertEqual(rec.confidence, 0.0)

    def test_all_needs_harness_is_empty_answer_scope_zero(self):
        rec = _report(
            _fn("walk", kind="POINTER"),
            findings=[_finding(laws.NEEDS_HARNESS, function="walk")],
        )
        vis, ans, res, conf = score(rec)
        self.assertEqual(vis, 1.0)
        self.assertEqual((ans, res, conf), (0.0, 0.0, 0.0))

    def test_none_function_key_matches_named_scalar(self):
        rec = _report(
            _fn("add"),
            findings=[_finding(laws.PROVED, function=None)],
        )
        vis, ans, res, conf = score(rec)
        self.assertEqual((vis, ans, res, conf), (1.0, 0.0, 0.0, 0.0))

        rec2 = _report(
            _fn("add"),
            findings=[_finding(laws.PROVED, function="add")],
        )
        vis, ans, res, conf = score(rec2)
        self.assertEqual((vis, ans, res, conf), (1.0, 1.0, 1.0, 1.0))
        apply(rec2)
        self.assertEqual(rec2.confidence, 1.0)
        self.assertFalse(rec2.notes)

    def test_failed_with_cex_resolves_failed_without_does_not(self):
        with_cex = _report(
            _fn("add"),
            findings=[_finding(laws.FAILED, cex="x=1")],
        )
        vis, ans, res, conf = score(with_cex)
        self.assertEqual((vis, ans, res, conf), (1.0, 1.0, 1.0, 1.0))

        no_cex = _report(
            _fn("add"),
            findings=[_finding(laws.FAILED)],
        )
        vis, ans, res, conf = score(no_cex)
        self.assertEqual(ans, 1.0)
        self.assertEqual(res, 0.0)
        self.assertEqual(conf, 0.0)

    def test_empty_scope_md_and_notes_never_say_na(self):
        from prism.pipeline import _write_md
        import tempfile
        from pathlib import Path

        rec = apply(RunReport(root="empty"))
        self.assertEqual(rec.confidence, 0.0)
        blob = " ".join(rec.notes).lower()
        self.assertNotIn("n/a", blob)
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "report.md"
            _write_md(rec, p)
            text = p.read_text(encoding="utf-8").lower()
        self.assertIn("**0.0**", text)
        self.assertNotIn("n/a", text)


if __name__ == "__main__":
    unittest.main()
