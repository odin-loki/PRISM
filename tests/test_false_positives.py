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
        # (An ALL_CAPS `TEST(alloc) {` is parsed as a function now, see
        # TestRealWorldEvaluation; a lowercase macro head stays a gap.)
        self.assertEqual(_names(path), ["after_macro"])


class TestRealWorldEvaluation(unittest.TestCase):
    """Reduced reproducers of the false alarms and parser gaps measured on
    jsmn, tinyexpr and cJSON (docs/EVALUATION.md), and their buggy twins."""

    def test_export_macro_forms_are_parsed(self):
        self.assertEqual(_names(FP / "realworld_forms.c"),
                         ["jsmn_count", "version_string", "is_empty", "fixed_buffers",
                          "decimal_point", "same_type", "set_value"])
        fns = {f.name: f for f in extract_functions(FP / "realworld_forms.c", "f.c")}
        # A plain C function once the export macro is set aside.
        self.assertEqual(fns["is_empty"].kind, "POINTER")
        self.assertEqual(fns["version_string"].kind, "VOID")

    def test_zlib_knr_forms_are_parsed(self):
        # Before: none of these five bodies was a function or a gap (Law 7).
        self.assertEqual(_names(FP / "realworld_knr.c"),
                         ["fill_window", "deflate_stored", "once", "flush_block",
                          "after_ifdef"])

    def test_unreadable_knr_head_is_a_gap_not_silent(self):
        path = TP / "knr_gap.c"
        self.assertEqual(parse_gaps(path),
                         [(7, "char ZLIB_INTERNAL *strwinerror(error)")])
        self.assertEqual(_names(path), ["after_gap"])

    def test_macro_defined_test_body_is_parsed_and_linted(self):
        fns = {f.name: f for f in extract_functions(TP / "realworld_twins.c", "t.c")}
        self.assertEqual(fns["TEST_alloc"].kind, "OTHER")
        self.assertIn((35, "MEM-UAF"), _hits("realworld_twins.c"))

    def test_twins_still_fire(self):
        hits = _hits("realworld_twins.c")
        self.assertIn((11, "CTRL-FALLTHROUGH"), hits)  # unannotated, not the last arm
        self.assertIn((19, "MEM-VLA-SIZE"), hits)  # buf[n]
        self.assertIn((27, "MEM-VLA-SIZE"), hits)  # buf[LEN], LEN a local
        self.assertIn((41, "PTR-NULL-DEREF"), hits)  # if (!n) return n->type;
        self.assertIn((42, "PTR-NULL-DEREF"), hits)  # braced then-branch
        self.assertIn((50, "INT-BOOL-AS-BIT"), hits)  # (a < b) | (c < d)
        self.assertIn((51, "INT-BOOL-AS-BIT"), hits)  # a == 1 & b == 2
        self.assertIn((62, "CTRL-MISSING-RETURN"), hits)  # #else arm does not return
        self.assertIn((72, "STR-NULL-ARG"), hits)  # CVE-2024-31755 shape
        self.assertIn((79, "STR-NULL-MEMBER"), hits)  # CVE-2023-50472 shape
        self.assertNotIn((88, "STR-NULL-MEMBER"), hits)  # member null-checked
        self.assertIn((94, "UNINIT-BRANCH"), hits)  # err never assigned

    def test_interval_leaves_floating_point_alone(self):
        from prism.interval import run_interval
        src = (
            "double add(double a, double b) { return a + b; }\n"
            "double scale(int n) { double x = n; return x * x + 1; }\n"
            "int iadd(int a, int b) { return a + b; }\n"
            # tinyexpr: a multi-word unsigned local and a ULL literal
            "int ncr(int n, int r) { unsigned long int un = n, ur = r, i = 1;"
            " return (int)(un - ur + i); }\n"
            "static unsigned long long st = 1;\n"
            "unsigned rnd(unsigned m) { st = st * 6364136223846793005ULL + 1ULL; return m; }\n"
            "unsigned umul(unsigned a) { return a * 16U; }\n"
        )
        fns = extract_functions_from_text(src, "f.c")
        got = {f.function: f.cls for f in run_interval(fns) if f.status == laws.FAILED}
        self.assertEqual(got, {"iadd": "INT-SIGNED-OVF"})

    def test_fuzz_and_replay_do_not_run_doubles_as_ints(self):
        from prism.fuse import _fuse_one
        fn = extract_functions_from_text(
            "double sum1(double a) { return a * 2; }\n", "f.c")[0]
        f = _fuse_one(fn, [], ROOT, budget=0.2, iters=4, rounds=1)
        self.assertEqual(f.status, laws.NEEDS_HARNESS)
        self.assertIn("float/double unencoded", f.message)

    def test_missing_header_is_notrun_not_a_compiler_error(self):
        from prism.adapters import _host_compilers, run_compiler
        from prism.config import Config
        if not _host_compilers():
            self.skipTest("no gcc/clang")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "t.c").write_text('#include "unity_fixture.h"\nint f(void) { return 1; }\n')
            cfg = Config(root=root)
            rows = run_compiler([root / "t.c"], cfg)
        self.assertTrue(rows)
        self.assertTrue(all(f.status == laws.NOTRUN for f in rows), rows)
        self.assertIn("unity_fixture.h", rows[0].message)

    def test_typedef_pointer_local_is_unencoded_not_error(self):
        # cJSON tests: `cJSON *root = cJSON_CreateObject();` in a no-parameter
        # test function was concolic ERROR "trailing tokens".
        from prism.concolic import run_concolic
        fns = extract_functions_from_text(
            "typedef struct cJSON cJSON;\ncJSON *make(void);\nvoid use(cJSON *p);\n"
            "void t4(void) {\n    cJSON *root = make();\n    use(root);\n}\n", "t.c")
        rows = [f for f in run_concolic(fns) if f.function == "t4"]
        self.assertEqual([f.status for f in rows], [laws.NEEDS_HARNESS])
        self.assertIn("typedef local unencoded", rows[0].message)

    def test_cxx_qualified_name_is_unencoded_not_error(self):
        # Catch2 isFalseTest: `flags & ResultDisposition::FalseTest` was
        # concolic ERROR "expected ) got :".
        from prism.concolic import run_concolic
        fns = extract_functions_from_text(
            "namespace RD { enum Flags { FalseTest = 4 }; }\n"
            "bool is_false(int flags) { return (flags & RD::FalseTest) != 0; }\n", "t.cpp")
        rows = [f for f in run_concolic(fns) if f.function == "is_false"]
        self.assertEqual([f.status for f in rows], [laws.NEEDS_HARNESS])

    def test_concrete_prep_keeps_hash_in_strings(self):
        from prism.concrete import _PREP_RE
        body = 'test(t, "issue #22");\n#ifdef X\nx = 1;\n#endif\n'
        self.assertEqual(_PREP_RE.sub(" ", body), 'test(t, "issue #22");\n \nx = 1;\n \n')


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
                # The Clang-AST layer runs clang once per unit: use every core.
                cpp = _engine_rows([str(_cpp_prism()), "--jobs", str(os.cpu_count() or 2)], root,
                                   Path(td) / f"cpp_{root.name}", ast_only)
                self.assertEqual(py, cpp, msg=root.name)
                if root == FP:
                    self.assertEqual(py, [])
                    # The Clang-AST lints are held to the same corpus.
                    self.assertEqual(ast_only, [])


if __name__ == "__main__":
    unittest.main()
