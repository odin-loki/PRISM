"""Shape of the conformance suite and the rules of tools/conformance.py.

The engine itself is scored by `python tools/conformance.py` (and the
conformance workflow); these tests keep the suite and the scorer honest
without depending on the engine's current numbers.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
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

PROPERTIES = {"no-overflow", "no-div0", "no-shift-ub", "no-oob", "no-null-deref", "memsafety", "no-fp-cast",
              "no-uncaught"}


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

    def test_concurrency_tasks(self) -> None:
        # roadmap 2.6: whole thread programs, true/false pairs, scored by conc only
        tasks = conf.discover([SUITE / "concurrency"])
        self.assertGreaterEqual(len(tasks), 20)
        stems = {t.source.stem for t in tasks}
        for t in tasks:
            self.assertEqual(t.origin, "concurrency", t.ident)
            self.assertIn(t.prop, {"norace", "noassert", "nodeadlock"}, t.ident)
            self.assertEqual(list(t.expected), ["main"], t.ident)
            self.assertIn("pthread_create", t.source.read_text().replace("thrd_create", "pthread_create"))
            stem = t.source.stem
            twin = stem[:-5] + "_false" if stem.endswith("_true") else stem[:-6] + "_true"
            self.assertIn(twin, stems, t.ident)
        self.assertEqual({t.prop for t in tasks}, {"norace", "noassert", "nodeadlock"})
        self.assertEqual(conf.STAGE_TASK_ORIGINS["conc"], {"concurrency"})
        self.assertEqual(conf.ORIGIN_STAGES["concurrency"], {"conc"})
        self.assertIn("conc", conf.VERDICT_STAGES)

    def test_juliet_is_pinned_not_vendored(self) -> None:
        sources = (SUITE / "SOURCES.md").read_text()
        self.assertIn(conf.JULIET_SHA256, sources)
        self.assertIn(str(conf.JULIET_SIZE), sources)
        self.assertFalse(list(SUITE.rglob("CWE*")), "Juliet stays out of git")


class EsbmcCpp(unittest.TestCase):
    """Roadmap 2.7: ESBMC's C++ regression tests (pinned; a curated subset in git)."""

    def test_subset_is_pinned_and_small(self) -> None:
        tasks = conf.discover([SUITE / "esbmc-cpp"])
        self.assertGreaterEqual(len(tasks), 40)
        self.assertLessEqual(len(tasks), 80, "the full ESBMC set stays out of git (--fetch-esbmc)")
        for t in tasks:
            self.assertEqual(t.origin, "esbmc-cpp", t.ident)
            self.assertEqual(t.prop, "esbmc-cpp", t.ident)
            self.assertEqual(list(t.expected), ["main"], t.ident)
            self.assertTrue(t.deterministic, t.ident)
            self.assertIn(f"esbmc@{conf.ESBMC_COMMIT[:12]} regression/esbmc-cpp", t.yml.read_text(), t.ident)
            self.assertNotRegex(t.source.read_text(errors="replace"), conf.ESBMC_FOREIGN_TEXT, t.ident)
        self.assertEqual({t.expected["main"] for t in tasks}, {True, False})
        sources = (SUITE / "SOURCES.md").read_text()
        self.assertIn(conf.ESBMC_COMMIT, sources)
        self.assertTrue((SUITE / "esbmc-cpp" / "LICENSE.Apache-2.0.txt").exists())
        self.assertIn("esbmc-cpp", conf.PROPERTY_SCOPED)
        self.assertIn("esbmc-cpp", conf.DEFAULT_ROOTS)

    def test_convert(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            reg = Path(d) / "regression"

            def mk(name: str, desc: str, files: dict[str, str]) -> Path:
                td = reg / "esbmc-cpp" / "vector" / name
                td.mkdir(parents=True)
                (td / "test.desc").write_text(desc)
                for fn, text in files.items():
                    (td / fn).write_text(text)
                return td

            ok = mk("v1", "CORE\nmain.cpp\n--unwind 10 --no-unwinding-assertions\n^VERIFICATION SUCCESSFUL$\n",
                    {"main.cpp": "#include <cassert>\nint main() { assert(1); }\n"})
            t, why = conf.esbmc_convert(ok, reg)
            self.assertEqual(why, "")
            assert t is not None
            self.assertTrue(t["expected"])
            self.assertEqual(t["label_bound"], 10)
            self.assertTrue(t["deterministic"])
            self.assertEqual(conf.esbmc_task_name(t["upstream"]), ("cpp03_vector", "v1"))
            bad = mk("v2", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n",
                     {"main.cpp": "int nondet_int();\nint main() { return 1 / nondet_int(); }\n"})
            t, why = conf.esbmc_convert(bad, reg)
            assert t is not None
            self.assertFalse(t["expected"])
            self.assertFalse(t["deterministic"])
            for name, desc, files, reason in [
                ("k", "KNOWNBUG\nmain.cpp\n\n^VERIFICATION FAILED$\n", {"main.cpp": ""}, "KNOWNBUG"),
                ("o", "CORE\nmain.cpp\n--overflow-check\n^VERIFICATION FAILED$\n", {"main.cpp": ""},
                 "--overflow-check"),
                ("m", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n", {"main.cpp": "", "a.h": ""}, "several"),
                ("c", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n^  String capacity exceeded$\n",
                 {"main.cpp": ""}, "operational model"),
                ("n", "CORE\nmain.cpp\n\n^EXIT=0$\n", {"main.cpp": ""}, "no single"),
            ]:
                t, why = conf.esbmc_convert(mk(name, desc, files), reg)
                self.assertIsNone(t, name)
                self.assertIn(reason, why, name)

    def test_property_scope(self) -> None:
        t = conf.Task(ident="x.yml", yml=Path("x.yml"), source=Path("x.cpp"), origin="esbmc-cpp", category="c",
                      lang="C++", prop="esbmc-cpp", expected={"main": True})
        # ESBMC does not check signed overflow by default: another property
        self.assertEqual(conf.classify(t, "main", [{"status": "FAILED", "cls": "INT-SIGNED-OVF"}]),
                         "failed-other-property")
        self.assertEqual(conf.classify(t, "main", [{"status": "FAILED", "cls": "FUNC-CONTRACT"}]), "false-alarm")
        t.expected["main"] = False
        self.assertEqual(conf.classify(t, "main", [{"status": "PROVED"}]), "wrong-proof")


class LibcModels(unittest.TestCase):
    """Roadmap 8.2: contract harnesses for the pir stage's libc models."""

    def test_harnesses(self) -> None:
        tasks = conf.discover([SUITE / "libc-models"])
        self.assertGreaterEqual(len(tasks), 4)
        models = REPO / "src" / "prism" / "pir" / "models" / "libc"
        included: set[str] = set()
        for t in tasks:
            self.assertEqual(t.origin, "libc-models", t.ident)
            text = t.source.read_text()
            included |= set(re.findall(r'#include "\.\./\.\./\.\./src/prism/pir/models/libc/(\w+\.c)"', text))
            for fn, exp in t.expected.items():
                self.assertIn(f"int {fn}(", text, t.ident)
                self.assertEqual(fn.endswith("_true"), exp, fn)
            self.assertEqual({True, False}, set(t.expected.values()), t.ident)
            # every false harness names the violation it plants
            self.assertEqual(set(t.expect_class), {fn for fn, e in t.expected.items() if not e}, t.ident)
        self.assertEqual(included, {p.name for p in models.glob("*.c")}, "every model file has a harness")
        self.assertIn("libc-models", conf.DEFAULT_ROOTS)

    def test_expect_class(self) -> None:
        t = conf.Task(ident="x.yml", yml=Path("x.yml"), source=Path("x.c"), origin="libc-models", category="c",
                      lang="C", prop="libc-contract", expected={"f": False},
                      expect_class={"f": {"MEM-OOB-WRITE", "MEM-OOB-READ"}})
        self.assertEqual(conf.classify(t, "f", [{"status": "FAILED", "cls": "MEM-OOB-WRITE"}]), "refuted")
        # refuted for another reason than the planted one: not a detection
        self.assertEqual(conf.classify(t, "f", [{"status": "FAILED", "cls": "UNINIT-READ"}]),
                         "failed-other-property")


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
        # concurrency labels are property-scoped too; conc is BOUNDED on true tasks
        race = [{"status": "FAILED", "cls": "CONC-DATA-RACE"}]
        noassert_true = self._task(True, origin="concurrency", prop="noassert")
        self.assertEqual(conf.classify(noassert_true, "f", race), "failed-other-property")
        self.assertEqual(conf.classify(noassert_true, "f", [{"status": "BOUNDED"}]), "bounded")
        self.assertEqual(conf.classify(self._task(False, origin="concurrency", prop="norace"), "f", race),
                         "refuted")
        self.assertEqual(conf.classify(self._task(True, origin="concurrency", prop="norace"), "f", race),
                         "false-alarm")

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
