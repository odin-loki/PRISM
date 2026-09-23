"""Certified mode (roadmap 3.2), the shipped trusted base (3.2 / 8.3) and
verdict links in report.md (6.4), for both engines.

The pir stage and its certificates are C++ only (roadmap D8); the Python
engine accepts --certified / --solver-cache for CLI parity and records them
in its pir NOTRUN row. The end-to-end cases need the C++ binary (PRISM_BIN or
build/prism); certificate checks also need CaDiCaL and cake_lpr
(scripts/fetch_deps.py) and otherwise assert the honest fallback.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prism import laws, shipdocs
from prism.config import Config
from prism.models import Finding, FunctionInfo, RunReport, StageResult
from prism.pipeline import _write_md, run_pir_notrun
from prism.sarif import to_sarif

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"

SAFE_C = """\
int div_ok(int a, int b) {
    int d = (b & 7) + 1;
    return a / d;
}

int add_bad(int a, int b) {
    return a + b;
}

int sum3(int x) {
    int acc = 0;
    for (int i = 0; i < 3; ++i) acc += x & 15;
    return acc;
}
"""


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    cands += [ROOT / d / "prism" for d in ("build", "build_wsl")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c.resolve()
    return None


def _tool(name: str) -> bool:
    home = Path(os.environ.get("PRISM_TOOLS_DIR", Path.home() / ".prism" / "tools")) / name
    if home.is_dir() and any(home.glob(f"*/bin/{name}")):
        return True
    return shutil.which(name) is not None


def _anchors() -> set[str]:
    return set(re.findall(r'<a id="(verdict-[a-z-]+)"></a>', (DOCS / "VERDICTS.md").read_text(encoding="utf-8")))


class TestVerdictAnchors(unittest.TestCase):
    def test_every_verdict_has_an_anchor(self) -> None:
        anchors = _anchors()
        for v in laws.VERDICTS:
            self.assertIn(shipdocs.verdict_anchor(v), anchors, v)
        self.assertEqual(shipdocs.verdict_anchor("ok"), "")

    def test_doc_links_to_verdicts_resolve(self) -> None:
        text = (DOCS / "VERDICTS.md").read_text(encoding="utf-8")
        slugs = {re.sub(r"[^a-z0-9 -]", "", h.strip().lower()).replace(" ", "-")
                 for h in re.findall(r"(?m)^#+ (.+)$", text)}
        ok = _anchors() | slugs
        for md in sorted(list(DOCS.glob("*.md")) + [ROOT / "README.md"]):
            if not md.exists():
                continue
            for a in re.findall(r"VERDICTS\.md#([A-Za-z0-9-]+)", md.read_text(encoding="utf-8")):
                if a in ("name", "anchor"):
                    continue
                self.assertIn(a, ok, f"{md.name}: VERDICTS.md#{a}")

    def test_cpp_uses_the_same_anchor_rule(self) -> None:
        src = (ROOT / "src" / "prism" / "shipdocs.cpp").read_text(encoding="utf-8")
        self.assertIn('std::string a = "verdict-";', src)
        self.assertIn("std::tolower", src)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("foreach(_doc TRUSTED_BASE VERDICTS)", cmake)


class TestPythonReportsShipDocs(unittest.TestCase):
    def _report(self, root: Path) -> RunReport:
        r = RunReport(root=str(root))
        s = StageResult(name="bmc", status="ok")
        s.findings = [
            Finding(stage="bmc", status=laws.FAILED, file="a.c", function="f", line=3, cls="INT-SIGNED-OVF",
                    message="ovf", strength=laws.STRENGTH_PROVES, counterexample="a=1"),
            Finding(stage="bmc", status=laws.PROVED, file="a.c", function="g", line=9, cls="",
                    message="ok", strength=laws.STRENGTH_PROVES),
        ]
        s.records = len(s.findings)
        r.stages = [s]
        r.functions = [FunctionInfo(file="a.c", name="f", kind="SCALAR", line=3, signature="int f(int)")]
        return r

    def test_report_md_links_and_shipped_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            _write_md(self._report(out), out / "report.md")
            md = (out / "report.md").read_text(encoding="utf-8")
            self.assertIn("[TRUSTED_BASE.md](TRUSTED_BASE.md)", md)
            self.assertEqual((out / "TRUSTED_BASE.md").read_bytes(), (DOCS / "TRUSTED_BASE.md").read_bytes())
            self.assertEqual((out / "VERDICTS.md").read_bytes(), (DOCS / "VERDICTS.md").read_bytes())
            anchors = _anchors()
            findings = [ln for ln in md.splitlines() if ln.startswith("- `")]
            self.assertEqual(len(findings), 2)
            for ln in findings:
                m = re.search(r"\(\[([A-Z-]+)\]\(VERDICTS\.md#(verdict-[a-z-]+)\)\)$", ln)
                self.assertIsNotNone(m, ln)
                assert m is not None
                self.assertIn(m.group(2), anchors)
                self.assertTrue(ln.startswith(f"- `{m.group(1)}`"), ln)
            self.assertIn("  cex `a=1`  ([FAILED](VERDICTS.md#verdict-failed))", md)

    def test_sarif_names_the_trusted_base(self) -> None:
        doc = to_sarif(self._report(ROOT))
        tb = doc["runs"][0]["properties"]["trustedBase"]
        self.assertEqual(tb["file"], "TRUSTED_BASE.md")
        self.assertEqual(tb["sha256"], hashlib.sha256((DOCS / "TRUSTED_BASE.md").read_bytes()).hexdigest())

    def test_trusted_base_describes_the_wired_pir_path(self) -> None:
        text = (DOCS / "TRUSTED_BASE.md").read_text(encoding="utf-8")
        for s in ("--certified", "every** one of its VCs", "certify_note", "writes a copy of it next to",
                  "trustedBase", "never quietly upgraded"):
            self.assertIn(s, text)
        self.assertNotIn("until the integrator switches it", text)


class TestPythonCliParity(unittest.TestCase):
    def test_python_cli_accepts_and_records_the_flags(self) -> None:
        r = subprocess.run([sys.executable, "-m", "prism", "--help"], capture_output=True, text=True,
                           cwd=ROOT, timeout=120)
        self.assertIn("--certified", r.stdout)
        self.assertIn("--solver-cache", r.stdout)
        rows = run_pir_notrun(Config(certified=True, solver_cache="/tmp/x"))
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertEqual(rows[0].extra["certified_mode"], "on")
        self.assertIn("not certified", rows[0].extra["certify_note"])
        self.assertEqual(rows[0].extra["solver_cache"], "/tmp/x")
        self.assertNotIn("certified_mode", run_pir_notrun(Config())[0].extra)

    def test_cpp_cli_has_the_flags(self) -> None:
        main = (ROOT / "src" / "prism" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('a == "--certified") cfg.certified = true', main)
        self.assertIn('a == "--solver-cache"', main)
        self.assertIn("check_function(fn, check_options(cfg))",
                      (ROOT / "src" / "prism" / "pir" / "stage.cpp").read_text(encoding="utf-8"))


class TestTaxonomyAndConfidenceCreditPir(unittest.TestCase):
    def test_seen_tables_list_pir(self) -> None:
        from prism.taxonomy import CLASSES
        by = {c["id"]: c for c in CLASSES}
        for cid in ("INT-SIGNED-OVF", "INT-DIV-ZERO", "INT-SHIFT-UB", "UNINIT-READ", "INT-CLZ-ZERO",
                    "FUNC-CONTRACT"):
            self.assertEqual(by[cid]["seen"].get("pir"), laws.STRENGTH_PROVES, cid)
        cpp = (ROOT / "src" / "prism" / "taxonomy.cpp").read_text(encoding="utf-8")
        for cid in ("INT-SIGNED-OVF", "INT-DIV-ZERO", "INT-SHIFT-UB", "UNINIT-READ", "INT-CLZ-ZERO",
                    "FUNC-CONTRACT"):
            row = next(ln for ln in cpp.splitlines() if f'{{"{cid}",' in ln)
            self.assertIn('{"pir", "PROVES"}', row, cid)

    def test_pir_proof_covers_and_resolves(self) -> None:
        from prism import confidence
        from prism.taxonomy import coverage_from_report
        r = RunReport(root=".")
        r.functions = [FunctionInfo(file="a.c", name="f", kind="SCALAR", line=1, signature="int f(int)")]
        s = StageResult(name="pir", status="ok")
        s.findings = [Finding(stage="pir", status=laws.PROVED, file="a.c", function="f", line=1, cls="",
                              message="ok", strength=laws.STRENGTH_PROVES)]
        r.stages = [s]
        rows = {row["id"]: row["verdict"] for row in coverage_from_report(r)}
        self.assertEqual(rows["UNINIT-READ"], "COVERED")
        self.assertEqual(rows["INT-SIGNED-OVF"], "COVERED")
        self.assertNotEqual(rows["MEM-OOB-READ"], "COVERED")
        v, a, res, c = confidence.score(r)
        self.assertEqual((a, res), (1.0, 1.0))
        # bmc NEEDS-HARNESS + pir PROVED: answered (pir answers what bmc cannot)
        b = StageResult(name="bmc", status="ok")
        b.findings = [Finding(stage="bmc", status=laws.NEEDS_HARNESS, file="a.c", function="f", line=1,
                              cls="", message="", strength=laws.STRENGTH_PROVES)]
        r.stages = [b, s]
        self.assertEqual(confidence.score(r)[1], 1.0)


class TestConformanceCertifiedSummary(unittest.TestCase):
    def test_counts_loop_free_true_functions(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        import conformance

        def row(fn: str, expected: bool, status: str, loops: str | None, note: str = "") -> dict:
            extra = {"loops": loops} if loops is not None else {}
            if fn == "v":
                extra["certificate_vcs"] = "0"
            if fn == "a":
                extra["certificate_bitblast"] = "2/2 lean-proved"
            if fn == "c":
                extra["certificate_bitblast"] = "0/1 lean-proved"
            if note:
                extra["certify_note"] = note
            return {"task": "t", "function": fn, "stage": conformance.CERT_STAGE, "expected": expected,
                    "law_task": False, "findings": [{"status": status, "extra": extra}]}

        rows = [row("a", True, laws.PROVED_CERTIFIED, "0"), row("b", True, laws.PROVED, "0", "VC x: why"),
                # zero VCs: stays PROVED (a certificate that checks nothing is not one)
                row("v", True, laws.PROVED, "0", "no verification conditions (nothing to certify)"),
                row("c", True, laws.PROVED_CERTIFIED, "1"), row("d", True, laws.NEEDS_HARNESS, None),
                row("e", False, laws.FAILED, "0")]
        s = conformance.certified_summary(rows)
        self.assertEqual((s["loop_free_true"], s["loop_free_true_proved"], s["loop_free_true_certified"]), (3, 3, 1))
        self.assertEqual(s["loop_free_true_no_vcs"], 1)
        self.assertEqual((s["loop_free_true_certified_lean"], s["loop_free_true_certified_z3"],
                          s["loop_free_true_certified_mixed"]), (1, 0, 0))
        self.assertEqual((s["looped_true"], s["looped_true_certified"], s["not_encoded_true"]), (1, 1, 1))
        self.assertEqual(s["wrong_certified"], 0)
        self.assertEqual(s["proved_not_certified"], [{"task": "t", "function": "b", "note": "VC x: why"}])
        self.assertEqual(conformance.classify(
            conformance.Task(ident="t", yml=Path("t.yml"), source=Path("t.c"), origin="prism", category="c",
                             lang="C", prop="no-overflow", expected={"e": False}),
            "e", [{"status": laws.PROVED_CERTIFIED}]), "wrong-proof")


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestCppCertifiedEndToEnd(unittest.TestCase):
    def _run(self, td: Path, *extra: str) -> dict:
        exe = _cpp_prism()
        assert exe is not None
        src = td / "src"
        src.mkdir(exist_ok=True)
        (src / "cert.c").write_text(SAFE_C, encoding="utf-8")
        out = td / "out"
        r = subprocess.run([str(exe), str(src), "--no-llm", "--out", str(out), "--stage",
                            "inventory,classify,pir", "--solver-cache", str(td / "cache"), *extra],
                           capture_output=True, text=True, timeout=900)
        self.assertTrue((out / "report.json").exists(), r.stderr[-2000:])
        return json.loads((out / "report.json").read_text(encoding="utf-8"))

    @staticmethod
    def _pir(rep: dict) -> dict[str, dict]:
        st = next(s for s in rep["stages"] if s["name"] == "pir")
        return {f["function"]: f for f in st["findings"] if f.get("function")}

    def test_certified_run(self) -> None:
        if shutil.which("clang") is None or shutil.which("opt") is None:
            self.skipTest("clang/opt not on PATH")
        with tempfile.TemporaryDirectory() as tdn:
            td = Path(tdn)
            rep = self._run(td, "--certified")
            fns = self._pir(rep)
            self.assertEqual(fns["add_bad"]["status"], laws.FAILED)
            self.assertTrue(fns["add_bad"]["counterexample"])
            chain = _tool("cadical") and _tool("cake_lpr")
            for name in ("div_ok", "sum3"):
                f = fns[name]
                self.assertEqual(f["extra"]["certified_mode"], "on")
                if chain:
                    self.assertEqual(f["status"], laws.PROVED_CERTIFIED, f)
                    self.assertEqual(f["extra"]["certificate"], "checked")
                    self.assertIn("cake_lpr", f["extra"]["certificate_info"])
                    for h in f["extra"]["cnf_sha256"].split(","):
                        self.assertRegex(h, r"^[0-9a-f]{64}$")
                else:
                    self.assertEqual(f["status"], laws.PROVED)
                    self.assertIn("not certified", f["extra"]["certify_note"])
            # the verdict audit found nothing to demote
            audit = [f for s in rep["stages"] for f in s["findings"] if f.get("extra", {}).get("audit")]
            self.assertEqual(audit, [])
            out = td / "out"
            self.assertEqual((out / "TRUSTED_BASE.md").read_bytes(), (DOCS / "TRUSTED_BASE.md").read_bytes())
            self.assertEqual((out / "VERDICTS.md").read_bytes(), (DOCS / "VERDICTS.md").read_bytes())
            md = (out / "report.md").read_text(encoding="utf-8")
            if chain:
                self.assertIn("([PROVED-CERTIFIED](VERDICTS.md#verdict-proved-certified))", md)
            sarif = json.loads((out / "report.sarif").read_text(encoding="utf-8"))
            props = sarif["runs"][0]["properties"]
            self.assertEqual(props["trustedBase"]["sha256"],
                             hashlib.sha256((DOCS / "TRUSTED_BASE.md").read_bytes()).hexdigest())
            if chain:
                self.assertGreaterEqual(props["certified"], 2)
            self.assertTrue((td / "cache" / "solve_times.json").exists())

    def test_plain_run_never_certifies(self) -> None:
        if shutil.which("clang") is None or shutil.which("opt") is None:
            self.skipTest("clang/opt not on PATH")
        with tempfile.TemporaryDirectory() as tdn:
            fns = self._pir(self._run(Path(tdn)))
            self.assertEqual(fns["div_ok"]["status"], laws.PROVED)
            self.assertEqual(fns["div_ok"]["extra"]["certified_mode"], "off")
            self.assertNotIn("certificate", fns["div_ok"]["extra"])


if __name__ == "__main__":
    unittest.main()
