"""LLM auditor honesty: hypothesize / LlamaEngine missing GGUF.

Helix is law. The LLM is HYPOTHESIS/READS. Missing engine (including a
missing GGUF with no llama-server/Ollama) is NOTRUN, never CLEAN, never
COVERED, never PROVED. Empty hypotheses stay HYPOTHESIS. complete() error
is ERROR. No live LLM required.

python -m unittest tests.test_llm
"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.agent import dafny_specs, hypothesize
from helix.ai import LLM_INSTALL, LLM_UNAVAILABLE_MSG, LlamaEngine
from helix.config import Config
from helix.models import Finding, FunctionInfo, RunReport, StageResult
from helix.taxonomy import coverage_from_report


def _fn() -> FunctionInfo:
    return FunctionInfo(
        file="t.c", name="add", kind="SCALAR", line=1,
        signature="int add(int x)", body="return x + 1;",
    )


def _engine(available: bool = True, text: str = "", error=None, backend: str = "mock"):
    engine = mock.Mock()
    engine.available.return_value = available
    engine.complete.return_value = mock.Mock(error=error, text=text, backend=backend)
    return engine


def _never_truth(finding) -> None:
    """LLM silence/hypothesis is not a proof and does not cover a class."""
    assert finding.status != laws.CLEAN
    assert finding.status != laws.PROVED
    assert finding.status != "COVERED"
    assert not laws.is_proof(finding.status)
    assert finding.status not in {
        laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING,
    }


class TestHypothesizeUnavailable(unittest.TestCase):
    def test_unavailable_is_notrun_reads_with_install(self):
        engine = _engine(available=False)
        out = hypothesize(engine, [_fn()], budget=4)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.stage, "llm")
        self.assertEqual(rec.message, LLM_UNAVAILABLE_MSG)
        self.assertEqual(rec.extra.get("install"), LLM_INSTALL)
        self.assertNotEqual(rec.status, laws.CLEAN)
        _never_truth(rec)
        engine.complete.assert_not_called()


class TestHypothesizeEmptyJson(unittest.TestCase):
    def test_empty_hypotheses_is_hypothesis_not_proved(self):
        engine = _engine(text='{"hypotheses":[]}')
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.HYPOTHESIS)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.stage, "llm")
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertNotEqual(rec.status, laws.NOTRUN)
        _never_truth(rec)
        engine.complete.assert_called_once()


class TestHypothesizeError(unittest.TestCase):
    def test_complete_error_is_error_not_proved(self):
        engine = _engine(error="timed out", text="")
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.ERROR)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.message, "timed out")
        self.assertEqual(rec.function, "add")
        self.assertNotEqual(rec.status, laws.PROVED)
        _never_truth(rec)
        engine.complete.assert_called_once()

    def test_http_error_is_notrun_never_code_error(self):
        engine = _engine(error="HTTP error", text="")
        out = hypothesize(engine, [_fn()], budget=1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual((rec.extra or {}).get("install"), LLM_INSTALL)
        _never_truth(rec)


class TestDafnySpecsHttpIsNotrun(unittest.TestCase):
    def test_http_error_is_notrun_never_code_error(self):
        engine = _engine(error="HTTP error", text="")
        out = dafny_specs(engine, [_fn()], budget=1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotEqual(rec.status, laws.HYPOTHESIS)
        self.assertEqual(rec.stage, "contracts")
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual((rec.extra or {}).get("install"), LLM_INSTALL)
        _never_truth(rec)

    def test_timed_out_stays_error_not_a_proof(self):
        engine = _engine(error="timed out", text="")
        out = dafny_specs(engine, [_fn()], budget=1)
        rec = out[0]
        self.assertEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.NOTRUN)
        _never_truth(rec)


class TestDafnySpecsUnavailable(unittest.TestCase):
    def test_missing_engine_is_notrun(self):
        engine = _engine(available=False)
        out = dafny_specs(engine, [_fn()], budget=3)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertEqual(rec.stage, "contracts")
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.message, LLM_UNAVAILABLE_MSG)
        self.assertNotEqual(rec.status, laws.CLEAN)
        _never_truth(rec)
        engine.complete.assert_not_called()


class TestLlamaEngineMissingGguf(unittest.TestCase):
    def test_missing_gguf_unavailable_hypothesize_notrun(self):
        cfg = Config(
            gguf=Path("/nonexistent/helix-missing.gguf"),
            ollama_host="http://127.0.0.1:1",
            llama_server="http://127.0.0.1:1",
        )
        with mock.patch.object(LlamaEngine, "_ok", return_value=False):
            engine = LlamaEngine(cfg)
        self.assertFalse(engine.available())
        self.assertEqual(engine.backend, "none")
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.extra.get("install"), LLM_INSTALL)
        self.assertNotEqual(rec.status, laws.CLEAN)
        _never_truth(rec)


def _llm_report(findings, status="ok") -> RunReport:
    rec = RunReport(root="x")
    rec.stages.append(StageResult(
        name="llm", status=status, findings=list(findings), records=len(findings),
    ))
    return rec


def _covered_ids(findings, status="ok") -> list[str]:
    return [r["id"] for r in coverage_from_report(_llm_report(findings, status))
            if r["verdict"] == "COVERED"]


class TestHypothesizeCannotCover(unittest.TestCase):
    def test_named_class_is_reads_not_covered(self):
        engine = _engine(text='{"hypotheses":[{"function":"add","line":1,'
                          '"cls":"INT-SIGNED-OVF","why":"maybe"}]}')
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.HYPOTHESIS)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")
        _never_truth(rec)
        rows = {r["id"]: r for r in coverage_from_report(_llm_report(out))}
        self.assertEqual(rows["INT-SIGNED-OVF"]["verdict"], "PARTIAL")
        self.assertEqual(rows["INT-SIGNED-OVF"]["best"], laws.STRENGTH_READS)
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")
        self.assertEqual(_covered_ids(out), [])

    def test_empty_hypotheses_cover_nothing(self):
        engine = _engine(text='{"hypotheses":[]}')
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(out[0].status, laws.HYPOTHESIS)
        self.assertEqual(out[0].strength, laws.STRENGTH_READS)
        _never_truth(out[0])
        self.assertEqual(_covered_ids(out), [])

    def test_empty_text_silence_is_hypothesis_not_covered(self):
        engine = _engine(text="")
        out = hypothesize(engine, [_fn()], budget=1)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.HYPOTHESIS)
        self.assertEqual(rec.strength, laws.STRENGTH_READS)
        self.assertNotEqual(rec.status, laws.CLEAN)
        _never_truth(rec)
        self.assertEqual(_covered_ids(out), [])

    def test_unavailable_covers_nothing(self):
        out = hypothesize(_engine(available=False), [_fn()], budget=1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertEqual(_covered_ids(out, status="NOTRUN"), [])

    def test_llm_stage_cannot_cover_even_if_finding_lies(self):
        f = Finding(
            stage="llm", status=laws.FAILED, file="t.c", function="add",
            line=1, cls="INT-SIGNED-OVF", message="spoof",
            strength=laws.STRENGTH_FINDS,
        )
        rows = {r["id"]: r for r in coverage_from_report(_llm_report([f]))}
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")
        self.assertEqual(rows["INT-SIGNED-OVF"]["best"], laws.STRENGTH_READS)
        self.assertEqual(_covered_ids([f]), [])


class TestLlmForcedReads(unittest.TestCase):
    def test_proved_failed_clean_become_hypothesis_reads(self):
        from helix.pipeline import llm_forced_reads

        for status, strength in (
            (laws.PROVED, laws.STRENGTH_PROVES),
            (laws.PROVED_UNBOUNDED, laws.STRENGTH_PROVES),
            (laws.FAILED, laws.STRENGTH_FINDS),
            (laws.CLEAN, laws.STRENGTH_FINDS),
            (laws.BOUNDED, laws.STRENGTH_PROVES),
            (laws.CRASH, laws.STRENGTH_FINDS),
        ):
            f = Finding(
                stage="bmc", status=status, file="t.c", function="add",
                line=1, cls="INT-SIGNED-OVF", message="lie",
                strength=strength,
            )
            out = llm_forced_reads([f])
            self.assertEqual(out[0].stage, "llm")
            self.assertEqual(out[0].strength, laws.STRENGTH_READS)
            self.assertEqual(out[0].status, laws.HYPOTHESIS)
            self.assertFalse(laws.is_proof(out[0].status))
            self.assertEqual(_covered_ids(out), [])

    def test_notrun_error_timeout_kept(self):
        from helix.pipeline import llm_forced_reads

        for status in (laws.NOTRUN, laws.ERROR, laws.TIMEOUT, laws.HYPOTHESIS, laws.READS):
            f = Finding(
                stage="x", status=status, file="", function=None,
                line=None, cls="INTENT", message=status,
                strength=laws.STRENGTH_FINDS,
            )
            out = llm_forced_reads([f])
            self.assertEqual(out[0].status, status)
            self.assertEqual(out[0].strength, laws.STRENGTH_READS)
            self.assertEqual(out[0].stage, "llm")


if __name__ == "__main__":
    unittest.main()
