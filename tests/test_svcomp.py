"""SV-COMP readiness (roadmap 6.3): tools/svcomp/.

The result mapping must keep PRISM's laws: `true` only from a proof of main
that covers the property (never PROVED-ASSUMING or BOUNDED, Law 2), `false`
only from a refutation whose counterexample replays, everything else
`unknown`. Witnesses must be well-formed SV-COMP format 2.0 YAML.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
SV = REPO / "tools" / "svcomp"
PROPS = REPO / "tests" / "conformance" / "sv-comp" / "properties"


def _load(name: str, path: Path) -> Any:
    # By path under a private name: tools/svcomp/prism.py must never shadow
    # the `prism` package.
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


P = _load("prism_svcomp_wrapper_under_test", SV / "prism_svcomp.py")
W = P.W
RUN = _load("prism_svcomp_runner_under_test", SV / "run_subset.py")

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover
    yaml = None


def report(**stages: list[dict[str, Any]]) -> dict[str, Any]:
    return {"stages": [{"name": n, "status": "ok", "findings": [dict(function="main", **f) for f in fs]}
                       for n, fs in stages.items()]}


def replayed(_f: dict[str, Any]) -> dict[str, Any]:
    return {"replay": "replayed", "detail": "UBSan: signed integer overflow", "line": 3, "column": 9}


def not_replayed(_f: dict[str, Any]) -> dict[str, Any]:
    return {"replay": "not-replayed", "why": "ran clean"}


def cc_with_ubsan() -> bool:
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc:
        return False
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "t.c"
        src.write_text("int main(void){return 0;}\n")
        import subprocess

        r = subprocess.run([cc, "-fsanitize=signed-integer-overflow", str(src), "-o", str(Path(d) / "t")],
                           capture_output=True)
        return r.returncode == 0


class PropertyTest(unittest.TestCase):
    def test_property_files(self) -> None:
        self.assertEqual(P.parse_property((PROPS / "no-overflow.prp").read_text()), "no-overflow")
        self.assertEqual(P.parse_property((PROPS / "unreach-call.prp").read_text()), "unreach-call")
        self.assertEqual(P.parse_property((PROPS / "valid-memsafety.prp").read_text()), "valid-memsafety")
        self.assertEqual(P.parse_property("CHECK( init(main()), LTL(F end) )"), "unsupported")

    def test_task_property_paths_resolve(self) -> None:
        # The task .yml files name ../properties/<p>.prp; the runner needs them.
        for t in RUN.tasks():
            self.assertTrue(t["property_file"].exists(), t["id"])
            self.assertTrue(t["input"].exists(), t["id"])
        self.assertEqual(len(RUN.tasks()), 45)

    def test_width_dependent_code(self) -> None:
        self.assertTrue(P.width_dependent_code("int main(){ long x = 1; return 0; }"))
        self.assertTrue(P.width_dependent_code("typedef long L; typedef L M; int main(){ M x = 1; return 0; }"))
        self.assertTrue(P.width_dependent_code("struct s { long a; }; int main(){ struct s v; return 0; }"))
        self.assertFalse(P.width_dependent_code("extern long f(void);\nint main(){ int x = 1; return x; }"))
        self.assertFalse(P.width_dependent_code(
            "void reach_error() { ((void) sizeof ((0) ? 1 : 0)); }\nint main(){ return 0; }"))


class ReachErrorSiteTest(unittest.TestCase):
    def test_call_site_not_definition(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("void reach_error() { __assert_fail(\"0\", \"t.c\", 1, \"reach_error\"); }\n"
                           "int main(void) {\n  if (1) reach_error();\n  return 0;\n}\n")
            self.assertEqual(P.reach_error_call_site(src, None), (3, 10))
            src.write_text("void reach_error(void) {}\nint main(void) {\n  reach_error();\n  reach_error();\n}\n")
            self.assertIsNone(P.reach_error_call_site(src, None))
            self.assertEqual(P.reach_error_call_site(src, 4), (4, 3))


class DecideTest(unittest.TestCase):
    def test_true_only_from_covering_proofs(self) -> None:
        for st in ("PROVED", "PROVED-UNBOUNDED", "PROVED-CERTIFIED"):
            d = P.decide(report(bmc=[{"status": st, "cls": ""}]), "no-overflow", replayed)
            self.assertEqual(d.answer, "true", st)
        for st in ("PROVED-ASSUMING", "BOUNDED", "NEEDS-HARNESS", "UNKNOWN", "TIMEOUT", "ERROR", "NOTRUN", "CLEAN"):
            d = P.decide(report(bmc=[{"status": st, "cls": ""}]), "no-overflow", replayed)
            self.assertEqual(d.answer, "unknown", st)

    def test_false_needs_replay(self) -> None:
        rep = report(bmc=[{"status": "FAILED", "cls": "INT-SIGNED-OVF", "message": "ovf+"}])
        self.assertEqual(P.decide(rep, "no-overflow", replayed).answer, "false(no-overflow)")
        self.assertEqual(P.decide(rep, "no-overflow", not_replayed).answer, "unknown")

    def test_other_class_is_not_a_refutation(self) -> None:
        rep = report(bmc=[{"status": "PROVED-UNBOUNDED", "cls": ""}],
                     pir=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "extra": {"prop": "assert"}}])
        self.assertEqual(P.decide(rep, "no-overflow", replayed).answer, "true")
        rep = report(bmc=[{"status": "FAILED", "cls": "INT-SHIFT-UB"}])
        self.assertEqual(P.decide(rep, "no-overflow", replayed).answer, "unknown")

    def test_shift_overflow_is_an_overflow(self) -> None:
        # SV-COMP no-overflow: a signed `<<` whose result is not representable
        # counts (pir: shift-base, bmc: shift31); a bad shift count does not
        for f in ({"status": "FAILED", "cls": "INT-SHIFT-UB", "message": "shift31: INT-SHIFT-UB"},
                  {"status": "FAILED", "cls": "INT-SHIFT-UB", "extra": {"prop": "shift-base"}}):
            self.assertEqual(P.decide(report(bmc=[f]), "no-overflow", replayed).answer, "false(no-overflow)")
            self.assertEqual(P.decide(report(bmc=[f]), "no-overflow", not_replayed).answer, "unknown")
        for f in ({"status": "FAILED", "cls": "INT-SHIFT-UB", "message": "shift: INT-SHIFT-UB"},
                  {"status": "FAILED", "cls": "INT-SHIFT-UB", "message": "shift-neg: INT-SHIFT-UB"},
                  {"status": "FAILED", "cls": "INT-SHIFT-UB", "extra": {"prop": "shift"}}):
            self.assertEqual(P.decide(report(bmc=[f]), "no-overflow", replayed).answer, "unknown")
        # not an unreach-call refutation
        f = {"status": "FAILED", "cls": "INT-SHIFT-UB", "message": "shift31: INT-SHIFT-UB"}
        self.assertEqual(P.decide(report(pir=[f]), "unreach-call", replayed).answer, "unknown")

    def test_refutation_with_call_sites_is_preferred(self) -> None:
        # both stages refute; pir's also has the nondet call sites: its
        # witness can place every function_return waypoint
        rep = report(bmc=[{"status": "FAILED", "cls": "INT-SIGNED-OVF", "extra": {"nondet": "f=1"}}],
                     pir=[{"status": "FAILED", "cls": "INT-SIGNED-OVF",
                           "extra": {"nondet": "f=1", "nondet_loc": "3:9"}}])
        d = P.decide(rep, "no-overflow", replayed)
        self.assertEqual(d.answer, "false(no-overflow)")
        assert d.finding is not None
        self.assertEqual(d.finding["stage"], "pir")
        # both have call sites: pir first
        rep["stages"][0]["findings"][0]["extra"]["nondet_loc"] = "3:9"
        d = P.decide(rep, "no-overflow", replayed)
        assert d.finding is not None
        self.assertEqual(d.finding["stage"], "pir")

    def test_unreplayed_refutation_blocks_true(self) -> None:
        rep = report(bmc=[{"status": "PROVED", "cls": ""}],
                     pir=[{"status": "FAILED", "cls": "INT-SIGNED-OVF"}])
        self.assertEqual(P.decide(rep, "no-overflow", not_replayed).answer, "unknown")

    def test_unreach_call_proofs_and_refutations(self) -> None:
        # bmc and pir both encode reach_error() as a property: either proof covers it
        for st in ("bmc", "pir"):
            self.assertEqual(P.decide(report(**{st: [{"status": "PROVED", "cls": ""}]}), "unreach-call",
                                      replayed).answer, "true")
            self.assertEqual(P.decide(report(**{st: [{"status": "BOUNDED", "cls": ""}]}), "unreach-call",
                                      replayed).answer, "unknown")
        # bmc names the check in the message prefix
        rep = report(bmc=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "message": "reach_error: FUNC-CONTRACT"}])
        self.assertEqual(P.decide(rep, "unreach-call", replayed).answer, "false(unreach-call)")
        self.assertEqual(P.decide(rep, "unreach-call", not_replayed).answer, "unknown")
        rep = report(bmc=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "message": "abort: FUNC-CONTRACT"}])
        self.assertEqual(P.decide(rep, "unreach-call", replayed).answer, "unknown")
        # an unreplayed bmc refutation blocks pir's proof
        rep = report(bmc=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "message": "reach_error: FUNC-CONTRACT"}],
                     pir=[{"status": "PROVED", "cls": ""}])
        self.assertEqual(P.decide(rep, "unreach-call", not_replayed).answer, "unknown")
        rep = report(pir=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "extra": {"prop": "reach_error"}}])
        self.assertEqual(P.decide(rep, "unreach-call", replayed).answer, "false(unreach-call)")
        rep = report(pir=[{"status": "FAILED", "cls": "FUNC-CONTRACT", "extra": {"prop": "abort"}}])
        self.assertEqual(P.decide(rep, "unreach-call", replayed).answer, "unknown")

    def test_memsafety_never_true(self) -> None:
        rep = report(bmc=[{"status": "PROVED-CERTIFIED", "cls": ""}], pir=[{"status": "PROVED", "cls": ""}])
        self.assertEqual(P.decide(rep, "valid-memsafety", replayed).answer, "unknown")

    def test_crashed_stage_and_missing_main(self) -> None:
        self.assertEqual(P.decide({"stages": []}, "no-overflow", replayed).answer, "unknown")
        rep = {"stages": [{"name": "bmc", "status": "failed", "detail": "boom", "findings": []}]}
        self.assertEqual(P.decide(rep, "no-overflow", replayed).answer, "unknown")

    def test_scoring(self) -> None:
        self.assertEqual(RUN.score(True, "true"), ("correct", 2))
        self.assertEqual(RUN.score(False, "false(no-overflow)"), ("correct", 1))
        self.assertEqual(RUN.score(False, "true"), ("wrong", -32))
        self.assertEqual(RUN.score(True, "false(no-overflow)"), ("wrong", -16))
        self.assertEqual(RUN.score(True, "unknown"), ("unknown", 0))
        self.assertEqual(RUN.score(False, "ERROR"), ("unknown", 0))


class WitnessTest(unittest.TestCase):
    def build(self, **kw: Any) -> list[dict[str, Any]]:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) { int x = __VERIFIER_nondet_int(); return x + 1; }\n")
            cex = W.Counterexample(function="main", target=W.Location("t.c", 1, 58, "main"), **kw)
            return list(W.build_violation_witness(cex, input_file=src, input_file_name="t.c",
                                                  specification="CHECK( init(main()), LTL(G ! overflow) )\n",
                                                  producer_version="test", creation_time="2026-09-23T00:00:00Z",
                                                  witness_uuid="00000000-0000-4000-8000-000000000000"))

    def test_structure(self) -> None:
        doc = self.build(nondet=[W.NondetValue(W.Location("t.c", 1, 50, "main"), 2147483647)])
        self.assertEqual(len(doc), 1)
        e = doc[0]
        self.assertEqual(e["entry_type"], "violation_sequence")
        self.assertEqual(e["metadata"]["format_version"], "2.0")
        self.assertEqual(e["metadata"]["task"]["specification"], "CHECK( init(main()), LTL(G ! overflow) )")
        segs = e["content"]
        self.assertEqual(segs[-1]["segment"][0]["waypoint"]["type"], "target")
        self.assertEqual(segs[0]["segment"][0]["waypoint"]["type"], "function_return")
        self.assertEqual(segs[0]["segment"][0]["waypoint"]["constraint"]["value"], "\\result == 2147483647")
        # format 2.0: a function_return constraint is `\result <op> <constant>` in ACSL
        self.assertEqual(segs[0]["segment"][0]["waypoint"]["constraint"]["format"], "acsl_expression")
        neg = self.build(nondet=[W.NondetValue(W.Location("t.c", 1, 50), -3)])
        self.assertEqual(neg[0]["content"][0]["segment"][0]["waypoint"]["constraint"]["value"], "\\result == -3")
        # exactly one target, and it is last
        types = [s["segment"][0]["waypoint"]["type"] for s in segs]
        self.assertEqual(types.count("target"), 1)

    def test_target_without_column(self) -> None:
        # a statement first on its line: no column ("the first statement or
        # full expression in that line"), never column 1 by default
        self.assertEqual(W.Location("t.c", 4, None).as_dict(), {"file_name": "t.c", "line": 4})
        self.assertEqual(W.Location("t.c", 4).as_dict()["column"], 1)

    def test_params(self) -> None:
        doc = self.build(params={"a": 5, "b": -1}, param_location=W.Location("t.c", 1, 18, "main"))
        wp = doc[0]["content"][0]["segment"][0]["waypoint"]
        self.assertEqual(wp["type"], "assumption")
        self.assertEqual(wp["constraint"]["value"], "(a == 5) && (b == (-1))")

    @unittest.skipIf(yaml is None, "PyYAML not installed")
    def test_yaml_round_trip(self) -> None:
        doc = self.build(nondet=[W.NondetValue(W.Location("t.c", 1, 50), -3)],
                         params={"n": 1}, param_location=W.Location("t.c", 1, 18))
        self.assertEqual(yaml.safe_load(W.to_yaml(doc)), doc)

    def test_correctness_witness(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) {\n  int i = 0;\n  while (i < 9) i++;\n  return 0;\n}\n")
            kw = dict(input_file=src, input_file_name="t.c", specification="CHECK( init(main()), LTL(G ! overflow) )",
                      producer_version="test", creation_time="2026-09-23T00:00:00Z",
                      witness_uuid="00000000-0000-4000-8000-000000000000")
            empty = W.build_correctness_witness([], **kw)
            self.assertEqual(empty[0]["entry_type"], "invariant_set")
            self.assertEqual(empty[0]["content"], [])
            self.assertIn("content: []", W.to_yaml(empty))
            inv = W.Invariant("loop_invariant", W.Location("t.c", 3, 3, "main"), "(i >= 0) && (i <= 9)")
            doc = W.build_correctness_witness([inv], **kw)
            e = doc[0]["content"][0]["invariant"]
            self.assertEqual(e, {"type": "loop_invariant", "value": "(i >= 0) && (i <= 9)", "format": "c_expression",
                                 "location": {"file_name": "t.c", "line": 3, "column": 3, "function": "main"}})
            if yaml is not None:
                self.assertEqual(yaml.safe_load(W.to_yaml(doc)), doc)

    def test_correctness_invariants_only_proved_ones(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) {\n  int i = 0, n = 5;\n  while (i < n) i++;\n"
                           "  for (int k = 0; k < 3; k++) i--;\n  do { i++; } while (i < 3);\n  return 0;\n}\n")
            base = {"stage": "bmc", "function": "main", "status": "PROVED-UNBOUNDED"}
            loops = ('[{"kind":"while","line":3,"column":3},{"kind":"for","line":4,"column":3},'
                     '{"kind":"do","line":5,"column":3}]')
            f = dict(base, extra={"k_induction": "closed-invariants", "invariant_loops": loops,
                                  "invariants": '[["i >= 0", "i <= n", "i == 2 * n"], ["k >= 0", "i <= 5"], ["i >= 0"]]'})
            invs, _ = P.correctness_invariants(src, f)
            # arithmetic conjuncts, names declared in a for-init and do loops are left out
            self.assertEqual([(i.location.line, i.value) for i in invs], [(3, "(i >= 0) && (i <= n)"), (4, "(i <= 5)")])
            # arithmetic only when constant bounds keep every subterm in int
            self.assertEqual(P.exportable_conjuncts(["i >= 0", "i <= 1000", "s == 2 * i", "s == 3000000 * i",
                                                     "t <= s + 1", "i - 1 < i"]),
                             ["i >= 0", "i <= 1000", "s == 2 * i", "i - 1 < i"])
            # plain k-induction or a bounded-unwind proof: no invariant (empty set)
            for extra in ({"k_induction": "closed"}, {"k_induction": "not-needed", "unwind_closed": "true"}):
                self.assertEqual(P.correctness_invariants(src, dict(base, extra=extra))[0], [])

    @unittest.skipUnless(shutil.which("clang") and shutil.which("opt"), "clang/opt not on PATH")
    def test_pir_invariants_in_c(self) -> None:
        # pir exports conjuncts over IR values; the wrapper names them with the
        # C variables (debug information) and keeps only what C means the same
        src_text = ("extern unsigned int __VERIFIER_nondet_uint(void);\n"
                    "int main(void) {\n"
                    "  unsigned int y = 1U, n = __VERIFIER_nondet_uint();\n"
                    "  int k = 0;\n"
                    "  while (y < n) {\n"
                    "    y = y + 2U;\n"
                    "    k = k + 1;\n"
                    "    for (int j = 0; j < 3; j++) k = k + j;\n"
                    "  }\n"
                    "  return 0;\n"
                    "}\n")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text(src_text)
            v = lambda n, w=32: {"v": n, "w": w}  # noqa: E731
            c = lambda x, w=32: {"c": str(x), "w": w}  # noqa: E731
            conj = [[{"rel": "mask:1", "a": v("y.0"), "b": c(1)},
                     {"rel": "ule", "a": v("y.0"), "b": v("call")},
                     {"rel": "sge", "a": v("k.0"), "b": c(0)},
                     {"rel": "ule", "a": v("k.0"), "b": c(5)},       # unsigned relation on an int: left out
                     {"rel": "eq", "a": v("nope"), "b": c(0)}],       # unknown value: left out
                    [{"rel": "sge", "a": v("k.1"), "b": v("k.0")},     # k.0 is not k's value at the inner loop
                     {"rel": "sge", "a": v("j.0"), "b": c(0)},
                     {"rel": "ule", "a": v("y.0"), "b": v("call")},     # y was assigned before this loop
                     {"rel": "sge", "a": v("k.1"), "b": c(1)}]]
            f = {"stage": "pir", "function": "main", "status": "PROVED-UNBOUNDED",
                 "extra": {"k_induction": "closed-invariants", "invariant_conjuncts": json.dumps(conj),
                           "invariant_loops": json.dumps([{"kind": "", "line": 5, "column": 3},
                                                          {"kind": "", "line": 8, "column": 5}])}}
            invs, note = P.correctness_invariants(src, f)
            got = [(i.location.line, i.location.column, i.value) for i in invs]
            # j is declared in the for-init: not in scope at the keyword
            self.assertEqual(got, [(5, 3, "((y & 1) == 1) && (y <= n) && (k >= 0)"), (8, 5, "(k >= 1)")], note)

    def test_cex_parsing(self) -> None:
        self.assertEqual(W.parse_assignments("a=1, b=-2"), {"a": 1, "b": -2})
        vals = W.parse_assignments("a=#xffffffff, b=#b101, c=true")
        self.assertEqual(vals, {"a": 0xFFFFFFFF, "b": 5, "c": 1})
        self.assertEqual(W.reinterpret(0xFFFFFFFF, "int"), -1)
        self.assertEqual(W.reinterpret(0xFFFFFFFF, "unsigned int"), 0xFFFFFFFF)


@unittest.skipUnless(cc_with_ubsan(), "no C compiler with UBSan")
class ReplayTest(unittest.TestCase):
    def test_deterministic_overflow_replays(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) {\n  int x = 2147483647;\n  x = x + 1;\n  return x == 0;\n}\n")
            rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=None, work=Path(d) / "w")
            self.assertEqual(rp["replay"], "replayed", rp)
            self.assertEqual(rp["line"], 3)

    def test_law9_no_exec_without_allow(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) { int x = 2147483647; x = x + 1; return 0; }\n")
            rp = P.replay(src, "no-overflow", allow_exec=False, nondet_values=None, work=Path(d) / "w")
            self.assertEqual(rp["replay"], "notrun")

    def test_nondet_needs_values(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("extern int __VERIFIER_nondet_int(void);\n"
                           "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  x = x + 1;\n  return 0;\n}\n")
            rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=None, work=Path(d) / "w")
            self.assertEqual(rp["replay"], "unsupported")
            rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=[2147483647], work=Path(d) / "w2")
            self.assertEqual(rp["replay"], "replayed", rp)
            rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=[5], work=Path(d) / "w3")
            self.assertEqual(rp["replay"], "not-replayed", rp)

    def test_shift_overflow_replays_other_shift_ub_does_not(self) -> None:
        # only a left shift whose result is not representable is an overflow
        cases = {"31": "replayed", "0": "not-replayed"}
        for s, want in cases.items():
            with tempfile.TemporaryDirectory() as d:
                src = Path(d) / "t.c"
                src.write_text("int main(void) {\n  int x = 1, s = %s;\n  x = x << s;\n  return x == 0;\n}\n" % s)
                rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=None, work=Path(d) / "w")
                self.assertEqual(rp["replay"], want, (s, rp))
                if want == "replayed":
                    self.assertEqual(rp["line"], 3)
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text("int main(void) {\n  int x = -1, s = 1;\n  x = x << s;\n  return x == 0;\n}\n")
            rp = P.replay(src, "no-overflow", allow_exec=True, nondet_values=None, work=Path(d) / "w")
            self.assertEqual(rp["replay"], "not-replayed", rp)  # negative base: UB, but not an overflow


class NondetTraceTest(unittest.TestCase):
    SRC = ("extern int __VERIFIER_nondet_int(void);\n"
           "unsigned char __VERIFIER_nondet_uchar();\n"
           "int main(void) {\n"
           "  int a = __VERIFIER_nondet_int();\n"
           "  unsigned char k = __VERIFIER_nondet_uchar();\n"
           "  while (k--) a += __VERIFIER_nondet_int();\n"
           "  return a;\n}\n")

    def test_trace_from_extra(self) -> None:
        f = {"function": "main", "extra": {"nondet": "__VERIFIER_nondet_int=-5, __VERIFIER_nondet_uchar=200"}}
        self.assertEqual(P.nondet_trace(f), [("__VERIFIER_nondet_int", -5), ("__VERIFIER_nondet_uchar", 200)])
        self.assertEqual(P.nondet_trace({"function": "main", "extra": {"nondet": ""}}), [])
        # not reported, or not a whole-program trace: no values (no replay)
        self.assertIsNone(P.nondet_trace({"function": "main", "extra": {}}))
        self.assertIsNone(P.nondet_trace({"function": "helper", "extra": {"nondet": "x=1"}}))
        self.assertIsNone(P.nondet_trace({"function": "main", "extra": {"nondet": "f=zz"}}))

    def test_waypoints_stop_at_an_ambiguous_site(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text(self.SRC)
            wps = P.nondet_waypoints(src, [("__VERIFIER_nondet_uchar", 1), ("__VERIFIER_nondet_int", 3)])
            # declarations are not call sites; the uchar call is unique
            self.assertEqual(len(wps), 1)
            self.assertEqual((wps[0].location.line, wps[0].location.column), (5, 45))
            self.assertEqual(wps[0].value, 1)
            # __VERIFIER_nondet_int is called at two sites: no waypoint for it
            self.assertEqual(P.nondet_waypoints(src, [("__VERIFIER_nondet_int", 3)]), [])

    TRACE = [("__VERIFIER_nondet_int", 7), ("__VERIFIER_nondet_uchar", 1), ("__VERIFIER_nondet_int", -2)]

    def test_locations_from_extra(self) -> None:
        f = {"function": "main", "extra": {"nondet": "a=1, b=2", "nondet_loc": "4:11, 0:0"}}
        self.assertEqual(P.nondet_locations(f, 2), [(4, 11), None])
        self.assertIsNone(P.nondet_locations(f, 3))  # does not match the trace
        self.assertIsNone(P.nondet_locations({"extra": {}}, 0))
        self.assertIsNone(P.nondet_locations({"extra": {"nondet_loc": "4:x"}}, 1))
        f = {"function": "main", "extra": {"nondet": "__VERIFIER_nondet_float=0.25"}}
        self.assertEqual(P.nondet_trace(f), [("__VERIFIER_nondet_float", 0.25)])

    def test_exact_locations_place_every_call(self) -> None:
        # pir's debug locations point at the start of each call; the waypoint
        # goes on its closing parenthesis, also where a function has two sites
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text(self.SRC)
            wps = P.nondet_waypoints(src, self.TRACE, [(4, 11), (5, 21), (6, 20)])
            self.assertEqual([(w.location.line, w.location.column, w.value) for w in wps],
                             [(4, 33, 7), (5, 45, 1), (6, 42, -2)])

    def test_wrong_location_falls_back_to_the_unique_site(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.c"
            src.write_text(self.SRC)
            # the first location names no call of that function: the int call
            # has two sites, so the prefix ends before it
            self.assertEqual(P.nondet_waypoints(src, self.TRACE, [(4, 12), (5, 21), (6, 20)]), [])
            # an unknown uchar location falls back to its only call site
            wps = P.nondet_waypoints(src, self.TRACE, [(4, 11), None, (6, 20)])
            self.assertEqual([(w.location.line, w.location.column) for w in wps], [(4, 33), (5, 45), (6, 42)])
            # a location list of the wrong length is ignored as a whole
            self.assertEqual(len(P.nondet_waypoints(src, self.TRACE, [(4, 11)])), 0)

    def test_line_markers_disable_debug_locations(self) -> None:
        # in a preprocessed task, debug lines name the original file's lines
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "t.i"
            src.write_text('# 1 "t.c"\n' + self.SRC)
            wps = P.nondet_waypoints(src, self.TRACE, [(5, 11), (6, 21), (7, 20)])
            self.assertEqual(wps, [])  # int has two sites: nothing placed without the locations
            # bmc's positions are physical (the analysed text itself): kept
            wps = P.nondet_waypoints(src, self.TRACE, [(5, 11), (6, 21), (7, 20)], physical=True)
            self.assertEqual(len(wps), 3)

    def test_doctests(self) -> None:
        import doctest

        for mod in (P, W):
            res = doctest.testmod(mod, verbose=False)
            self.assertGreater(res.attempted, 0, mod.__name__)
            self.assertEqual(res.failed, 0, mod.__name__)


class ToolInfoTest(unittest.TestCase):
    def setUp(self) -> None:
        try:
            from benchexec.tools.template import BaseTool2  # noqa: F401
        except ImportError:
            self.skipTest("BenchExec not installed")
        self.mod = _load("prism_svcomp_toolinfo_under_test", SV / "prism.py")

    def test_cmdline(self) -> None:
        from benchexec.tools.template import BaseTool2

        tool = self.mod.Tool()
        task = BaseTool2.Task.with_files(["x.c"], property_file="p.prp", options={"data_model": "ILP32"})
        cmd = tool.cmdline("prism_svcomp.py", ["--allow-exec"], task, BaseTool2.ResourceLimits())
        self.assertEqual(cmd, ["prism_svcomp.py", "--allow-exec", "--prop", "p.prp", "--data-model", "ILP32", "x.c"])

    def test_determine_result(self) -> None:
        import benchexec.result as result
        from benchexec.tools.template import BaseTool2
        from benchexec.util import ProcessExitCode

        tool = self.mod.Tool()

        def run(lines: list[str]) -> Any:
            return BaseTool2.Run([], ProcessExitCode.create(value=0), BaseTool2.RunOutput([ln + "\n" for ln in lines]),
                                 None)

        self.assertEqual(tool.determine_result(run(["PRISM-SVCOMP-RESULT: true"])), result.RESULT_TRUE_PROP)
        self.assertEqual(tool.determine_result(run(["PRISM-SVCOMP-RESULT: false(no-overflow)"])),
                         result.RESULT_FALSE_OVERFLOW)
        self.assertEqual(tool.determine_result(run(["PRISM-SVCOMP-RESULT: unknown"])), result.RESULT_UNKNOWN)
        self.assertEqual(tool.determine_result(run(["nothing"])), result.RESULT_ERROR)


@unittest.skipUnless(os.environ.get("PRISM_BIN"), "PRISM_BIN not set")
class EndToEndTest(unittest.TestCase):
    """The wrapper on two pinned SV-COMP tasks with the C++ engine."""

    def run_task(self, rel: str) -> Any:
        with tempfile.TemporaryDirectory() as d:
            oc = P.solve(REPO / "tests/conformance/sv-comp" / rel, PROPS / "no-overflow.prp",
                         prism=os.environ["PRISM_BIN"], allow_exec=True, data_model="LP64",
                         out=Path(d) / "out", witness_path=Path(d) / "witness.yml")
            text = oc.witness_path.read_text() if oc.witness_path else None
            return oc.decision, text

    def test_false_task(self) -> None:
        if not cc_with_ubsan():
            self.skipTest("no C compiler with UBSan")
        dec, wit = self.run_task("signedintegeroverflow-regression/PostfixIncrement.i")
        self.assertEqual(dec.answer, "false(no-overflow)", dec.reason)
        self.assertIsNotNone(wit)
        assert wit is not None
        self.assertIn("entry_type: violation_sequence", wit)
        if yaml is not None:
            doc = yaml.safe_load(wit)
            self.assertEqual(doc[0]["content"][-1]["segment"][0]["waypoint"]["type"], "target")

    def test_true_task(self) -> None:
        dec, wit = self.run_task("loop-simple/nested_1.c")
        self.assertEqual(dec.answer, "true", dec.reason)
        # a `true` answer carries a correctness witness (format 2.0 invariant_set)
        self.assertIsNotNone(wit)
        assert wit is not None
        self.assertIn("entry_type: invariant_set", wit)


if __name__ == "__main__":
    unittest.main()
