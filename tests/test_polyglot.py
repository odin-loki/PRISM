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
from prism import scope
from prism.pipeline import STAGE_ORDER
from prism.taxonomy import CLASSES

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src" / "prism" / "polyglot.cpp").read_text(encoding="utf-8")
SCOPE_CPP = (ROOT / "src" / "prism" / "scope.cpp").read_text(encoding="utf-8")
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

    def test_executes_column_matches(self):
        """Law 9: Tool.executes (Python) == PgTool::executes (C++), tool by tool."""
        for check in pg.CHECKS:
            for tool in check.tools:
                m = re.search(r'\{\{?"' + re.escape(tool.name) + r'", \{', CPP)
                self.assertIsNotNone(m, tool.name)
                assert m is not None
                # The row runs to the next tool row or the check's install line.
                end = re.compile(r'\n {9}(?:"| \{")').search(CPP, m.end())
                row = CPP[m.start():end.start() if end else len(CPP)]
                self.assertEqual("/*executes=*/true" in row, tool.executes,
                                 f"{tool.name} executes drifted")
        self.assertEqual({t.name for c in pg.CHECKS for t in c.tools if t.executes},
                         {"perl", "cargo-clippy", "eslint"})

    def _cpp_row(self, name: str) -> str:
        m = re.search(r'\{\{?"' + re.escape(name) + r'", \{', CPP)
        self.assertIsNotNone(m, name)
        assert m is not None
        end = re.compile(r'\n {9}(?:"| \{")').search(CPP, m.end())
        return CPP[m.start():end.start() if end else len(CPP)]

    def test_benign_column_matches(self):
        """Tool.benign (Python) == PgTool::benign (C++), tool by tool, in order."""
        for check in pg.CHECKS:
            for tool in check.tools:
                row = self._cpp_row(tool.name)
                m = re.search(r'/\*benign=\*/\{(.*?)\}\}', row, re.S)
                cpp = re.findall(r'R"\((.*?)\)"', m.group(1)) if m else []
                self.assertEqual(tuple(cpp), tool.benign, f"{tool.name} benign drifted")
                for b in tool.benign:
                    re.compile(b)
        # The tools that print a success line have one.
        named = {t.name for c in pg.CHECKS for t in c.tools if t.benign}
        self.assertLessEqual({"prism-syntax", "ruff", "mypy", "ruby", "php", "perl", "gofmt"},
                             named)

    def test_builtin_scans_match(self):
        for cls, rx, msg in pg.BUILTIN_SCANS:
            self.assertIn(f'{{"{cls}", R"({rx})"', CPP.replace("\n     ", " "), cls)
            self.assertIn(f'"{msg}"', CPP, cls)

    def test_extension_and_skip_tables_match(self):
        for ext, lang in pg.LANG_EXTS.items():
            self.assertIn(f'{{"{ext}", "{lang}"}}', CPP)
        # One skip list for every stage: prism/scope.py == src/prism/scope.cpp.
        self.assertIs(pg.SKIP_DIRS, scope.SKIP_DIRS)
        cpp_dirs = re.search(
            r"skip_dirs\(\) \{\s*static const std::set<std::string> k = \{(.*?)\};",
            SCOPE_CPP, re.S)
        self.assertIsNotNone(cpp_dirs)
        assert cpp_dirs is not None
        self.assertEqual(set(re.findall(r'"([^"]+)"', cpp_dirs.group(1))), set(scope.SKIP_DIRS))
        for pre in scope.SKIP_PREFIXES:
            self.assertIn(f'name.starts_with("{pre}")', SCOPE_CPP)
        self.assertIn('#include "prism/scope.hpp"', CPP)
        self.assertIn("scope::skip_dir(", CPP)
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
            "k.ini": "aws = AKIAABCDEFGHIJKLMNOP\n",  # prism:allow
            "id_rsa.conf": "-----BEGIN RSA PRIVATE KEY-----\n",  # prism:allow
            "t.env": "TOKEN=ghp_" + "a" * 36 + "\n",
        })
        found = {f.cls for f in out if f.status == laws.FAILED}
        self.assertLessEqual({"SECRET-AWS-KEY", "SECRET-PRIVATE-KEY", "SECRET-GITHUB-TOKEN"},
                             found)

    def test_secrets_in_any_text_file_whatever_its_name(self):
        """id_rsa, key.pem, .npmrc, Dockerfile: no known extension, still scanned."""
        out = _run({
            "id_rsa": "-----BEGIN OPENSSH PRIVATE KEY-----\n",  # prism:allow
            "certs/key.pem": "-----BEGIN PRIVATE KEY-----\n",  # prism:allow
            ".npmrc": "//registry.npmjs.org/:_authToken=ghp_" + "b" * 36 + "\n",
            "Dockerfile": "ENV AWS_KEY=AKIA" + "ABCDEFGHIJKLMNOP\n",
            "blob.bin": "AKIA" + "ABCDEFGHIJKLMNOP\0\n",
        })
        hits = {(f.file, f.cls) for f in out if f.status == laws.FAILED}
        self.assertIn(("id_rsa", "SECRET-PRIVATE-KEY"), hits)
        self.assertIn(("certs/key.pem", "SECRET-PRIVATE-KEY"), hits)
        self.assertIn((".npmrc", "SECRET-GITHUB-TOKEN"), hits)
        self.assertIn(("Dockerfile", "SECRET-AWS-KEY"), hits)
        # A NUL byte in the first 8 KiB is binary: not a text file.
        self.assertNotIn("blob.bin", {f for f, _ in hits})
        # Language tools still go by extension: none of these is a language file.
        self.assertFalse([f for f in out if f.extra.get("check")])

    def test_text_sniff(self):
        d = _tree({"a": "text\n", "b.dat": "x\0y", "sub/c.txt": "ok\n"})
        try:
            (d / "big.txt").write_bytes(b"a" * (pg.MAX_FILE_BYTES + 1))
            names = [p.relative_to(d).as_posix() for p in pg.iter_text_files(d)]
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertEqual(names, ["a", "sub/c.txt"])
        self.assertIn("SNIFF_BYTES = 8192", CPP)
        self.assertIn("head.find('\\0') == std::string::npos", CPP)

    def test_allow_marker_skips_line(self):
        out = _run({"k.py": 'KEY = "AKIAABCDEFGHIJKLMNOP"  # prism:allow\n'})
        self.assertFalse([f for f in out if f.cls.startswith("SECRET-")])
        self.assertIn(f'"{pg.ALLOW_MARKER}"', CPP)

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


class TestExecToolsNeedAllowExec(unittest.TestCase):
    """Law 9: perl -c, cargo clippy and eslint run project code."""

    FILES = {"a.pl": "print 1;\n", "k/Cargo.toml": "[package]\n",
             "k/src/main.rs": "fn main(){}\n", "a.js": "let x = 1;\n"}

    def test_present_exec_tool_is_notrun_without_the_flag(self):
        with mock.patch("prism.polyglot.resolve_adapter", return_value="/usr/bin/true"), \
             mock.patch("prism.polyglot._run") as run:
            run.return_value = (0, "", False)
            out = _run(self.FILES)
        rows = {f.extra.get("check"): f for f in out if f.extra.get("check")}
        for group, tool in (("perl-syntax", "perl"), ("rust-lint", "cargo-clippy"),
                            ("javascript-lint", "eslint")):
            f = rows[group]
            self.assertEqual(f.status, laws.NOTRUN, group)
            self.assertEqual(f.extra["reason"], "executes-scanned-code")
            self.assertIn("--allow-exec", f.extra["install"])
            self.assertEqual(f.message, f"{group} ({tool}): executes code from the scanned "
                                        "tree; re-run with --allow-exec (only on code you trust)")
        argv = [c.args[0] for c in run.call_args_list]
        self.assertFalse(any("-c" in a and a[-1].endswith(".pl") for a in argv))
        self.assertFalse(any("clippy" in a for a in argv))
        self.assertFalse(any("unix" in a for a in argv))  # eslint --format unix

    def test_allow_exec_runs_them(self):
        with mock.patch("prism.polyglot.resolve_adapter", return_value="/usr/bin/true"), \
             mock.patch("prism.polyglot._run", return_value=(0, "", False)) as run:
            out = _run(self.FILES, allow_exec=True)
        self.assertFalse(any(f.extra.get("reason") == "executes-scanned-code" for f in out))
        argv = [c.args[0] for c in run.call_args_list]
        self.assertTrue(any("clippy" in a for a in argv))

    def test_mypy_never_reads_project_config(self):
        mypy = next(t for c in pg.CHECKS for t in c.tools if t.name == "mypy")
        self.assertIn("--config-file=", mypy.argv)
        self.assertFalse(mypy.executes)


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

    def test_ruff_named_rules_and_syntax_errors(self):
        """ruff >= 0.5 names syntax errors `invalid-syntax`; older prints SyntaxError."""
        out = self._parse("ruff", "/r/b.py:1:7: invalid-syntax: Expected a parameter\n"
                                  "/r/c.py:2:1: SyntaxError: Expected an expression\n")
        self.assertEqual([(f.file, f.line, f.extra["rule"]) for f in out],
                         [("b.py", 1, "invalid-syntax"), ("c.py", 2, "SyntaxError")])
        self.assertEqual(out[0].message, "ruff: Expected a parameter")

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


class TestOutputNotUnderstood(unittest.TestCase):
    """Output that parsed to nothing is ERROR, unless it is the tool's benign chatter."""

    def _ruff_only(self, text: str, rc: int = 1) -> list:
        def fake_run(cmd, timeout, cwd=None):
            return (rc, text, False) if cmd[1:2] == ["check"] else (0, "", False)

        def resolve(cfg, name, exes):
            return "/x/ruff" if name == "ruff" else None

        with mock.patch("prism.polyglot.resolve_adapter", side_effect=resolve), \
             mock.patch("prism.polyglot._run", side_effect=fake_run):
            out = _run({"a.py": "x = 1\n"})
        return [f for f in out if f.extra.get("check") == "python-lint"]

    def test_unparsed_output_is_error_not_unknown(self):
        rows = self._ruff_only("ruff: something new happened\n")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, laws.ERROR)
        self.assertEqual(rows[0].message,
                         "ruff: output not understood: ruff: something new happened")

    def test_benign_output_is_unknown(self):
        rows = self._ruff_only("All checks passed!\n", rc=0)
        self.assertEqual([r.status for r in rows], [laws.UNKNOWN])
        self.assertIn("no diagnostics (not a proof)", rows[0].message)

    def test_unexplained_output(self):
        ruby = next(t for c in pg.CHECKS for t in c.tools if t.name == "ruby")
        self.assertEqual(pg.unexplained_output(ruby, "Syntax OK\n\n"), "")
        self.assertEqual(pg.unexplained_output(ruby, "Syntax OK\nweird\n"), "weird")
        # A line the tool pattern matches but parse_output drops (a note) is understood.
        sc = next(t for c in pg.CHECKS for t in c.tools if t.name == "shellcheck")
        self.assertEqual(pg.parse_output(sc, "/r/a.sh:2:6: note: quote it [SC2086]\n", R, None,
                                         next(c for c in pg.CHECKS if c.group == "shell-lint")),
                         [])
        self.assertEqual(pg.unexplained_output(sc, "/r/a.sh:2:6: note: quote it [SC2086]\n"), "")
        self.assertIn('": output not understood: "', CPP)


# Real tools on this machine: a well-formed file must come back UNKNOWN (ran,
# nothing to say), never ERROR "output not understood" (a benign line missing
# from Tool.benign); a broken file must be FAILED with a file (the regex matches).
_REAL = {
    "python-lint": ("ruff", {"ok.py": "x = 1\n"}, {"bad.py": "def f(:\n    pass\n"}),
    "python-types": ("mypy", {"ok.py": "x: int = 1\n"}, {"t.py": 'x: int = "s"\n'}),
    "javascript-syntax": ("node", {"ok.js": "let x = 1;\n"}, {"bad.js": "let = ;\n"}),
    "typescript-types": ("tsc", {"ok.ts": "let y: number = 1;\n"},
                         {"bad.ts": "let y: number = 's';\n"}),
    "shell-syntax": ("bash", {"ok.sh": "echo hi\n"}, {"bad.sh": "if then\nfi\n"}),
    "shell-lint": ("shellcheck", {"ok.sh": "#!/bin/sh\necho hi\n"},
                   {"bad.sh": "#!/bin/sh\nx=1\n"}),
    "go-syntax": ("gofmt", {"ok.go": "package main\nfunc main(){}\n"},
                  {"bad.go": "package main\nfunc main( {\n"}),
    "ruby-syntax": ("ruby", {"ok.rb": "puts 1\n"}, {"bad.rb": "def (\n"}),
    "php-syntax": ("php", {"ok.php": "<?php echo 1;\n"}, {"bad.php": "<?php echo ;\n"}),
    "perl-syntax": ("perl", {"ok.pl": "print 1;\n"}, {"bad.pl": "my $x = ;\n"}),
    "yaml-lint": ("yamllint", {"ok.yml": "---\na: 1\n"}, {"bad.yml": "---\na: [1\n"}),
}


class TestRealToolsUnderstood(unittest.TestCase):
    def _rows(self, group: str, files: dict[str, str]) -> list:
        out = _run(files, allow_exec=True)
        return [f for f in out if f.extra.get("check") == group]

    def test_each_installed_tool(self):
        ran = 0
        for group, (exe, ok, bad) in _REAL.items():
            if not shutil.which(exe):
                continue
            ran += 1
            with self.subTest(group=group, tool=exe):
                good = self._rows(group, ok)
                self.assertEqual([f.status for f in good], [laws.UNKNOWN],
                                 [(f.status, f.message) for f in good])
                broken = self._rows(group, bad)
                self.assertTrue(any(f.status == laws.FAILED and f.file for f in broken),
                                [(f.status, f.message) for f in broken])
        if not ran:
            self.skipTest("no polyglot tools installed")


if __name__ == "__main__":
    unittest.main()
