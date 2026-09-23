"""Optional adapters: a missing tool is NOTRUN, never CLEAN.

Search order: config/explicit, the pinned fetch_deps build under
~/.prism/tools/<name>/<commit>/, PATH.
python -m unittest tests.test_adapters
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters import _tool_unusable
from prism.adapters_extra import (
    OPTIONAL_TOOLS,
    _cocci_rules,
    _is_fake_adapter,
    _probe_looks_missing,
    _run_cbmc,
    run_optional_tools,
)
from prism.config import (
    Config,
    adapter_install,
    find_vendored_exe,
    pinned_commit,
    resolve_adapter,
)
from prism.cparse import extract_functions
from prism.models import Finding
from prism.wp import run_wp

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def _no_adapter(*_a, **_k):
    return None


class TestOptionalAdapters(unittest.TestCase):
    def test_signature_returns_list_of_findings(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            out = run_optional_tools([], Config())
        self.assertIsInstance(out, list)
        self.assertTrue(out)
        self.assertTrue(all(isinstance(f, Finding) for f in out))

    def test_every_missing_tool_is_notrun(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([TD], Config())
        by_stage = {f.stage: f for f in findings}
        expected = [spec[0] for spec in OPTIONAL_TOOLS] + ["libfuzzer"]
        self.assertEqual(expected, [
            "klee", "afl-fuzz", "frama-c", "infer",
            "clang-tidy", "cbmc", "strix",
            "semgrep", "spatch", "libfuzzer",
        ])
        for name in expected:
            self.assertIn(name, by_stage, msg=f"missing finding for {name}")
            f = by_stage[name]
            self.assertEqual(f.status, laws.NOTRUN, msg=f"{name} status={f.status}")
            self.assertNotEqual(f.status, laws.CLEAN)
            install = (f.extra or {}).get("install", "")
            self.assertTrue(install, msg=f"{name} has no install hint")
            self.assertIn("third_party/MANIFEST.toml", install)
            if name not in ("clang-tidy", "libfuzzer"):
                self.assertIn("scripts/fetch_deps.py --tool", install, msg=name)
            if name == "libfuzzer":
                self.assertTrue(
                    "not on PATH" in f.message or "not found" in f.message,
                    msg=f.message,
                )
            else:
                self.assertEqual(install, adapter_install(name))
                self.assertIn("not found", f.message)

    def test_missing_is_never_clean_even_with_c_files(self):
        c_files = list(TD.glob("*.c"))[:2]
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools(c_files, Config())
        statuses = {f.status for f in findings}
        self.assertEqual(statuses, {laws.NOTRUN})
        self.assertNotIn(laws.CLEAN, statuses)

    def test_present_help_is_not_a_code_verdict(self):
        def fake_resolve(_cfg, stage, names):
            if stage == "klee" or "klee" in tuple(names):
                return r"C:\tools\klee.exe"
            return None

        help_proc = mock.Mock(returncode=0, stdout="KLEE --help", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=help_proc):
            findings = run_optional_tools([], Config())
        klee = next(f for f in findings if f.stage == "klee")
        self.assertNotEqual(klee.status, laws.CLEAN)
        self.assertNotEqual(klee.status, laws.PROVED)
        self.assertNotEqual(klee.status, laws.NOTRUN)
        self.assertEqual(klee.status, laws.UNKNOWN)
        missing = [f for f in findings if f.stage not in {"klee", "libfuzzer"}]
        self.assertTrue(missing)
        self.assertTrue(all(f.status == laws.NOTRUN for f in missing))

    def test_help_probe_failed_is_notrun_never_clean_or_proved(self):
        """A binary that raises on --help is NOTRUN, not ERROR/CLEAN/PROVED."""
        def fake_resolve(_cfg, stage, names):
            if stage == "klee" or "klee" in tuple(names):
                return r"C:\tools\klee.exe"
            return None

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._probe",
                        side_effect=RuntimeError("exec format error")), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        klee = next(f for f in findings if f.stage == "klee")
        self.assertEqual(klee.status, laws.NOTRUN)
        self.assertNotEqual(klee.status, laws.ERROR)
        self.assertNotEqual(klee.status, laws.CLEAN)
        self.assertNotEqual(klee.status, laws.PROVED)
        self.assertNotEqual(klee.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(klee.status))
        self.assertIn("probe failed", klee.message.lower())
        self.assertEqual((klee.extra or {}).get("exe"), r"C:\tools\klee.exe")

    def test_no_help_answer_is_notrun_never_clean_or_proved(self):
        """Present binary that answers none of --help/-h/--version is NOTRUN."""
        def fake_resolve(_cfg, stage, names):
            if stage == "infer" or "infer" in tuple(names):
                return r"C:\tools\infer"
            return None

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._probe", return_value=None), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        infer = next(f for f in findings if f.stage == "infer")
        self.assertEqual(infer.status, laws.NOTRUN)
        self.assertNotEqual(infer.status, laws.ERROR)
        self.assertNotEqual(infer.status, laws.CLEAN)
        self.assertNotEqual(infer.status, laws.PROVED)
        self.assertNotEqual(infer.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(infer.status))
        self.assertIn("did not answer --help", infer.message)
        self.assertEqual((infer.extra or {}).get("exe"), r"C:\tools\infer")

    def test_help_probe_timeout_all_flags_is_notrun(self):
        def fake_resolve(_cfg, stage, names):
            if stage == "semgrep" or "semgrep" in tuple(names) or "semgrep.exe" in tuple(names):
                return r"C:\tools\semgrep.exe"
            return None

        def boom(cmd, timeout, cwd=None):
            raise subprocess.TimeoutExpired(cmd, timeout)

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=boom), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.NOTRUN)
        self.assertNotEqual(semgrep.status, laws.ERROR)
        self.assertNotEqual(semgrep.status, laws.CLEAN)
        self.assertNotEqual(semgrep.status, laws.PROVED)
        self.assertIn("did not answer --help", semgrep.message)

    def test_shell_not_found_probe_is_notrun_never_error(self):
        """Vendored stub that sh cannot exec is missing, not a failed analysis."""
        def fake_resolve(_cfg, stage, names):
            if stage == "frama-c" or "frama-c" in tuple(names):
                return "/opt/Code Analysis/.prism/tools/frama-c/bin/frama-c"
            return None

        missing = mock.Mock(
            returncode=127,
            stdout="",
            stderr="sh: 1: /opt/Code Analysis/.prism/tools/frama-c/bin/frama-c: not found\n",
        )
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=missing), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        frama = next(f for f in findings if f.stage == "frama-c")
        self.assertEqual(frama.status, laws.NOTRUN)
        self.assertNotEqual(frama.status, laws.ERROR)
        self.assertNotEqual(frama.status, laws.CLEAN)
        self.assertNotEqual(frama.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(frama.status))
        self.assertIn("did not answer --help", frama.message)

    def test_doctest_help_is_not_a_real_adapter(self):
        text = "[doctest] doctest version is \"2.4.11\"\n--stack-trace"
        self.assertTrue(_probe_looks_missing(text, 0))
        self.assertTrue(_probe_looks_missing("Unknown option: --timeout\n", 1))
        self.assertTrue(_is_fake_adapter("Catch2 v3.5.0\n"))
        self.assertTrue(_is_fake_adapter("[doctest] doctest version is 2.4.11\n"))
        self.assertTrue(_tool_unusable("Catch2 v3.5.0\n", 0))
        self.assertTrue(_tool_unusable("", 126))
        self.assertTrue(_tool_unusable("", 127))
        self.assertTrue(_tool_unusable("sh: 1: cbmc: not found\n", 1))
        self.assertTrue(_tool_unusable("'cbmc' is not recognized as an internal or external command\n", 1))
        self.assertTrue(_tool_unusable("cannot execute binary file\n", 126))
        self.assertFalse(_tool_unusable("VERIFICATION SUCCESSFUL\n", 0))
        self.assertFalse(_is_fake_adapter("CBMC 5.95.1\n"))

    def test_catch2_help_as_framac_is_notrun_not_present(self):
        """Catch2 answering --help is not Frama-C; never the 'present' UNKNOWN."""
        def fake_resolve(_cfg, stage, names):
            if stage == "frama-c" or "frama-c" in tuple(names):
                return r"C:\tools\frama-c.exe"
            return None

        catch2 = mock.Mock(returncode=0, stdout="Catch2 v3.5.0\n", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=catch2), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        frama = next(f for f in findings if f.stage == "frama-c")
        self.assertEqual(frama.status, laws.NOTRUN)
        self.assertNotEqual(frama.status, laws.UNKNOWN)
        self.assertNotEqual(frama.status, laws.ERROR)
        self.assertNotEqual(frama.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(frama.status))
        self.assertNotIn("present", frama.message.lower())
        self.assertIn("did not answer --help", frama.message)

    def test_empty_help_success_is_unknown_not_clean(self):
        """rc=0 with empty stdout/stderr is still a successful probe: UNKNOWN."""
        def fake_resolve(_cfg, stage, names):
            if stage == "afl-fuzz" or "afl-fuzz" in tuple(names) or "afl-fuzz.exe" in tuple(names):
                return r"C:\tools\afl-fuzz.exe"
            return None

        empty_ok = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=empty_ok), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        afl = next(f for f in findings if f.stage == "afl-fuzz")
        self.assertEqual(afl.status, laws.UNKNOWN)
        self.assertNotEqual(afl.status, laws.CLEAN)
        self.assertNotEqual(afl.status, laws.PROVED)
        self.assertNotEqual(afl.status, laws.NOTRUN)
        self.assertNotEqual(afl.status, laws.ERROR)
        self.assertFalse(laws.is_proof(afl.status))
        self.assertIn("not a code verdict", afl.message.lower())

    def test_explicit_path_beats_vendor_and_path(self):
        with tempfile.TemporaryDirectory() as td:
            explicit = Path(td) / ("klee.exe" if sys.platform == "win32" else "klee")
            explicit.write_bytes(b"x")
            if sys.platform != "win32":
                explicit.chmod(0o755)
            cfg = Config(tools={"klee": str(explicit)})
            with mock.patch("prism.config.find_vendored_exe", return_value=str(Path(td) / "vendor")), \
                 mock.patch("prism.config.shutil.which", return_value=str(Path(td) / "pathbin")):
                hit = resolve_adapter(cfg, "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), explicit.resolve())

    def test_path_is_last_after_config_and_vendor(self):
        with tempfile.TemporaryDirectory() as td:
            pathbin = Path(td) / ("infer.exe" if sys.platform == "win32" else "infer")
            pathbin.write_bytes(b"x")
            if sys.platform != "win32":
                pathbin.chmod(0o755)
            with mock.patch("prism.config.find_vendored_exe", return_value=None), \
                 mock.patch("prism.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(Config(), "infer", ("infer",))
            self.assertEqual(Path(hit).resolve(), pathbin.resolve())
            with mock.patch("prism.config.find_vendored_exe", return_value=None), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                self.assertIsNone(resolve_adapter(Config(), "infer", ("infer",)))

    def test_pinned_tools_dir_without_path(self):
        """fetch_deps layout: <tools>/<component>/<manifest commit>/bin/<exe>."""
        commit = pinned_commit("klee")
        self.assertIsNotNone(commit, "klee must be pinned in third_party/MANIFEST.toml")
        with tempfile.TemporaryDirectory() as td:
            tools = Path(td) / "tools"
            exe_dir = tools / "klee" / str(commit) / "bin"
            exe_dir.mkdir(parents=True)
            exe = exe_dir / ("klee.exe" if sys.platform == "win32" else "klee")
            exe.write_bytes(b"MZ" if sys.platform == "win32" else b"\x7fELF")
            if sys.platform != "win32":
                exe.chmod(0o755)
            # A build of any other commit is not the pinned tool.
            other = tools / "klee" / ("0" * 40) / "bin"
            other.mkdir(parents=True)
            with mock.patch.dict("os.environ", {"PRISM_TOOLS_DIR": str(tools)}), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                hit = find_vendored_exe("klee", ("klee",))
                resolved = resolve_adapter(Config(root=ROOT / "testdata"), "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), exe.resolve())
            self.assertEqual(Path(resolved).resolve(), exe.resolve())
            exe.unlink()
            with mock.patch.dict("os.environ", {"PRISM_TOOLS_DIR": str(tools)}), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                self.assertIsNone(find_vendored_exe("klee", ("klee",)))

    def test_adapter_install_points_at_fetch_deps(self):
        hint = adapter_install("esbmc")
        self.assertIn("python scripts/fetch_deps.py --tool esbmc", hint)
        self.assertIn("third_party/MANIFEST.toml", hint)
        self.assertNotIn("SOURCES.md", hint)
        self.assertNotIn("apt install", hint)

    def test_semgrep_match_is_failed(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        result_json = (
            '{"results":[{"check_id":"c.lang.security","path":"test.c",'
            '"start":{"line":3},"extra":{"message":"use-after-free"}}]}'
        )
        help_proc = mock.Mock(returncode=0, stdout="semgrep --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout=result_json, stderr="")

        def fake_resolve(_cfg, stage, names):
            if stage == "semgrep" or "semgrep" in tuple(names) or "semgrep.exe" in tuple(names):
                return r"C:\tools\semgrep.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.FAILED)
        self.assertEqual(semgrep.cls, "c.lang.security")

    def test_semgrep_empty_results_is_unknown_not_clean(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        result_json = '{"results":[]}'
        help_proc = mock.Mock(returncode=0, stdout="semgrep --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout=result_json, stderr="")

        def fake_resolve(_cfg, stage, names):
            if stage == "semgrep" or "semgrep" in tuple(names) or "semgrep.exe" in tuple(names):
                return r"C:\tools\semgrep.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        semgrep = next(f for f in findings if f.stage == "semgrep")
        self.assertEqual(semgrep.status, laws.UNKNOWN)
        self.assertNotEqual(semgrep.status, laws.CLEAN)
        self.assertNotEqual(semgrep.status, laws.PROVED)
        self.assertIn("no matches", semgrep.message.lower())

    def test_missing_infer_is_notrun_never_clean_or_proved(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools(c_files, Config())
        infer = next(f for f in findings if f.stage == "infer")
        self.assertEqual(infer.status, laws.NOTRUN)
        self.assertNotEqual(infer.status, laws.CLEAN)
        self.assertNotEqual(infer.status, laws.PROVED)
        self.assertFalse(laws.is_proof(infer.status))
        install = (infer.extra or {}).get("install", "")
        self.assertEqual(install, adapter_install("infer"))
        self.assertIn("fetch_deps.py --tool infer", install)
        self.assertIn("not found", infer.message)

    def test_infer_no_issues_is_unknown_not_clean(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        help_proc = mock.Mock(returncode=0, stdout="infer --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout="No issues found\n", stderr="")

        def fake_resolve(_cfg, stage, names):
            if stage == "infer" or "infer" in tuple(names):
                return r"C:\tools\infer"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        def fake_which(name):
            if name == "gcc":
                return r"C:\tools\gcc.exe"
            return None

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run), \
             mock.patch("prism.adapters_extra.shutil.which", side_effect=fake_which):
            findings = run_optional_tools(c_files, Config())
        infer = next(f for f in findings if f.stage == "infer")
        self.assertEqual(infer.status, laws.UNKNOWN)
        self.assertNotEqual(infer.status, laws.CLEAN)
        self.assertNotEqual(infer.status, laws.PROVED)
        self.assertFalse(laws.is_proof(infer.status))
        self.assertIn("no issues", infer.message.lower())
        self.assertIn("not a proof", infer.message.lower())

    def test_missing_framac_is_notrun_wp_is_separate_stage(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools(c_files, Config())
        frama = next(f for f in findings if f.stage == "frama-c")
        self.assertEqual(frama.status, laws.NOTRUN)
        self.assertNotEqual(frama.status, laws.CLEAN)
        self.assertNotEqual(frama.status, laws.PROVED)
        self.assertFalse(laws.is_proof(frama.status))
        install = (frama.extra or {}).get("install", "")
        self.assertEqual(install, adapter_install("frama-c"))
        self.assertIn("fetch_deps.py --tool frama-c", install)
        self.assertIn("not found", frama.message)
        self.assertFalse(any(f.stage == "wp" for f in findings))

        wp_src = TD / "acsl_abs.c"
        self.assertTrue(wp_src.is_file())
        wp = run_wp(extract_functions(wp_src, wp_src.name), unwind=8)
        self.assertTrue(wp)
        self.assertTrue(all(f.stage == "wp" for f in wp))
        self.assertFalse(any(f.stage == "frama-c" for f in wp))
        self.assertNotEqual(wp[0].status, laws.NOTRUN)

    def test_cocci_rules_discovers_getenv_null(self):
        names = {p.name for p in _cocci_rules([])}
        self.assertIn("getenv_null.cocci", names)
        names_td = {p.name for p in _cocci_rules([TD / "getenv_null.c"])}
        self.assertIn("getenv_null.cocci", names_td)

    def test_spatch_match_is_failed(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        help_proc = mock.Mock(returncode=0, stdout="spatch --help", stderr="")
        match_proc = mock.Mock(
            returncode=0,
            stdout=f"{c_files[0]}:12: realloc(p, n)\n",
            stderr="",
        )

        def fake_resolve(_cfg, stage, names):
            if stage == "spatch" or "spatch" in tuple(names) or "spatch.exe" in tuple(names):
                return r"C:\tools\spatch.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return match_proc

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        spatch = [f for f in findings if f.stage == "spatch" and f.status == laws.FAILED]
        self.assertTrue(spatch)
        self.assertIn("realloc_self", {f.cls for f in spatch})

    def test_spatch_empty_is_unknown_not_clean(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)
        help_proc = mock.Mock(returncode=0, stdout="spatch --help", stderr="")
        scan_proc = mock.Mock(returncode=0, stdout="", stderr="")

        def fake_resolve(_cfg, stage, names):
            if stage == "spatch" or "spatch" in tuple(names) or "spatch.exe" in tuple(names):
                return r"C:\tools\spatch.exe"
            return None

        def fake_run(cmd, timeout):
            if "--help" in cmd or "-h" in cmd or "--version" in cmd or "-version" in cmd:
                return help_proc
            return scan_proc

        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            findings = run_optional_tools(c_files, Config())
        spatch = next(f for f in findings if f.stage == "spatch")
        self.assertEqual(spatch.status, laws.UNKNOWN)
        self.assertNotEqual(spatch.status, laws.CLEAN)
        self.assertNotEqual(spatch.status, laws.PROVED)
        self.assertIn("no matches", spatch.message.lower())

    def test_afl_missing_is_notrun_never_clean(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        afl = next(f for f in findings if f.stage == "afl-fuzz")
        self.assertEqual(afl.status, laws.NOTRUN)
        self.assertNotEqual(afl.status, laws.CLEAN)
        self.assertNotEqual(afl.status, laws.PROVED)
        self.assertFalse(laws.is_proof(afl.status))
        self.assertIn("not found", afl.message)
        self.assertTrue((afl.extra or {}).get("install"))
        self.assertIn("fetch_deps.py --tool aflplusplus", (afl.extra or {}).get("install", ""))

    def test_afl_help_probe_is_never_clean_or_proved(self):
        def fake_resolve(_cfg, stage, names):
            if stage == "afl-fuzz" or "afl-fuzz" in tuple(names) or "afl-fuzz.exe" in tuple(names):
                return r"C:\tools\afl-fuzz.exe"
            return None

        help_proc = mock.Mock(returncode=0, stdout="afl-fuzz --help", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=help_proc), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        afl = next(f for f in findings if f.stage == "afl-fuzz")
        self.assertEqual(afl.status, laws.UNKNOWN)
        self.assertNotEqual(afl.status, laws.CLEAN)
        self.assertNotEqual(afl.status, laws.PROVED)
        self.assertNotEqual(afl.status, laws.PROVED_ASSUMING)
        self.assertFalse(laws.is_proof(afl.status))
        self.assertIn("not a code verdict", afl.message.lower())

    def test_afl_help_with_c_files_is_never_clean_or_proved(self):
        c_files = list(TD.glob("*.c"))[:1]
        self.assertTrue(c_files)

        def fake_resolve(_cfg, stage, names):
            if stage == "afl-fuzz" or "afl-fuzz" in tuple(names) or "afl-fuzz.exe" in tuple(names):
                return r"C:\tools\afl-fuzz.exe"
            return None

        help_proc = mock.Mock(returncode=0, stdout="afl-fuzz --help", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
             mock.patch("prism.adapters_extra._run", return_value=help_proc), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools(c_files, Config())
        afl = next(f for f in findings if f.stage == "afl-fuzz")
        self.assertEqual(afl.status, laws.UNKNOWN)
        self.assertNotEqual(afl.status, laws.CLEAN)
        self.assertNotEqual(afl.status, laws.PROVED)
        self.assertFalse(laws.is_proof(afl.status))

    def test_libfuzzer_missing_is_notrun_never_clean(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([], Config())
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.NOTRUN)
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.PROVED)
        self.assertFalse(laws.is_proof(lf.status))
        self.assertTrue((lf.extra or {}).get("install"))
        self.assertIn("third_party/MANIFEST.toml", (lf.extra or {}).get("install", ""))

    def test_libfuzzer_successful_probe_is_never_clean_or_proved(self):
        compile_ok = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value="/usr/bin/clang"), \
             mock.patch("prism.adapters_extra._run", return_value=compile_ok):
            findings = run_optional_tools([], Config())
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.UNKNOWN)
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.PROVED)
        self.assertNotEqual(lf.status, laws.PROVED_ASSUMING)
        self.assertFalse(laws.is_proof(lf.status))
        self.assertIn("not a code verdict", lf.message.lower())

    def test_libfuzzer_unsupported_flag_is_notrun_never_clean(self):
        unsupported = mock.Mock(
            returncode=1, stdout="", stderr="error: unsupported argument '-fsanitize=fuzzer'",
        )
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value="/usr/bin/clang"), \
             mock.patch("prism.adapters_extra._run", return_value=unsupported):
            findings = run_optional_tools([], Config())
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.NOTRUN)
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.PROVED)
        self.assertFalse(laws.is_proof(lf.status))
        self.assertIn("libfuzzer", lf.message.lower())

    def test_cbmc_refuses_no_bounds_check(self):
        paths = list(TD.glob("*.c"))[:1]
        self.assertTrue(paths)
        cmd = [r"C:\tools\cbmc.exe", str(paths[0]), "--no-bounds-check"]
        with self.assertRaises(ValueError) as ctx:
            for flag in cmd:
                if flag.startswith("--no-") and flag.endswith("-check"):
                    raise ValueError(f"refusing to disable a check: {flag}")
        self.assertIn("--no-bounds-check", str(ctx.exception))

        cmds: list[list[str]] = []
        proc = mock.Mock(returncode=0, stdout="VERIFICATION SUCCESSFUL", stderr="")

        def capture(cmd, timeout, cwd=None):
            cmds.append(list(cmd))
            return proc

        with mock.patch("prism.adapters_extra._run", side_effect=capture):
            _run_cbmc(r"C:\tools\cbmc.exe", paths, Config())
        for built in cmds:
            for flag in built:
                self.assertFalse(
                    flag.startswith("--no-") and flag.endswith("-check"),
                    msg=f"cbmc must not pass check-disabling flags: {flag}",
                )

    def test_cbmc_unknown_option_is_notrun_never_error(self):
        paths = list(TD.glob("*.c"))[:1]
        proc = mock.Mock(
            returncode=1,
            stdout="",
            stderr="Unknown option: --timeout\n --stack-trace\n[doctest] doctest version is 2.4.11\n",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_cbmc(r"C:\tools\cbmc.exe", paths, Config())
        self.assertTrue(out)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertIn("not CBMC", out[0].message)

    def test_catch2_help_as_cbmc_is_notrun(self):
        paths = list(TD.glob("*.c"))[:1]
        proc = mock.Mock(
            returncode=0,
            stdout="Catch2 v3.5.0\nUnknown option: --unwind\n",
            stderr="",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_cbmc(r"C:\tools\cbmc.exe", paths, Config())
        self.assertTrue(out)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.BOUNDED)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertIn("not CBMC", out[0].message)
        self.assertEqual((out[0].extra or {}).get("install"), adapter_install("cbmc"))

    def test_spatch_no_rules_apply_is_unknown_not_error(self):
        from prism.adapters_extra import _run_spatch
        c_files = list(TD.glob("*.c"))[:1]
        proc = mock.Mock(returncode=1, stdout="", stderr="No rules apply. Perhaps your semantic patch")
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_spatch(r"C:\tools\spatch.exe", c_files, Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status != laws.ERROR for f in out))
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("not a proof", out[0].message)
        self.assertNotEqual(out[0].status, laws.CLEAN)


if __name__ == "__main__":
    unittest.main()
