"""False-positive corpus, its true-positive twins, and the parser forms.

python -m unittest tests.test_false_positives

testdata_fp/ is correct C/C++ written in the idioms the lints used to
misread: a free or unlock inside a block that returns, formats spelled as
macros, dangerous API names inside string literals, checked allocations,
RAII, methods and templates. run_lints must report nothing FAILED there.
testdata_tp/ holds the buggy variants of the same idioms: each must still
fire, on the right line. Both directories sit outside testdata/ so the
planted-bug counts there do not move. When a C++ binary is available
(PRISM_BIN, build/), both engines must report the same findings there and
on testdata/.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.checkers import run_lints
from prism.cparse import (
    extract_functions,
    extract_functions_from_text,
    iter_sources,
    parse_gap_findings,
    parse_gaps,
)

ROOT = Path(__file__).resolve().parents[1]
FP = ROOT / "testdata_fp"
TP = ROOT / "testdata_tp"


def _failed(root: Path) -> list[tuple[str, int, str]]:
    return sorted(
        (f.file, f.line or 0, f.cls)
        for f in run_lints(iter_sources(root), root)
        if f.status == laws.FAILED
    )


def _hits(name: str) -> set[tuple[int, str]]:
    return {
        (f.line or 0, f.cls)
        for f in run_lints([TP / name], TP)
        if f.status == laws.FAILED
    }


def _names(path: Path) -> list[str]:
    return [f.name for f in extract_functions(path, path.name)]


class TestFalsePositiveCorpus(unittest.TestCase):
    def test_corpus_is_big_enough(self):
        self.assertGreaterEqual(len(iter_sources(FP)), 20)

    def test_zero_failed_findings(self):
        self.assertEqual(_failed(FP), [])

    def test_every_corpus_body_is_attributed(self):
        for p in iter_sources(FP):
            self.assertEqual(parse_gaps(p), [], msg=p.name)
            self.assertTrue(extract_functions(p, p.name), msg=p.name)


class TestTruePositives(unittest.TestCase):
    """Each fix narrowed a checker; the buggy variant must still fire."""

    def test_uaf_paths(self):
        hits = _hits("uaf_paths.c")
        self.assertIn((13, "MEM-UAF"), hits)  # free in a block that falls through
        self.assertIn((25, "MEM-UAF"), hits)  # use inside the leaving block
        self.assertIn((44, "MEM-UAF"), hits)  # free; break; use after the loop
        self.assertIn((55, "MEM-DOUBLE-FREE"), hits)
        # A double free is MEM-DOUBLE-FREE only, not also MEM-UAF there.
        self.assertNotIn((55, "MEM-UAF"), hits)

    def test_double_unlock_on_fallthrough(self):
        self.assertIn((15, "LOCK-DOUBLE-UNLOCK"), _hits("lock_twice.c"))

    def test_leak_after_null_guard(self):
        hits = _hits("leaks.c")
        self.assertIn((12, "MEM-LEAK"), hits)
        self.assertIn((23, "RES-FD-LEAK"), hits)
        # The NULL / failed-open guard return is not the leak.
        self.assertNotIn((10, "MEM-LEAK"), hits)
        self.assertNotIn((21, "RES-FD-LEAK"), hits)

    def test_real_gets_and_formats_still_fire(self):
        hits = _hits("api_and_fmt.c")
        self.assertIn((11, "API-GETS"), hits)
        self.assertNotIn((10, "API-GETS"), hits)  # the string that names gets()
        self.assertIn((18, "FMT-STRING"), hits)  # printf(user)
        self.assertIn((19, "FMT-STRING"), hits)  # macro never #defined here
        self.assertNotIn((17, "FMT-STRING"), hits)  # printf(FMT, x)

    def test_unchecked_alloc_one_line_and_two(self):
        hits = _hits("unchecked_alloc.c")
        self.assertIn((6, "PTR-UNCHECKED-ALLOC"), hits)
        self.assertIn((12, "PTR-UNCHECKED-ALLOC"), hits)

    def test_uaf_in_methods_and_namespaces(self):
        found = [
            (f.function, f.line) for f in run_lints([TP / "methods_uaf.cpp"], TP)
            if f.cls == "MEM-UAF"
        ]
        self.assertIn(("W::inl", 11), found)
        self.assertIn(("W::go", 21), found)
        self.assertIn(("inner", 30), found)

    def test_strcpy_literal_measured_raw(self):
        hits = _hits("strcpy_escape.c")
        self.assertIn((7, "STR-OFF-BY-ONE"), hits)  # "ab\n" needs 4 bytes in b[3]
        self.assertNotIn((14, "STR-OFF-BY-ONE"), hits)  # and fits b[4]

    def test_parse_gap_is_reported_not_skipped(self):
        path = TP / "parse_gap.c"
        self.assertEqual([line for line, _ in parse_gaps(path)], [8])
        recs = parse_gap_findings(path, "parse_gap.c")
        self.assertEqual(len(recs), 1)
        rec = recs[0]
        self.assertEqual((rec.stage, rec.status, rec.cls, rec.line),
                         ("inventory", laws.NOTRUN, "PARSE-GAP", 8))
        self.assertIn("line 8", rec.message)
        # The macro-bodied definition does not swallow the next function.
        self.assertEqual(_names(path), ["after_macro"])


class TestParserForms(unittest.TestCase):
    def test_cxx_forms(self):
        self.assertEqual(_names(FP / "methods.cpp"), [
            "Buffer::Buffer", "Buffer::~Buffer", "Buffer::ok", "Buffer::size",
            "Buffer::fill", "use_buffer",
        ])
        self.assertEqual(_names(FP / "namespaced.cpp"),
                         ["clamp_to", "twice", "Point::operator==", "label"])
        self.assertEqual(_names(FP / "raii.cpp"),
                         ["add_locked", "make_squares", "greet"])

    def test_c_forms(self):
        self.assertEqual(_names(FP / "knr_style.c"), ["knr_add", "knr_first"])
        self.assertEqual(_names(FP / "attr_static_inline.c"),
                         ["attr_first", "sinl", "pick", "run_pick"])

    def test_new_forms_are_other_not_modelled_as_c(self):
        kinds = {f.name: f.kind for f in extract_functions(FP / "methods.cpp")}
        self.assertTrue(all(k == "OTHER" for n, k in kinds.items() if "::" in n))
        kinds = {f.name: f.kind for f in extract_functions(FP / "knr_style.c")}
        self.assertEqual(kinds, {"knr_add": "OTHER", "knr_first": "OTHER"})
        kinds = {f.name: f.kind for f in extract_functions(FP / "namespaced.cpp")}
        self.assertEqual(kinds["twice"], "OTHER")  # trailing return type
        self.assertEqual(kinds["clamp_to"], "OTHER")
        kinds = {f.name: f.kind for f in extract_functions(FP / "attr_static_inline.c")}
        self.assertEqual(kinds["pick"], "OTHER")  # returns a function pointer
        self.assertEqual(kinds["attr_first"], "SCALAR")

    def test_head_shapes(self):
        src = (
            "std::vector<int> make(int n) {\n    return {};\n}\n"
            "std::map<int, std::vector<int>> nested(void) {\n    return {};\n}\n"
            "Z3_ast Z3_API mk(Z3_context c) {\n    return 0;\n}\n"
            "W& W::operator=(const W& o) {\n    return *this;\n}\n"
            "W::W(int a) : v(a) {\n}\n"
            "struct S { int get() const { return 1; } S() : x(0) {} int x; };\n"
            "int body_char(void) {\n    return '}';\n}\n"
            "int after(void) {\n    return 1;\n}\n"
        )
        fns = extract_functions_from_text(src, "x.cpp")
        self.assertEqual([f.name for f in fns], [
            "make", "nested", "mk", "W::operator=", "W::W", "S::get", "S::S",
            "body_char", "after",
        ])
        self.assertTrue(all(f.kind == "OTHER" for f in fns[:7]))
        self.assertEqual(fns[7].span, (16, 18))  # `'}'` does not end the body


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    exe = "prism.exe" if os.name == "nt" else "prism"
    cands += [ROOT / d / exe for d in ("build", "build_wsl", "build/Release")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def _engine_rows(cmd: list[str], root: Path, out: Path,
                 ast_only: list | None = None) -> list[list]:
    """FAILED lint rows. The C++ engine's Clang-AST layer (roadmap 2.8, D8:
    C++ only) adds rows the frozen Python engine does not have; those are
    collected in `ast_only` and left out of the parity rows. An AST row that
    superseded a regex row on the same line and class stays in (same triple)."""
    subprocess.run([*cmd, str(root), "--no-llm", "--stage", "lints", "--out", str(out)],
                   cwd=ROOT, capture_output=True, text=True, timeout=600)
    rep = json.loads((out / "report.json").read_text(encoding="utf-8"))
    rows = []
    for st in rep["stages"]:
        if st["name"] != "lints":
            continue
        for f in st["findings"]:
            if f["status"] != laws.FAILED:
                continue
            row = [f["file"], f.get("line") or 0, f["cls"]]
            extra = f.get("extra") or {}
            if extra.get("engine") == "clang-ast" and extra.get("supersedes") != "regex":
                if ast_only is not None:
                    ast_only.append(row)
                continue
            rows.append(row)
    return sorted(rows)


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestEngineLintParity(unittest.TestCase):
    """Both engines report the same lint findings on both corpora and on
    the planted-bug corpus."""

    def test_same_findings(self):
        with tempfile.TemporaryDirectory(prefix="prism fp ") as td:
            for root in (FP, TP, ROOT / "testdata"):
                py = _engine_rows([sys.executable, "-m", "prism"], root,
                                  Path(td) / f"py_{root.name}")
                ast_only: list = []
                cpp = _engine_rows([str(_cpp_prism())], root,
                                   Path(td) / f"cpp_{root.name}", ast_only)
                self.assertEqual(py, cpp, msg=root.name)
                if root == FP:
                    self.assertEqual(py, [])
                    # The Clang-AST lints are held to the same corpus.
                    self.assertEqual(ast_only, [])


if __name__ == "__main__":
    unittest.main()
