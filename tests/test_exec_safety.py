"""Law 9: running PRISM on a hostile tree must not run that tree's code.

python -m unittest tests.test_exec_safety

A planted tree carries code that would write a sentinel file if PRISM ever
executed it: a ``void cleanup_everything(void)`` (the sanitize stage used to
call any zero-argument function), a constructor that calls it when any
binary built from the file starts (the fuzz harness), a Perl BEGIN block (``perl -c`` runs it),
a Cargo ``build.rs`` (``cargo clippy`` builds it), an ``eslint.config.js``
(eslint loads it) and a mypy plugin named by the project's ``mypy.ini``.
Without ``--allow-exec`` a full run creates no sentinel and reports NOTRUN
with the ``--allow-exec`` hint. With it, sanitize still never calls the
hostile ``void(void)``: only a ``// prism: run`` function is called.
The C++ side is covered by tests/cpp/test_main.cpp ("Law 9" cases).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prism import laws, sandbox
from prism import polyglot as pg
from prism.config import Config
from prism.pipeline import EXEC_STAGES, exec_gate_note
from prism.sanitize import _opted_in_callable, marked_run, run_sanitize

ROOT = Path(__file__).resolve().parents[1]
PIPELINE_CPP = (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "src" / "prism" / "main.cpp").read_text(encoding="utf-8")
SANDBOX_CPP = (ROOT / "src" / "prism" / "sandbox.cpp").read_text(encoding="utf-8")
SANDBOX_HPP = (ROOT / "include" / "prism" / "sandbox.hpp").read_text(encoding="utf-8")

# Slow stages this test does not need (the gates under test all stay on).
_SKIP = "esbmc,dafny,cppcheck,pbsd,ltl,wp,contracts,harness,lints,interval,taint,thread"


def _hostile_tree(root: Path, sentinel_dir: Path) -> None:
    s = sentinel_dir.as_posix()
    files = {
        "hostile.c": (
            "#include <stdio.h>\n"
            "void cleanup_everything(void) {\n"
            f'    FILE *f = fopen("{s}/C_SENTINEL", "w");\n'
            "    if (f) fclose(f);\n"
            "}\n"
            "__attribute__((constructor)) static void on_load(void) { cleanup_everything(); }\n"
            "int ident(int x) { return x; }\n"
            "int pick_a(int x) { cleanup_everything(); return x; }\n"
            "int pick_b(int x) { return x + 1; }\n"
            "// requires: x > 0\n"
            "// ensures: result > 0\n"
            "int keep(int x) { cleanup_everything(); return x; }\n"
        ),
        "evil.pl": (
            f"BEGIN {{ open(my $f, '>', '{s}/PERL_SENTINEL'); close($f); }}\n"
            "print 1;\n"
        ),
        "crate/Cargo.toml": (
            '[package]\nname = "evil"\nversion = "0.1.0"\nedition = "2021"\nbuild = "build.rs"\n'
        ),
        "crate/build.rs": (
            f'fn main() {{ std::fs::write("{s}/CARGO_SENTINEL", "x").unwrap(); }}\n'
        ),
        "crate/src/main.rs": "fn main() {}\n",
        "eslint.config.js": (
            f'require("fs").writeFileSync("{s}/ESLINT_SENTINEL", "x");\n'
            "module.exports = [];\n"
        ),
        "app.js": "let x = 1;\n",
        "mypy.ini": f"[mypy]\nplugins = {s}/evil_plugin.py\n",
        "mod.py": "x: int = 1\n",
    }
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    (sentinel_dir / "evil_plugin.py").write_text(
        f'open("{s}/MYPY_SENTINEL", "w").write("x")\n'
        "def plugin(version):\n"
        "    from mypy.plugin import Plugin\n"
        "    return Plugin\n",
        encoding="utf-8",
    )


def _sentinels(d: Path) -> list[str]:
    return sorted(p.name for p in d.glob("*_SENTINEL"))


def _run_prism(tree: Path, out: Path, *extra: str) -> dict:
    cmd = [sys.executable, "-m", "prism", str(tree), "--no-llm", "--out", str(out),
           "--fuzz-budget", "0.5", "--fuzz-iters", "16", "--jobs", "2", *extra]
    env = dict(os.environ, PYTHONPATH=str(ROOT))
    # cwd = the hostile tree: tools that read project config from the cwd
    # (mypy.ini plugins, eslint.config.js) see it, as they would in real use.
    subprocess.run(cmd, cwd=tree, env=env, capture_output=True, text=True, timeout=900)
    return json.loads((out / "report.json").read_text(encoding="utf-8"))


def _is_exec_notrun(f: dict) -> bool:
    extra = f.get("extra") or {}
    return (
        f.get("status") == laws.NOTRUN
        and extra.get("reason") == sandbox.EXEC_REASON
        and extra.get("install") == sandbox.EXEC_INSTALL
        and "--allow-exec" in (f.get("message") or "")
    )


class TestHostileTreeWithoutAllowExec(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.td = Path(tempfile.mkdtemp(prefix="prism_hostile_"))
        cls.tree = cls.td / "tree"
        cls.sentinel_dir = cls.td / "sentinels"
        cls.tree.mkdir()
        cls.sentinel_dir.mkdir()
        _hostile_tree(cls.tree, cls.sentinel_dir)
        cls.report = _run_prism(cls.tree, cls.td / "out", "--skip", _SKIP)
        cls.stages = {s["name"]: s for s in cls.report["stages"]}

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.td, ignore_errors=True)

    def test_no_sentinel_was_written(self):
        self.assertEqual(_sentinels(self.sentinel_dir), [])

    def test_sanitize_is_notrun_with_hint(self):
        st = self.stages["sanitize"]
        self.assertEqual(st["status"], "NOTRUN")
        self.assertTrue(all(_is_exec_notrun(f) for f in st["findings"]))
        self.assertIn("--allow-exec", st["install"])

    def test_diff_is_notrun_with_hint(self):
        st = self.stages["diff"]
        self.assertEqual(st["status"], "NOTRUN")
        self.assertTrue(st["findings"])
        self.assertTrue(all(_is_exec_notrun(f) for f in st["findings"]))

    def test_fuzz_keeps_concrete_oracle_and_writes_the_gap(self):
        st = self.stages["fuzz"]
        self.assertEqual(st["status"], "ok")
        self.assertTrue(any(_is_exec_notrun(f) for f in st["findings"]))
        for f in st["findings"]:
            self.assertNotEqual((f.get("extra") or {}).get("oracle"), "binary")

    def test_exec_tools_in_polyglot_are_notrun(self):
        rows = self.stages["polyglot"]["findings"]
        by_check = {(f.get("extra") or {}).get("check"): f for f in rows}
        for group, exe in (("perl-syntax", "perl"), ("rust-lint", "cargo"),
                           ("javascript-lint", "eslint")):
            f = by_check.get(group)
            self.assertIsNotNone(f, f"{group} wrote nothing (quiet skip)")
            if shutil.which(exe):
                self.assertTrue(_is_exec_notrun(f), f"{group}: {f}")
            else:
                self.assertEqual(f["status"], laws.NOTRUN)
            self.assertNotIn(f["status"], {laws.CLEAN, laws.UNKNOWN})

    def test_nothing_held_back_is_reported_clean(self):
        for st in self.report["stages"]:
            for f in st["findings"]:
                if _is_exec_notrun(f):
                    self.assertFalse(laws.is_proof(f["status"]))


class TestHostileTreeWithAllowExec(unittest.TestCase):
    """--allow-exec: sanitize still calls only `// prism: run` functions."""

    def test_sanitize_never_calls_the_hostile_void_void(self):
        with tempfile.TemporaryDirectory(prefix="prism_hostile_ae_") as td:
            tdp = Path(td)
            tree, sdir = tdp / "tree", tdp / "sentinels"
            tree.mkdir()
            sdir.mkdir()
            _hostile_tree(tree, sdir)
            report = _run_prism(tree, tdp / "out", "--allow-exec", "--stage",
                                "inventory,classify,sanitize")
            self.assertEqual(_sentinels(sdir), [])
            st = {s["name"]: s for s in report["stages"]}["sanitize"]
            for f in st["findings"]:
                self.assertNotEqual(f.get("function"), "cleanup_everything")
                self.assertNotEqual(f["status"], laws.CLEAN)

    @unittest.skipUnless(shutil.which("gcc") or shutil.which("clang"), "no C compiler")
    def test_opted_in_function_runs_in_the_sandbox(self):
        with tempfile.TemporaryDirectory(prefix="prism_optin_") as td:
            src = Path(td) / "ok.c"
            src.write_text(
                "void cleanup_everything(void) { }\n"
                "// prism: run\n"
                "int selftest(void) { return 0; }\n",
                encoding="utf-8",
            )
            out = run_sanitize([src], Config(allow_exec=True, jobs=1))
            ran = [f for f in out if f.file]
            for f in ran:
                self.assertEqual(f.function, "selftest")
                self.assertIn(f.extra.get("sandbox"), {"bwrap", "rlimits-only", "none"})
            if not ran:  # no usable sanitizer here: NOTRUN, never CLEAN
                self.assertTrue(all(f.status == laws.NOTRUN for f in out))


class TestOptInRule(unittest.TestCase):
    def _file(self, text: str) -> Path:
        td = Path(tempfile.mkdtemp(prefix="prism_mark_"))
        self.addCleanup(shutil.rmtree, td, True)
        p = td / "m.c"
        p.write_text(text, encoding="utf-8")
        return p

    def test_marker_selection(self):
        p = self._file(
            "static int helper(void) { return 0; }\n"
            "void cleanup_everything(void) { }\n"
            "\n"
            "/* entry point for the sanitizer run\n"
            " * prism: run */\n"
            "int\n"
            "selftest(void)\n"
            "{\n"
            "    return helper();\n"
            "}\n"
            "void trailing(void) { } // prism: run\n"
        )
        self.assertEqual(_opted_in_callable(p), "selftest")
        self.assertIsNone(_opted_in_callable(self._file(
            "// prism: run\nstatic void only_static(void) { }\n")))
        self.assertIsNone(_opted_in_callable(self._file(
            "// prism: run\nint takes(int x) { return x; }\n")))
        self.assertIsNone(_opted_in_callable(self._file(
            "void cleanup_everything(void) { }\n")))

    def test_marked_run_matches_cpp_cases(self):
        self.assertFalse(marked_run(["// prism: run", "", "void gap(void) {}"], 3))
        self.assertTrue(marked_run(["// prism:run", "void f(void) {}"], 2))
        self.assertTrue(marked_run(["void f(void) { } /* prism: run */"], 1))
        self.assertFalse(marked_run(["// prism: running", "void f(void) {}"], 2))

    def test_default_config_is_notrun(self):
        p = self._file("void cleanup_everything(void) { }\n")
        out = run_sanitize([p], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertEqual(out[0].extra["reason"], sandbox.EXEC_REASON)


class TestVendoredToolsNotFromTheScannedTree(unittest.TestCase):
    """A planted third_party/SOURCES.md + third_party/esbmc/bin/esbmc is not run."""

    def test_planted_vendor_binary_is_refused(self):
        from unittest import mock

        from prism.config import path_within, resolve_adapter

        with tempfile.TemporaryDirectory(prefix="prism_vendor_") as td:
            tree = Path(td)
            (tree / "third_party").mkdir()
            (tree / "third_party" / "SOURCES.md").write_text("x\n", encoding="utf-8")
            fake = tree / "third_party" / "esbmc" / "bin" / "esbmc"
            fake.parent.mkdir(parents=True)
            fake.write_text("#!/bin/sh\ntouch SENTINEL\n", encoding="utf-8")
            fake.chmod(0o755)
            self.assertTrue(path_within(tree / "third_party", tree))
            self.assertFalse(path_within(ROOT, tree))
            with mock.patch("prism.config.repo_root", return_value=tree), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                self.assertIsNone(resolve_adapter(Config(root=tree), "esbmc", ("esbmc",)))
                trusted = resolve_adapter(Config(root=tree, allow_exec=True), "esbmc", ("esbmc",))
            self.assertEqual(trusted, str(fake))
        cpp = (ROOT / "src" / "prism" / "config.cpp").read_text(encoding="utf-8")
        self.assertIn("cfg.allow_exec || !path_within(p, cfg.root)", cpp)
        self.assertNotIn("std::vector<fs::path> starts{cfg.root", cpp)


class TestPolicyAndSandbox(unittest.TestCase):
    def test_policy_defaults_to_deny_and_restores(self):
        self.assertFalse(Config().allow_exec)
        self.assertFalse(sandbox.allowed())
        with sandbox.policy(True):
            self.assertTrue(sandbox.allowed())
        self.assertFalse(sandbox.allowed())

    def test_exec_message_is_the_cpp_text(self):
        self.assertEqual(
            sandbox.exec_message("fuzz"),
            "fuzz: executes code from the scanned tree; "
            "re-run with --allow-exec (only on code you trust)",
        )
        self.assertIn(f'EXEC_INSTALL = "{sandbox.EXEC_INSTALL}"', SANDBOX_HPP)
        self.assertIn(f'EXEC_REASON = "{sandbox.EXEC_REASON}"', SANDBOX_HPP)
        self.assertIn('": executes code from the scanned tree; "', SANDBOX_CPP)

    def test_bwrap_argv_matches_cpp(self):
        py = sandbox._bwrap_base("bwrap", Path("/s"))
        cpp = re.search(r"std::vector<std::string> out\{(.*?)\};", SANDBOX_CPP, re.S)
        self.assertIsNotNone(cpp)
        assert cpp is not None
        # C++ literals: everything but the bwrap path and the two scratch args.
        literals = re.findall(r'"([^"]*)"', cpp.group(1))
        self.assertEqual(literals, [*py[1:11], *py[13:], "--"])
        self.assertEqual(py[11:13], ["/s", "/s"])
        for n in ("LIMIT_NOFILE", "LIMIT_FSIZE_BYTES", "LIMIT_AS_BYTES"):
            self.assertIn(n, SANDBOX_HPP)
        self.assertIn("2ULL * 1024 * 1024 * 1024", SANDBOX_HPP)
        self.assertEqual(sandbox.LIMIT_AS_BYTES, 2 * 1024 * 1024 * 1024)

    @unittest.skipUnless(sandbox.sandbox_kind() == "bwrap", "bubblewrap not usable here")
    def test_bwrap_jail_cannot_write_outside_scratch(self):
        with tempfile.TemporaryDirectory(prefix="prism_jail_") as td, \
                tempfile.TemporaryDirectory(prefix="prism_jail_out_", dir=str(ROOT)) as outside:
            scratch = Path(td)
            target = Path(outside) / "ESCAPED"
            r = sandbox.run_binary(
                ["sh", "-c", f'echo in > "{scratch}/ok"; echo out > "{target}"'],
                scratch=scratch, timeout=10, text=True,
            )
            self.assertTrue((scratch / "ok").exists(), r.stderr)
            self.assertFalse(target.exists())

    @unittest.skipUnless(sys.platform != "win32", "rlimits are POSIX")
    def test_rlimits_apply(self):
        with tempfile.TemporaryDirectory(prefix="prism_rl_") as td:
            r = sandbox.run_binary(["sh", "-c", "ulimit -n; ulimit -v"], scratch=Path(td),
                                   timeout=10, text=True)
            lines = (r.stdout or "").split()
            self.assertEqual(lines[0], str(sandbox.LIMIT_NOFILE))
            self.assertEqual(lines[1], str(sandbox.LIMIT_AS_BYTES // 1024))


class TestEngineTables(unittest.TestCase):
    def test_polyglot_executes_column(self):
        execs = {t.name for c in pg.CHECKS for t in c.tools if t.executes}
        self.assertEqual(execs, {"perl", "cargo-clippy", "eslint"})
        mypy = next(t for c in pg.CHECKS for t in c.tools if t.name == "mypy")
        self.assertIn("--config-file=", mypy.argv)

    def test_exec_stage_table_matches_cpp(self):
        cpp = dict(re.findall(r'\{"([a-z]+)", "(whole|part)"\}', PIPELINE_CPP))
        self.assertEqual(cpp, EXEC_STAGES)

    def test_cli_flag_in_both_engines(self):
        self.assertIn('a == "--allow-exec"', MAIN_CPP)
        self.assertIn("cfg.allow_exec = true", MAIN_CPP)
        main_py = (ROOT / "prism" / "__main__.py").read_text(encoding="utf-8")
        self.assertIn('"--allow-exec"', main_py)
        self.assertIn("allow_exec=args.allow_exec", main_py)

    def test_exec_gate_note_once(self):
        from prism.models import Finding
        held = Finding(stage="fuzz", status=laws.CLEAN, file="", function=None, line=None,
                       cls="", message="", strength=laws.STRENGTH_FINDS,
                       extra={"exec": laws.NOTRUN})
        out = exec_gate_note("fuzz", [held], "fuzz (compiled harness, AFL++, libFuzzer)")
        self.assertEqual(len(out), 2)
        self.assertEqual(len(exec_gate_note("fuzz", out, "fuzz")), 2)


if __name__ == "__main__":
    unittest.main()
