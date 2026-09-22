"""Polyglot stage: every language, not just C/C++. python -m unittest tests.test_polyglot

Laws: a missing tool is NOTRUN (with install), a silent tool is UNKNOWN (not
a proof), a diagnostic is FAILED. The C++ engine (src/prism/polyglot.cpp)
must carry the same tables as the Python engine (prism/polyglot.py).
"""

from __future__ import annotations

import re
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.config import Config
from prism import polyglot as pg
from prism.pipeline import STAGE_ORDER
from prism.taxonomy import CLASSES

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src" / "prism" / "polyglot.cpp").read_text(encoding="utf-8")
R = ROOT / "testdata"  # an existing directory; parser paths are made relative to it


def _tree(files: dict[str, str]) -> Path:
    d = Path(tempfile.mkdtemp(prefix="prism_pg_test_"))
    for rel, text in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return d


def _run(files: dict[str, str], **cfg_kw) -> list:
    d = _tree(files)
    try:
        return pg.run_polyglot(d, Config(root=d, jobs=2, **cfg_kw))
    finally:
        shutil.rmtree(d, ignore_errors=True)


class TestCppParity(unittest.TestCase):
    def test_syntax_helper_is_byte_identical(self):
        m = re.search(r'SYNTAX_HELPER = R"PRISMPY\((.*?)\)PRISMPY";', CPP, re.S)
        self.assertIsNotNone(m, "polyglot.cpp must embed SYNTAX_HELPER")
        self.assertEqual(m.group(1), pg.SYNTAX_HELPER)

    def test_check_groups_in_same_order(self):
        cpp_groups = re.findall(r'^\s+\{"([a-z]+-[a-z]+)", \{"', CPP, re.M)
        self.assertEqual(cpp_groups, [c.group for c in pg.CHECKS])

    def test_every_tool_pattern_argv_and_install_match(self):
        for check in pg.CHECKS:
            self.assertIn(f'"{check.install}"', CPP, check.group)
            for tool in check.tools:
                pat = tool.pattern if tool.pattern != pg._GCC else None
                if pat is not None:
                    self.assertIn(f'R"({pat})"', CPP, f"{tool.name} pattern drifted")
                argv = ", ".join(f'"{a}"' for a in tool.argv)
                self.assertIn(argv, CPP.replace("\n            ", " "),
                              f"{tool.name} argv drifted")
                exes = ", ".join(f'"{e}"' for e in tool.exes)
                self.assertIn(f'{{"{tool.name}", {{{exes}}}', CPP, f"{tool.name} exes drifted")
        self.assertIn(f'GCC_PATTERN =\n    R"({pg._GCC})";', CPP)

    def test_builtin_scans_match(self):
        for cls, rx, msg in pg.BUILTIN_SCANS:
            self.assertIn(f'{{"{cls}", R"({rx})"', CPP.replace("\n     ", " "), cls)
            self.assertIn(f'"{msg}"', CPP, cls)

    def test_extension_and_skip_tables_match(self):
        for ext, lang in pg.LANG_EXTS.items():
            self.assertIn(f'{{"{ext}", "{lang}"}}', CPP)
        for d in pg.SKIP_DIRS:
            self.assertIn(f'"{d}"', CPP)
        for e in pg.TEXT_ONLY_EXTS | pg.C_FAMILY_EXTS:
            self.assertIn(f'"{e}"', CPP)

    def test_stage_is_in_both_orders(self):
        self.assertIn("polyglot", STAGE_ORDER)
        hpp = (ROOT / "include" / "prism" / "pipeline.hpp").read_text(encoding="utf-8")
        self.assertIn('"optional", "polyglot", "esbmc"', hpp)

    def test_taxonomy_classes_in_both_engines(self):
        ids = {c["id"] for c in CLASSES}
        tax = (ROOT / "src" / "prism" / "taxonomy.cpp").read_text(encoding="utf-8")
        for cid in ["SYNTAX-ERROR", "TYPE-ERROR", "LANG-LINT", "VCS-CONFLICT-MARKER",
                    *[c for c, _, _ in pg.BUILTIN_SCANS]]:
            self.assertIn(cid, ids)
            self.assertIn(f'{{"{cid}", ', tax)


class TestBuiltinScan(unittest.TestCase):
    def test_conflict_marker_in_c(self):
        out = _run({"a.c": "<<<<<<< HEAD\nint x;\n=======\nint y;\n>>>>>>> b\n"})
        hits = [f for f in out if f.cls == "VCS-CONFLICT-MARKER"]
        self.assertEqual([f.line for f in hits], [1, 5])
        self.assertTrue(all(f.status == laws.FAILED for f in hits))

    def test_secrets(self):
        out = _run({
            "k.ini": "aws = AKIAABCDEFGHIJKLMNOP\n",
            "id_rsa.conf": "-----BEGIN RSA PRIVATE KEY-----\n",
            "t.env": "TOKEN=ghp_" + "a" * 36 + "\n",
        })
        found = {f.cls for f in out if f.status == laws.FAILED}
        self.assertLessEqual({"SECRET-AWS-KEY", "SECRET-PRIVATE-KEY", "SECRET-GITHUB-TOKEN"},
                             found)

    def test_nothing_found_is_clean_not_proof(self):
        out = pg.builtin_scan([], Path("."))
        self.assertEqual(out[0].status, laws.CLEAN)
        self.assertIn("not a proof", out[0].message)
        self.assertFalse(laws.is_proof(out[0].status))

    def test_skip_dirs(self):
        d = _tree({"node_modules/x.js": "<<<<<<< a\n", "src/y.py": "x = 1\n"})
        try:
            names = [p.relative_to(d).as_posix() for p in pg.iter_polyglot_sources(d)]
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertEqual(names, ["src/y.py"])


class TestSyntaxHelper(unittest.TestCase):
    """python-syntax always has an interpreter: the one running PRISM."""

    def test_python_json_toml_errors(self):
        out = _run({
            "bad.py": "def f(:\n    pass\n",
            "bad.json": '{"a": 1,}\n',
            "bad.toml": "a = \n",
            "ok.py": "x = 1\n",
        })
        syn = {f.file: f for f in out if f.cls == "SYNTAX-ERROR"}
        self.assertIn("bad.py", syn)
        self.assertEqual(syn["bad.py"].line, 1)
        self.assertIn("bad.json", syn)
        self.assertIn("bad.toml", syn)
        self.assertNotIn("ok.py", syn)
        for f in syn.values():
            self.assertEqual(f.status, laws.FAILED)
            self.assertEqual(f.stage, "polyglot")

    def test_all_ok_is_unknown_not_clean(self):
        with mock.patch("prism.polyglot.resolve_adapter", return_value=None):
            out = _run({"ok.py": "x = 1\n"})
        rows = [f for f in out if f.extra.get("check") == "python-syntax"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, laws.UNKNOWN)
        self.assertIn("not a proof", rows[0].message)


@unittest.skipUnless(shutil.which("mypy"), "mypy not installed")
class TestBrokenFileDoesNotBlindTypeChecker(unittest.TestCase):
    def test_type_error_still_found_next_to_syntax_error(self):
        out = _run({"bad.py": "def f(:\n    pass\n", "typed.py": 'x: int = "s"\n'})
        types = [f for f in out if f.cls == "TYPE-ERROR"]
        self.assertEqual([f.file for f in types], ["typed.py"])
        self.assertIn("bad.py", {f.file for f in out if f.cls == "SYNTAX-ERROR"})


class TestMissingToolsAreNotrun(unittest.TestCase):
    def test_every_language_without_tools_is_notrun(self):
        files = {
            "a.js": "let x = 1;\n", "b.ts": "let y: number = 1;\n", "c.sh": "echo hi\n",
            "d.go": "package main\n", "e.rb": "x = 1\n", "f.php": "<?php echo 1;\n",
            "g.pl": "print 1;\n", "h.lua": "print(1)\n", "i.yml": "a: 1\n",
            "j.py": "x = 1\n", "k/Cargo.toml": "[package]\n", "k/src/main.rs": "fn main(){}\n",
        }
        with mock.patch("prism.polyglot.resolve_adapter", return_value=None):
            out = _run(files)
        by_check = {}
        for f in out:
            by_check.setdefault(f.extra.get("check"), []).append(f)
        for check in pg.CHECKS:
            if check.group == "python-syntax":
                continue  # sys.executable is always present
            rows = by_check.get(check.group)
            self.assertTrue(rows, f"{check.group} wrote nothing (quiet skip)")
            self.assertTrue(all(r.status == laws.NOTRUN for r in rows), check.group)
            self.assertTrue(all(r.extra.get("install") for r in rows), check.group)

    def test_language_absent_writes_nothing(self):
        with mock.patch("prism.polyglot.resolve_adapter", return_value=None):
            out = _run({"a.c": "int main(void){return 0;}\n"})
        self.assertEqual([f.extra.get("check") for f in out if f.extra.get("check")], [])

    def test_empty_tree_is_unknown(self):
        out = _run({})
        self.assertEqual(out[0].status, laws.UNKNOWN)


class TestParsers(unittest.TestCase):
    """Parse real tool output shapes without needing the tools."""

    def _tool(self, name: str) -> tuple:
        for c in pg.CHECKS:
            for t in c.tools:
                if t.name == name:
                    return t, c
        raise AssertionError(name)

    def _parse(self, name: str, text: str) -> list:
        t, c = self._tool(name)
        return pg.parse_output(t, text.replace("/r/", f"{R}/"), R, None, c)

    def test_ruff(self):
        out = self._parse("ruff", "/r/a.py:1:8: F401 [*] `os` imported but unused\n")
        self.assertEqual((out[0].file, out[0].line, out[0].extra["rule"]), ("a.py", 1, "F401"))
        self.assertEqual(out[0].message, "ruff: `os` imported but unused")
        self.assertEqual(out[0].cls, "LANG-LINT")

    def test_mypy_skips_notes(self):
        out = self._parse("mypy", "/r/a.py:2:10: error: Incompatible types  [assignment]\n"
                                  "/r/a.py:2:10: note: see docs\n")
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].cls, "TYPE-ERROR")
        self.assertEqual(out[0].extra["rule"], "assignment")

    def test_node(self):
        out = self._parse("node", "/r/a.js:3\n  let = ;\n      ^\n\nSyntaxError: Unexpected token ';'\n")
        self.assertEqual((out[0].file, out[0].line), ("a.js", 3))
        self.assertIn("SyntaxError", out[0].message)

    def test_tsc(self):
        out = self._parse("tsc", "/r/t.ts(1,7): error TS2322: Type 'string' is not assignable.\n")
        self.assertEqual((out[0].line, out[0].extra["rule"], out[0].cls), (1, "TS2322", "TYPE-ERROR"))

    def test_bash_php_perl_ruby(self):
        self.assertEqual(self._parse("bash", "/r/a.sh: line 4: syntax error near `fi'\n")[0].line, 4)
        php = self._parse("php", "PHP Parse error:  syntax error, unexpected ';' in /r/a.php on line 2\n")
        self.assertEqual((php[0].file, php[0].line), ("a.php", 2))
        perl = self._parse("perl", "syntax error at /r/a.pl line 1, near \"= ;\"\n")
        self.assertEqual((perl[0].file, perl[0].line), ("a.pl", 1))
        rb = self._parse("ruby", "/usr/bin/ruby: /r/a.rb:1: syntax error, unexpected end-of-input\n")
        self.assertEqual((rb[0].file, rb[0].line, rb[0].cls), ("a.rb", 1, "SYNTAX-ERROR"))
        warn = self._parse("ruby", "/r/a.rb:3: warning: assigned but unused variable - y\n")
        self.assertEqual(warn[0].cls, "LANG-LINT")

    def test_shellcheck_gofmt_clippy_yamllint(self):
        sc = self._parse("shellcheck", "/r/a.sh:2:6: warning: foo is unused [SC2034]\n")
        self.assertEqual((sc[0].line, sc[0].extra["severity"]), (2, "warning"))
        go = self._parse("gofmt", "/r/a.go:2:11: expected ')', found '{'\n")
        self.assertEqual((go[0].file, go[0].cls), ("a.go", "SYNTAX-ERROR"))
        t, c = self._tool("cargo-clippy")
        cl = pg.parse_output(t, "src/main.rs:1:17: warning: unused variable: `x`\n",
                             R, R / "k", c)
        self.assertEqual(cl[0].file, "k/src/main.rs")
        ym = self._parse("yamllint", "/r/a.yml:1:9: [error] syntax error: expected ',' (syntax)\n")
        self.assertEqual((ym[0].line, ym[0].extra["rule"]), (1, "syntax"))

    def test_eslint_without_config_is_notrun(self):
        t, _ = self._tool("eslint")
        self.assertRegex("ESLint couldn't find an eslint.config.(js|mjs|cjs) file.",
                         re.compile(t.unconfigured, re.I))


if __name__ == "__main__":
    unittest.main()
