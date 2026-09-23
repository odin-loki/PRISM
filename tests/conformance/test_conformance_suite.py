"""Shape of the conformance suite and the rules of tools/conformance.py.

The engine itself is scored by `python tools/conformance.py` (and the
conformance workflow); these tests keep the suite and the scorer honest
without depending on the engine's current numbers.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import sys
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUITE = REPO / "tests" / "conformance"

_spec = importlib.util.spec_from_file_location("prism_conformance_t", REPO / "tools" / "conformance.py")
assert _spec and _spec.loader
conf = importlib.util.module_from_spec(_spec)
sys.modules["prism_conformance_t"] = conf
_spec.loader.exec_module(conf)

PROPERTIES = {"no-overflow", "no-div0", "no-shift-ub", "no-oob", "no-null-deref", "memsafety"}


def _tasks() -> list:
    return conf.discover([SUITE / "prism", SUITE / "sv-comp"])


class SuiteShape(unittest.TestCase):
    def test_in_house_tasks(self) -> None:
        tasks = [t for t in _tasks() if t.origin == "prism"]
        self.assertGreaterEqual(len(tasks), 120)
        stems = {t.source.stem for t in tasks}
        for t in tasks:
            self.assertTrue(t.source.exists(), t.ident)
            self.assertIn(t.prop, PROPERTIES, t.ident)
            self.assertTrue(t.expected, t.ident)
            for fn, exp in t.expected.items():
                self.assertIn(fn, t.source.read_text(), t.ident)
                if not exp and fn not in t.expect_status:
                    self.assertIn(fn, t.witness, f"{t.ident}: false function needs a witness")
            # every feature comes as a true/false pair
            stem = t.source.stem
            if stem.endswith("_true"):
                self.assertIn(stem[:-5] + "_false", stems, t.ident)
            elif stem.endswith("_false"):
                self.assertIn(stem[:-6] + "_true", stems, t.ident)
        n_true = sum(1 for t in tasks for v in t.expected.values() if v)
        n_false = sum(1 for t in tasks for v in t.expected.values() if not v)
        self.assertEqual(n_true, n_false)
        self.assertTrue(any(t.lang == "C++" for t in tasks))
        self.assertTrue(any(t.expect_status.get(fn) == "NEEDS-HARNESS" for t in tasks for fn in t.expected))

    def test_sv_comp_subset_is_pinned(self) -> None:
        tasks = [t for t in _tasks() if t.origin == "sv-comp"]
        self.assertGreaterEqual(len(tasks), 40)
        self.assertTrue(all(t.prop == "no-overflow" and list(t.expected) == ["main"] for t in tasks))
        self.assertTrue({True, False} <= {t.expected["main"] for t in tasks})
        sources = (SUITE / "SOURCES.md").read_text()
        self.assertIn("07b00127ac57f773f9de6eee95821b6947948dbe", sources)
        self.assertIn("Apache-2.0", sources)

    def test_juliet_is_pinned_not_vendored(self) -> None:
        sources = (SUITE / "SOURCES.md").read_text()
        self.assertIn(conf.JULIET_SHA256, sources)
        self.assertIn(str(conf.JULIET_SIZE), sources)
        self.assertFalse(list(SUITE.rglob("CWE*")), "Juliet stays out of git")


class ScorerRules(unittest.TestCase):
    def _task(self, expected: bool, origin: str = "prism", prop: str = "no-overflow",
              status: str | None = None) -> object:
        return conf.Task(ident="x.yml", yml=Path("x.yml"), source=Path("x.c"), origin=origin, category="c",
                         lang="C", prop=prop, expected={"f": expected},
                         expect_status={"f": status} if status else {})

    def test_classify(self) -> None:
        fail = [{"status": "FAILED", "cls": "INT-SIGNED-OVF"}]
        for proof in sorted(conf.PROOF):
            self.assertEqual(conf.classify(self._task(False), "f", [{"status": proof}]), "wrong-proof")
            self.assertEqual(conf.classify(self._task(True), "f", [{"status": proof}]), "proved")
        self.assertEqual(conf.classify(self._task(True), "f", fail), "false-alarm")
        self.assertEqual(conf.classify(self._task(False), "f", fail), "refuted")
        # BOUNDED is never a proof (Law 2) and never a refutation
        self.assertEqual(conf.classify(self._task(False), "f", [{"status": "BOUNDED"}]), "bounded")
        self.assertEqual(conf.classify(self._task(True), "f", [{"status": "BOUNDED"}]), "bounded")
        self.assertEqual(conf.classify(self._task(True), "f", []), "missing")
        # Law 6
        law = self._task(False, status="NEEDS-HARNESS")
        self.assertEqual(conf.classify(law, "f", [{"status": "NEEDS-HARNESS"}]), "law-ok")
        self.assertEqual(conf.classify(law, "f", fail), "law6-violation")
        self.assertEqual(conf.classify(law, "f", [{"status": "PROVED"}]), "wrong-proof")
        # property-scoped (SV-COMP) labels: another class is not a false alarm
        sv = self._task(True, origin="sv-comp")
        self.assertEqual(conf.classify(sv, "f", [{"status": "FAILED", "cls": "INT-SHIFT-UB"}]),
                         "failed-other-property")
        self.assertEqual(conf.classify(sv, "f", fail), "false-alarm")

    def test_parse_cex(self) -> None:
        self.assertEqual(conf.parse_cex("a=2147418111, b=-3"), {"a": 2147418111, "b": -3})
        self.assertEqual(conf.parse_cex("a=#x0000000f, b=#b101"), {"a": 15, "b": 5})
        self.assertEqual(conf.parse_cex("ovf+=sat"), {})

    def test_signature(self) -> None:
        text = "[[nodiscard]] static long long g(unsigned short a, const int& b) {\n return 0; }\n"
        ret, params = conf.find_signature(text, "g")
        self.assertEqual(params, [("unsigned short", "a"), ("const int&", "b")])
        self.assertEqual(conf.type_range("unsigned short"), (0, 65535))
        self.assertEqual(conf.type_range("const int&"), (-(2**31), 2**31 - 1))
        self.assertIsNone(conf.type_range("int *"))


FAKE = textwrap.dedent('''\
    #!{py}
    """Stand-in engine: claims {status} for every function it is asked about."""
    import json, re, sys
    from pathlib import Path
    if "--list-stages" in sys.argv:
        print("inventory\\nclassify\\nbmc\\nharness")
        sys.exit(0)
    src = Path(sys.argv[1]); out = Path(sys.argv[sys.argv.index("--out") + 1])
    out.mkdir(parents=True, exist_ok=True)
    fns = re.findall(r"(?m)^[\\w ]*?\\b(\\w+)\\([^)]*\\)\\s*\\{{", src.read_text())
    fs = [dict(stage="bmc", status="{status}", function=f, cls="INT-SIGNED-OVF", message="fake",
               counterexample="") for f in fns]
    rep = dict(stages=[dict(name="bmc", status="ok", findings=fs)])
    (out / "report.json").write_text(json.dumps(rep))
''')


class ReleaseGate(unittest.TestCase):
    def _run(self, status: str, tmp: Path) -> tuple[int, dict]:
        fake = tmp / f"fake_{status}.py"
        fake.write_text(FAKE.format(py=sys.executable, status=status))
        fake.chmod(0o755)
        out = tmp / f"out_{status}"
        rc = conf.main(["--prism", str(fake), "--out", str(out), "--filter", "prism/overflow/(add|neg)_",
                        "--no-replay", "-j", "2"])
        return rc, json.loads((out / "metrics.json").read_text())

    def test_gate(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            rc, m = self._run("PROVED", Path(d))
            self.assertEqual(rc, 1, "a wrong proof must fail the release gate")
            self.assertEqual(m["meta"]["wrong_proofs"], 2)
            self.assertEqual(m["metrics"]["bmc"]["prism"]["completeness"], 1.0)
            rc, m = self._run("NEEDS-HARNESS", Path(d))
            self.assertEqual(rc, 0)
            self.assertEqual(m["metrics"]["bmc"]["prism"]["completeness"], 0.0)
            rc, m = self._run("FAILED", Path(d))
            self.assertEqual(rc, 0)
            self.assertEqual(m["metrics"]["bmc"]["prism"]["false_alarms"], 2)
            self.assertEqual(m["metrics"]["bmc"]["prism"]["detection"], 0.0)  # not replayed


@unittest.skipUnless(shutil.which("clang") or shutil.which("gcc"), "no C compiler")
class SelfCheckSample(unittest.TestCase):
    """A sample of the label self-check (the workflow runs all of it)."""

    def test_sample(self) -> None:
        import tempfile
        cc, _ = conf.sanitizer_compilers()
        if not shutil.which(cc):
            self.skipTest("no sanitizer-capable compiler")
        tasks = [t for t in _tasks() if t.origin == "prism" and t.source.stem.split("_")[0]
                 in {"add", "mod", "shl", "arr", "ushort", "div0"}]
        self.assertTrue(tasks)
        with tempfile.TemporaryDirectory() as d:
            recs, bad = conf.self_check(tasks, Path(d), max(1, (os.cpu_count() or 2) // 2))
        self.assertEqual(bad, 0, [r for r in recs if r["check"] == "FAIL"])


if __name__ == "__main__":
    unittest.main()
