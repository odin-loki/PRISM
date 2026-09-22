"""SARIF export + --fail-on. python -m unittest tests.test_sarif

Only defects are SARIF results; LLM output is a note; a stage that could not
run is a tool execution notification (never silently absent). When a C++
`prism` binary is available (PRISM_BIN, build/, build_wsl/), both engines
must emit the same SARIF for the same tree.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prism import __version__, laws
from prism.models import Finding, RunReport, StageResult
from prism.sarif import exit_code, to_sarif

ROOT = Path(__file__).resolve().parents[1]


def _f(status: str, **kw) -> Finding:
    base = dict(stage="lints", status=status, file="a.c", function="f", line=3,
                cls="MEM-UAF", message="use after free", strength=laws.STRENGTH_FINDS)
    base.update(kw)
    return Finding(**base)


def _report(*stages: StageResult) -> RunReport:
    r = RunReport(root=str(ROOT / "testdata"))
    r.stages = list(stages)
    return r


class TestSarifShape(unittest.TestCase):
    def test_defects_are_results_everything_else_is_not(self):
        rep = _report(StageResult("lints", "ok", findings=[
            _f(laws.FAILED),
            _f(laws.CRASH, stage="fuzz", cls="", counterexample="n=0"),
            _f(laws.CLEAN, cls=""),
            _f(laws.PROVED, stage="bmc", cls=""),
            _f(laws.BOUNDED, stage="bmc", cls=""),
            _f(laws.HYPOTHESIS, stage="llm", cls="INTENT", strength=laws.STRENGTH_READS),
        ]))
        doc = to_sarif(rep)
        self.assertEqual(doc["version"], "2.1.0")
        run = doc["runs"][0]
        self.assertEqual(run["tool"]["driver"]["name"], "PRISM")
        self.assertEqual(run["tool"]["driver"]["version"], __version__)
        res = run["results"]
        self.assertEqual([r["properties"]["status"] for r in res],
                         [laws.FAILED, laws.CRASH, laws.HYPOTHESIS])
        self.assertEqual([r["level"] for r in res], ["error", "error", "note"])
        self.assertEqual(res[1]["ruleId"], "fuzz/CRASH")
        self.assertIn("counterexample: n=0", res[1]["message"]["text"])
        self.assertTrue(res[2]["properties"]["hypothesis"])
        loc = res[0]["locations"][0]["physicalLocation"]
        self.assertEqual(loc["artifactLocation"], {"uri": "a.c", "uriBaseId": "SRCROOT"})
        self.assertEqual(loc["region"], {"startLine": 3})
        self.assertTrue(run["originalUriBaseIds"]["SRCROOT"]["uri"].startswith("file://"))
        self.assertEqual(sorted(r["id"] for r in run["tool"]["driver"]["rules"]),
                         ["INTENT", "MEM-UAF", "fuzz/CRASH"])

    def test_warning_severity_and_some_strength_are_warnings(self):
        rep = _report(StageResult("polyglot", "ok", findings=[
            _f(laws.FAILED, extra={"severity": "warning"}),
            _f(laws.FAILED, strength=laws.STRENGTH_SOME),
        ]))
        self.assertEqual([r["level"] for r in to_sarif(rep)["runs"][0]["results"]],
                         ["warning", "warning"])

    def test_notrun_and_failed_stages_are_notifications(self):
        rep = _report(
            StageResult("esbmc", "NOTRUN", detail="esbmc not found", install="apt install esbmc"),
            StageResult("bmc", "failed", detail="boom"),
            StageResult("lints", "ok"),
        )
        inv = to_sarif(rep)["runs"][0]["invocations"][0]
        self.assertFalse(inv["executionSuccessful"])
        notes = inv["toolExecutionNotifications"]
        self.assertEqual([n["level"] for n in notes], ["warning", "error"])
        self.assertEqual(notes[0]["message"]["text"], "esbmc NOTRUN: esbmc not found")


class TestExitCode(unittest.TestCase):
    def test_matrix(self):
        clean = _report(StageResult("lints", "ok", findings=[_f(laws.CLEAN)]))
        defect = _report(StageResult("lints", "ok", findings=[_f(laws.FAILED)]))
        gap = _report(StageResult("esbmc", "NOTRUN", findings=[_f(laws.NOTRUN)]))
        crashed = _report(StageResult("bmc", "failed"))
        for rep, never, dfct, gp in (
            (clean, 0, 0, 0), (defect, 0, 1, 1), (gap, 0, 0, 1), (crashed, 2, 2, 2),
        ):
            self.assertEqual(exit_code(rep, "never"), never)
            self.assertEqual(exit_code(rep, "defect"), dfct)
            self.assertEqual(exit_code(rep, "gap"), gp)

    def test_cli_fail_on(self):
        with tempfile.TemporaryDirectory(prefix="prism_sarif_") as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "a.c").write_text("<<<<<<< HEAD\nint x;\n>>>>>>> b\n", encoding="utf-8")
            out = Path(td) / "out"
            base = [sys.executable, "-m", "prism", str(src), "--no-llm",
                    "--stage", "polyglot", "--out", str(out)]
            r0 = subprocess.run(base, cwd=ROOT, capture_output=True, text=True, timeout=300)
            r1 = subprocess.run([*base, "--fail-on", "defect"], cwd=ROOT,
                                capture_output=True, text=True, timeout=300)
            self.assertEqual(r0.returncode, 0, r0.stdout + r0.stderr)
            self.assertEqual(r1.returncode, 1, r1.stdout + r1.stderr)
            doc = json.loads((out / "report.sarif").read_text(encoding="utf-8"))
            ids = {r["ruleId"] for r in doc["runs"][0]["results"]}
            self.assertIn("VCS-CONFLICT-MARKER", ids)


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    exe = "prism.exe" if os.name == "nt" else "prism"
    cands += [ROOT / d / exe for d in ("build", "build_wsl", "build/Release")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestEngineParity(unittest.TestCase):
    """Same tree, both engines, same SARIF results and notifications."""

    def _norm(self, doc: dict) -> dict:
        run = doc["runs"][0]
        res = sorted(
            (r["ruleId"], r["level"], r["message"]["text"],
             r.get("locations", [{}])[0].get("physicalLocation", {})
              .get("artifactLocation", {}).get("uri", ""),
             r.get("locations", [{}])[0].get("physicalLocation", {})
              .get("region", {}).get("startLine", 0))
            for r in run["results"]
        )
        notes = sorted(n["message"]["text"] for n in
                       run["invocations"][0]["toolExecutionNotifications"])
        return {"results": res, "notes": notes,
                "rules": [r["id"] for r in run["tool"]["driver"]["rules"]],
                "base": run.get("originalUriBaseIds"),
                "version": run["tool"]["driver"]["version"]}

    def test_polyglot_sarif_matches(self):
        tree = {
            "bad.py": "def f(:\n    pass\n",
            "bad.json": '{"a": 1,}\n',
            "a.c": "<<<<<<< HEAD\nint x;\n>>>>>>> b\n",
            "k.ini": "aws = AKIAABCDEFGHIJKLMNOP\n",  # prism:allow
            "s.sh": "if then\nfi\n",
        }
        with tempfile.TemporaryDirectory(prefix="prism parity ") as td:
            src = Path(td) / "src dir"
            src.mkdir()
            for rel, text in tree.items():
                (src / rel).write_text(text, encoding="utf-8")
            outs = {}
            for name, cmd in (
                ("py", [sys.executable, "-m", "prism"]),
                ("cpp", [str(_cpp_prism())]),
            ):
                out = Path(td) / f"out_{name}"
                subprocess.run([*cmd, str(src), "--no-llm", "--stage", "polyglot",
                                "--out", str(out)], cwd=ROOT, capture_output=True,
                               text=True, timeout=600)
                outs[name] = self._norm(json.loads((out / "report.sarif").read_text("utf-8")))
            self.assertEqual(outs["py"], outs["cpp"])
            self.assertTrue(outs["py"]["results"])


if __name__ == "__main__":
    unittest.main()
