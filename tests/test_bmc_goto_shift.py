"""bmc: the goto model, C++ shift rules by -std, function-try-block extraction.

- goto (docs/SVCOMP.md "goto"): a forward jump out to a later statement of
  the same or an enclosing statement list and a backward goto that forms a
  loop are encoded (the loop is unwound like any other and k-induction
  havocs it); any other goto is NEEDS-HARNESS ("unstructured goto
  unencoded"), never ERROR and never a guess.
- Shifts (docs/CONFORMANCE.md F7): a C++ unit gets C++ rules for `<<` by
  its -std (compile_commands.json, else the compilers' default gnu++17):
  C++20 defines every signed left shift with an in-range count, C++11..17
  define 1 << 31 but not a negative left operand, C and C++98/03 neither.
- A function-try-block (`void f(int &x) try { } catch (...) { }`) is
  extracted with the try block and its handlers as its body (it was a
  PARSE-GAP; F10).

Both engines: the Python engine directly, the C++ engine through PRISM_BIN
(or build/prism) on the same files.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, cxx_std_year, k_induction, with_cxx_std
from prism.cparse import extract_functions, parse_gaps

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

GOTO_EXPECT = {
    "goto_out_ok": {laws.PROVED, laws.PROVED_UNBOUNDED},
    "goto_out_bad": {laws.FAILED},
    "goto_loop_ok": {laws.PROVED, laws.PROVED_UNBOUNDED},
    "goto_loop_bad": {laws.FAILED},
    "goto_loop_open": {laws.PROVED_UNBOUNDED},
    "goto_into_block": {laws.NEEDS_HARNESS},
    "goto_past_decl": {laws.NEEDS_HARNESS},
    "goto_loop_deep": {laws.BOUNDED},
    "goto_uninit": {laws.FAILED},
}


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    exe = "prism.exe" if os.name == "nt" else "prism"
    cands += [ROOT / d / exe for d in ("build", "build_wsl", "build/Release")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def _cpp_bmc(root: Path) -> dict[str, dict]:
    """function -> bmc finding of the C++ engine on root."""
    exe = _cpp_prism()
    assert exe is not None
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([str(exe), str(root), "--no-llm", "--stage", "inventory,classify,bmc",
                        "--out", out], cwd=ROOT, capture_output=True, text=True, timeout=600)
        rep = json.loads((Path(out) / "report.json").read_text(encoding="utf-8"))
    rows: dict[str, dict] = {}
    for st in rep["stages"]:
        if st["name"] == "bmc":
            for f in st["findings"]:
                if f.get("function"):
                    rows[f["function"]] = f
    return rows


def _py(src: str, name: str, file: str, std: int = 0, unwind: int = 8):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / file
        p.write_text(src, encoding="utf-8")
        fn = next(f for f in extract_functions(p, file) if f.name == name)
    fn.cxx_std = std
    return k_induction(fn, unwind)


SHIFT_POS = "int f(int s) {\n    if (s < 0 || s > 31) return 0;\n    return 1 << s;\n}\n"
SHIFT_NEG = "int f(int s) {\n    if (s < 0 || s > 31) return 0;\n    return -1 << s;\n}\n"
SHIFT_THREE = "int f(int s) {\n    if (s < 0 || s > 31) return 0;\n    return 3 << s;\n}\n"
SHIFT_COUNT = "int f(int s) {\n    if (s < 0 || s > 32) return 0;\n    return 1 << s;\n}\n"


@unittest.skipUnless(HAS_Z3, "z3 not installed")
class TestGotoPython(unittest.TestCase):
    def test_goto_structured_file(self):
        fns = extract_functions(TD / "goto_structured.c", "goto_structured.c")
        seen = set()
        for fn in fns:
            r = k_induction(fn, 8)
            seen.add(fn.name)
            self.assertIn(r.status, GOTO_EXPECT[fn.name], f"{fn.name}: {r.message}")
            if r.status == laws.NEEDS_HARNESS:
                self.assertIn("unstructured goto unencoded", r.message)
        self.assertEqual(seen, set(GOTO_EXPECT))

    def test_goto_out_bad_is_overflow(self):
        fn = next(f for f in extract_functions(TD / "goto_structured.c", "g.c")
                  if f.name == "goto_out_bad")
        r = k_induction(fn, 8)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_with_goto_proved(self):
        fn = next(f for f in extract_functions(TD / "goto_unenc.c", "g.c") if f.name == "with_goto")
        self.assertEqual(k_induction(fn, 8).status, laws.PROVED_UNBOUNDED)

    def test_goto_into_else_is_harness(self):
        src = ("int f(int x) {\n    if (x) {\n        goto l;\n    } else {\n    l:\n"
               "        x = 1;\n    }\n    return x;\n}\n")
        r = _py(src, "f", "t.c")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_backward_goto_from_inner_loop(self):
        # The backward goto crosses an inner loop; 3 passes close the region.
        src = ("int f(int x) {\n    int n = 0;\nagain:\n    n++;\n"
               "    for (int i = 0; i < 2; i++) {\n        if (n < 3) goto again;\n    }\n"
               "    return 100 / (n - 3);\n}\n")
        r = _py(src, "f", "t.c")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-DIV-ZERO")

    def test_forward_goto_out_of_switch(self):
        src = ("int f(int x) {\n    switch (x) {\n    case 1:\n        goto out;\n"
               "    default:\n        x = x * 2;\n    }\n    return 0;\nout:\n    return 7 / x;\n}\n")
        r = _py(src, "f", "t.c")
        self.assertIn(r.status, {laws.FAILED}, r.message)  # x * 2 overflows
        src_ok = src.replace("x = x * 2;", "x = 0;")
        r = _py(src_ok, "f", "t.c")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_label_not_reached_is_harness(self):
        # A goto to a label in a nested block that comes later (jump into it).
        src = "int f(int x) {\n    goto l;\n    {\n    l:\n        x++;\n    }\n    return x;\n}\n"
        r = _py(src, "f", "t.c")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_goto_callee_not_inlined(self):
        from prism.inline import inline_static
        src = ("static int g(int x) {\n    if (x) goto l;\n    x = 1;\nl:\n    return x;\n}\n"
               "int f(int y) {\n    return g(y) + g(y);\n}\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.c"
            p.write_text(src, encoding="utf-8")
            fns = extract_functions(p, "t.c")
        f = next(x for x in inline_static(fns) if x.name == "f")
        self.assertIn("g(", f.body)


@unittest.skipUnless(HAS_Z3, "z3 not installed")
class TestShiftRulesPython(unittest.TestCase):
    def test_std_year(self):
        self.assertEqual(cxx_std_year("-std=c++20"), 20)
        self.assertEqual(cxx_std_year("-std=gnu++2a"), 20)
        self.assertEqual(cxx_std_year("-std=c++17"), 17)
        self.assertEqual(cxx_std_year("-std=c++0x"), 11)
        self.assertEqual(cxx_std_year("-std=c++98"), 3)
        self.assertEqual(cxx_std_year("-std=c11"), 0)

    def test_c_keeps_c_rules(self):
        r = _py(SHIFT_POS, "f", "t.c")
        self.assertEqual((r.status, r.cls), (laws.FAILED, "INT-SHIFT-UB"), r.message)

    def test_cxx_default_is_cxx17(self):
        # F7: 1 << 31 is defined since C++11 (CWG 1457); a negative left operand is not.
        self.assertIn(_py(SHIFT_POS, "f", "t.cpp").status, {laws.PROVED, laws.PROVED_UNBOUNDED})
        self.assertEqual(_py(SHIFT_NEG, "f", "t.cpp").status, laws.FAILED)
        self.assertEqual(_py(SHIFT_THREE, "f", "t.cpp").status, laws.FAILED)  # 3 * 2^31

    def test_cxx20_defines_signed_left_shift(self):
        for src in (SHIFT_POS, SHIFT_NEG, SHIFT_THREE):
            self.assertIn(_py(src, "f", "t.cpp", std=20).status, {laws.PROVED, laws.PROVED_UNBOUNDED})
        r = _py(SHIFT_COUNT, "f", "t.cpp", std=23)
        self.assertEqual((r.status, r.cls), (laws.FAILED, "INT-SHIFT-UB"), r.message)

    def test_cxx03_keeps_c_rules(self):
        self.assertEqual(_py(SHIFT_POS, "f", "t.cpp", std=3).status, laws.FAILED)

    def test_header_keeps_c_rules(self):
        self.assertEqual(_py(SHIFT_POS, "f", "t.h").status, laws.FAILED)

    def test_with_cxx_std_reads_compile_commands(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "a.cpp").write_text(SHIFT_NEG, encoding="utf-8")
            (root / "b.cpp").write_text(SHIFT_NEG.replace("f(", "g("), encoding="utf-8")
            (root / "compile_commands.json").write_text(json.dumps([
                {"directory": str(root), "file": "a.cpp", "arguments": ["c++", "-std=c++20", "-c", "a.cpp"]},
                {"directory": str(root), "file": "b.cpp", "command": "c++ -std=c++20 -std=c++14 -c b.cpp"},
            ]), encoding="utf-8")
            fns = extract_functions(root / "a.cpp", "a.cpp") + extract_functions(root / "b.cpp", "b.cpp")
            got = {f.name: f.cxx_std for f in with_cxx_std(fns, root)}
            self.assertEqual(got, {"f": 20, "g": 14})
            self.assertNotIn("cxx_std", fns[0].to_dict())


class TestFunctionTryBlock(unittest.TestCase):
    SRC = ("static void set_ten(int &x) try {\n    x = 10;\n} catch (const int &i) {\n    x = i;\n}"
           " catch (...) {\n}\n\nint user(int n) {\n    int v = 0;\n    set_ten(v);\n    return 100 / v;\n}\n")

    def test_extracted_with_handlers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.cpp"
            p.write_text(self.SRC, encoding="utf-8")
            fns = extract_functions(p, "t.cpp")
            gaps = parse_gaps(p)
        by = {f.name: f for f in fns}
        self.assertEqual(set(by), {"set_ten", "user"})
        body = by["set_ten"].body
        self.assertTrue(body.startswith("try {"), body)
        self.assertTrue(body.rstrip().endswith("catch (...) {\n}"), body)
        self.assertEqual(by["set_ten"].span, (1, 6))
        self.assertNotIn("try", by["set_ten"].signature)
        self.assertEqual(gaps, [])

    @unittest.skipUnless(HAS_Z3, "z3 not installed")
    def test_bmc_is_harness_not_a_guess(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.cpp"
            p.write_text(self.SRC, encoding="utf-8")
            fns = extract_functions(p, "t.cpp")
        from prism.bmc import run_bmc
        for r in run_bmc(fns, 8):
            self.assertEqual(r.status, laws.NEEDS_HARNESS, f"{r.function}: {r.message}")


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestCppEngine(unittest.TestCase):
    def test_goto_structured_parity(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "goto_structured.c").write_text(
                (TD / "goto_structured.c").read_text(encoding="utf-8"), encoding="utf-8")
            rows = _cpp_bmc(root)
        for name, want in GOTO_EXPECT.items():
            self.assertIn(rows[name]["status"], want, f"{name}: {rows[name]['message']}")
            py = k_induction(next(f for f in extract_functions(TD / "goto_structured.c", "goto_structured.c")
                                  if f.name == name), 8)
            self.assertEqual(rows[name]["status"], py.status, name)

    def test_shift_rules_by_compile_commands(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "neg20.cpp").write_text(SHIFT_NEG.replace("f(", "neg20("), encoding="utf-8")
            (root / "neg17.cpp").write_text(SHIFT_NEG.replace("f(", "neg17("), encoding="utf-8")
            (root / "pos03.cpp").write_text(SHIFT_POS.replace("f(", "pos03("), encoding="utf-8")
            (root / "posdef.cpp").write_text(SHIFT_POS.replace("f(", "posdef("), encoding="utf-8")
            (root / "posc.c").write_text(SHIFT_POS.replace("f(", "posc("), encoding="utf-8")
            (root / "compile_commands.json").write_text(json.dumps([
                {"directory": str(root), "file": "neg20.cpp", "arguments": ["c++", "-std=c++20", "-c", "neg20.cpp"]},
                {"directory": str(root), "file": "neg17.cpp", "arguments": ["c++", "-std=gnu++17", "-c", "neg17.cpp"]},
                {"directory": str(root), "file": "pos03.cpp", "command": "c++ -std=c++03 -c pos03.cpp"},
            ]), encoding="utf-8")
            rows = _cpp_bmc(root)
        proved = {laws.PROVED, laws.PROVED_UNBOUNDED}
        self.assertIn(rows["neg20"]["status"], proved, rows["neg20"]["message"])
        self.assertEqual(rows["neg17"]["status"], laws.FAILED)
        self.assertEqual(rows["pos03"]["status"], laws.FAILED)
        self.assertIn(rows["posdef"]["status"], proved, rows["posdef"]["message"])
        self.assertEqual(rows["posc"]["status"], laws.FAILED)

    def test_function_try_block_no_parse_gap(self):
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as out:
            root = Path(d)
            (root / "t.cpp").write_text(TestFunctionTryBlock.SRC, encoding="utf-8")
            subprocess.run([str(_cpp_prism()), str(root), "--no-llm", "--stage", "inventory,classify,bmc",
                            "--out", out], cwd=ROOT, capture_output=True, text=True, timeout=600)
            rep = json.loads((Path(out) / "report.json").read_text(encoding="utf-8"))
        fns = {f["name"]: f for f in rep["functions"]}
        self.assertIn("set_ten", fns)
        self.assertTrue(fns["set_ten"]["body"].startswith("try {"))
        self.assertEqual(fns["set_ten"]["span"], [1, 6])
        inv = [f for st in rep["stages"] if st["name"] == "inventory" for f in st["findings"]]
        self.assertFalse([f for f in inv if f["cls"] == "PARSE-GAP"], inv)
        bmc = [f for st in rep["stages"] if st["name"] == "bmc" for f in st["findings"]]
        self.assertTrue(bmc)
        for f in bmc:
            self.assertEqual(f["status"], laws.NEEDS_HARNESS, f)


if __name__ == "__main__":
    unittest.main()
