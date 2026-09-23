"""prism/laws.py equals the Lean model of the verdict lattice.

tests/data/verdict_tables.json is printed by `lake exe verdict_tables`
from proofs/Prism/Verdict.lean, where the laws are proved. The domain is
finite, so every function is compared entry by entry over all of it (the
C++ module is checked against the same file in tests/cpp/test_main.cpp).
docs/VERDICTS.md, "Connecting the proof to the code".
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

from prism import laws
from prism.models import Finding, RunReport, StageResult
from prism.pipeline import STAGE_ORDER
from prism.sarif import to_sarif

ROOT = Path(__file__).resolve().parent.parent
TABLES = json.loads((ROOT / "tests" / "data" / "verdict_tables.json").read_text(encoding="utf-8"))


def _frac(f: list[int]) -> float:
    return f[0] / f[1] if f[1] else 0.0


class TestLeanTables(unittest.TestCase):
    def test_vocabulary_and_predicates(self):
        rows = TABLES["verdicts"]
        self.assertEqual([r["name"] for r in rows], list(laws.VERDICTS))
        for r in rows:
            s = r["name"]
            self.assertEqual(laws.is_proof(s), r["is_proof"], s)
            self.assertEqual(laws.is_formal(s), r["is_formal"], s)
            self.assertEqual(s in laws.ANSWERED, r["answered"], s)
            self.assertEqual(s in laws.NO_ANSWER, r["no_answer"], s)
            self.assertEqual(s in laws.DEFECTS, r["defect"], s)
            self.assertEqual(s in laws.MODEL, r["model"], s)
            self.assertEqual(laws.rank(s), r["rank"], s)

    def test_python_and_cpp_vocabulary_identical(self):
        hpp = (ROOT / "include" / "prism" / "laws.hpp").read_text(encoding="utf-8")
        consts = dict(re.findall(
            r'inline constexpr std::string_view (\w+) = "([^"]+)";', hpp))
        statuses = {v for k, v in consts.items()
                    if not k.startswith(("STRENGTH_", "CERTIFICATE_"))}
        self.assertEqual(statuses, set(laws.VERDICTS))
        for k, v in consts.items():
            self.assertEqual(getattr(laws, k), v, k)

    def test_origins_and_stages(self):
        self.assertEqual([o["name"] for o in TABLES["origins"]], list(laws.ORIGINS))
        for o in TABLES["origins"]:
            self.assertEqual(o["name"] in laws.PROVING_ORIGINS, o["may_prove"])
        self.assertEqual([s["name"] for s in TABLES["stages"]], [*STAGE_ORDER, "other"])
        for s in TABLES["stages"]:
            self.assertEqual(laws.stage_origin(s["name"]), s["origin"], s["name"])
        self.assertEqual(set(laws.STAGE_ORIGIN), {s["name"] for s in TABLES["stages"]})

    def test_merge_every_pair(self):
        self.assertEqual(len(TABLES["merge"]), 18 * 18)
        for r in TABLES["merge"]:
            self.assertEqual(laws.merge_refusal(r["a"], r["b"]), r["refusal"], r)
            if r["refusal"] == "none":
                laws.refuse_merge(r["a"], r["b"])
            else:
                with self.assertRaises(ValueError):
                    laws.refuse_merge(r["a"], r["b"])

    def test_rewrite_every_pair(self):
        self.assertEqual(len(TABLES["rewrite"]), 18 * 18)
        for r in TABLES["rewrite"]:
            self.assertEqual(laws.may_rewrite(r["from"], r["to"]), r["allowed"], r)

    def test_admit_whole_domain(self):
        self.assertEqual(len(TABLES["admit"]), 7 * 18 * 2)
        for r in TABLES["admit"]:
            self.assertEqual(laws.admit(r["origin"], r["status"], r["certificate"]), r["result"], r)

    def test_audit_whole_domain(self):
        self.assertEqual(len(TABLES["audit"]), 29 * 18 * 2)
        for r in TABLES["audit"]:
            got = laws.audit(r["stage"], r["status"], r["certificate"])
            self.assertEqual(got, (r["result"], r["violation"]), r)

    def test_confidence_grid(self):
        self.assertGreater(len(TABLES["confidence"]), 50)
        for r in TABLES["confidence"]:
            got = laws.score_counts(
                r["n_fun"], r["classified"], r["attempted"], r["answered"], r["resolved"])
            for g, key in zip(got, ("vis", "ans", "res", "conf")):
                self.assertAlmostEqual(g, _frac(r[key]), places=12, msg=(key, r))
        self.assertEqual(laws.confidence(0.0, float("inf"), 1.0), 0.0)


def _f(stage: str, status: str, fn: str, **extra: str) -> Finding:
    return Finding(stage=stage, status=status, file="a.c", function=fn, line=1, cls="",
                   message="m", strength=laws.STRENGTH_PROVES, extra=dict(extra))


class TestAudit(unittest.TestCase):
    def test_non_proving_stage_is_demoted_and_flagged(self):
        rep = RunReport(root=".")
        rep.stages = [
            StageResult(name="fuzz", status="ok", findings=[
                _f("fuzz", laws.PROVED, "f"), _f("fuzz", laws.CLEAN, "g")]),
            StageResult(name="bmc", status="ok", findings=[
                _f("bmc", laws.PROVED_CERTIFIED, "h"),
                _f("bmc", laws.PROVED_CERTIFIED, "k",
                   **{laws.CERTIFICATE_KEY: laws.CERTIFICATE_CHECKED}),
                _f("bmc", laws.BOUNDED, "m")]),
        ]
        self.assertEqual(laws.audit_report(rep), 2)
        fz = rep.stages[0].findings
        self.assertEqual([f.status for f in fz], [laws.UNKNOWN, laws.CLEAN, laws.ERROR])
        self.assertEqual(fz[0].extra["audit_original"], laws.PROVED)
        self.assertEqual(fz[2].message, "verdict audit: fuzz may not emit PROVED")
        bm = rep.stages[1].findings
        self.assertEqual([f.status for f in bm],
                         [laws.PROVED, laws.PROVED_CERTIFIED, laws.BOUNDED, laws.ERROR])
        self.assertEqual(
            bm[3].message,
            "verdict audit: bmc may not emit PROVED-CERTIFIED without a checked certificate")
        self.assertEqual(laws.audit_report(rep), 0)  # idempotent

    def test_audit_logic_matches_cpp_source(self):
        # Same message and extra keys as src/prism/laws.cpp audit_report.
        src = (ROOT / "src" / "prism" / "laws.cpp").read_text(encoding="utf-8")
        self.assertIn('"verdict audit: " + s.name + " may not emit "', src)
        self.assertIn('" without a checked certificate"', src)
        self.assertIn('e.extra["audit"] = "verdict"', src)
        self.assertIn('f.extra["audit_original"]', src)
        for py in ("prism/pipeline.py",):
            self.assertEqual((ROOT / py).read_text(encoding="utf-8").count("laws.audit_report("), 2)
        self.assertEqual(
            (ROOT / "src/prism/pipeline.cpp").read_text(encoding="utf-8").count("laws::audit_report("), 2)

    def test_certified_is_not_a_sarif_result(self):
        rep = RunReport(root=".")
        rep.stages = [StageResult(name="bmc", status="ok", findings=[
            _f("bmc", laws.PROVED_CERTIFIED, "h",
               **{laws.CERTIFICATE_KEY: laws.CERTIFICATE_CHECKED})])]
        run = to_sarif(rep)["runs"][0]
        self.assertEqual(run["results"], [])
        self.assertEqual(run["properties"]["certified"], 1)

    def test_new_merge_refusals(self):
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.NOTRUN, laws.CLEAN)
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.HYPOTHESIS, laws.PROVED)
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.PROVED_CERTIFIED, laws.PROVED)
        laws.refuse_merge(laws.NOTRUN, laws.ERROR)


class TestProofsProject(unittest.TestCase):
    def test_lean_sources_have_no_sorry(self):
        for p in (ROOT / "proofs").rglob("*.lean"):
            if ".lake" in p.parts:
                continue
            text = re.sub(r"--.*|/-.*?-/", "", p.read_text(encoding="utf-8"), flags=re.S)
            self.assertNotRegex(text, r"\bsorry\b|native_decide", str(p))

    def test_toolchain_pinned(self):
        tc = (ROOT / "proofs" / "lean-toolchain").read_text(encoding="utf-8").strip()
        self.assertRegex(tc, r"^leanprover/lean4:v4\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
