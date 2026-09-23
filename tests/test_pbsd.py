"""ParanoidBSD bridge: portable checkers, never empty-ok when the tree is gone.

Missing clang/goto-cc/cbmc is NOTRUN, never CLEAN or PROVED.
PBSD lints never emit PROVED / PROVED-UNBOUNDED. Python engine prism/pbsd.py is
the engine. C++ run_pbsd_lints ports that control flow (never CLEAN-as-proof).
"""

from __future__ import annotations

import inspect
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.checkers import run_lints
from prism.config import Config
from prism.pbsd import HEAVY, VERIFY_SCANNERS, _heavy_notrun, discover_pbsd_root, run_pbsd_lints

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
MISSING = Path(os.path.abspath(os.sep)) / "prism-no-such-paranoidbsd"
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}
_CPP_PBSD = ROOT / "src" / "prism" / "adapters.cpp"


def _cfg(*, pbsd: Path | None = None, allow_exec: bool = False) -> Config:
    # A present ParanoidBSD tree is imported only under --allow-exec (Law 9):
    # tests that exercise the tree half opt in, as a user would.
    kw: dict = {"root": TD, "allow_exec": allow_exec}
    if pbsd is not None:
        kw["pbsd_root"] = pbsd
    return Config(**kw)


def _never_proved(hits) -> None:
    for f in hits:
        assert f.status != laws.PROVED, f.message
        assert f.status != laws.PROVED_UNBOUNDED, f.message
        assert f.status not in _PROOF, f.status
        assert not laws.is_proof(f.status), f.status


def _cpp_pbsd_stub_src() -> str:
    text = _CPP_PBSD.read_text(encoding="utf-8")
    start = text.find("Python engine prism/pbsd.py run_pbsd_lints")
    if start < 0:
        start = text.find("std::vector<Finding> run_pbsd_lints")
    if start < 0:
        return ""
    rest = text[start:]
    end = rest.find("\n}  // namespace prism")
    return rest if end < 0 else rest[:end]


class TestOnesidedLint(unittest.TestCase):
    def test_onesided_lint_fires(self):
        hits = [f for f in run_lints([TD / "onesided.c"], TD)
                if f.cls == "MEM-ONESIDED-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("onesided_index", names)
        self.assertNotIn("two_sided_index", names)
        self.assertNotIn("unsigned_index", names)

    def test_capacity_lint_fires(self):
        hits = [f for f in run_lints([TD / "capacity.c"], TD)
                if f.cls == "MEM-CAPACITY-FIRST"]
        self.assertTrue(hits)


class TestPbsdExplicitOnly(unittest.TestCase):
    """No personal/guessed default tree; using a tree needs --allow-exec (Law 9)."""

    def test_no_hardcoded_default_location(self):
        for rel in ("prism/config.py", "src/prism/config.cpp", "prism/pbsd.py",
                    "src/prism/adapters.cpp"):
            text = (ROOT / rel).read_text(encoding="utf-8")
            self.assertNotIn("odinl", text, rel)
            self.assertNotIn("Desktop", text, rel)
        self.assertNotIn('"ParanoidBSD"', (ROOT / "src" / "prism" / "config.cpp").read_text("utf-8"))
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            self.assertIsNone(Config().pbsd_root)
            os.environ["PRISM_PBSD"] = str(MISSING)
            self.assertEqual(Config().pbsd_root, MISSING)
        finally:
            os.environ.pop("PRISM_PBSD", None)
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_not_configured_is_notrun_with_how(self):
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            hits = run_pbsd_lints([TD / "abs_ok.c"], Config(root=TD))
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old
        self.assertEqual([f.status for f in hits], [laws.NOTRUN])
        self.assertEqual(hits[0].message, "ParanoidBSD tree not configured")
        self.assertIn("--pbsd PATH", hits[0].extra["install"])

    def test_present_tree_without_allow_exec_is_not_imported(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "tools" / "verify").mkdir(parents=True)
            with mock.patch("prism.pbsd._load_verify") as load, \
                 mock.patch("prism.pbsd._run_sibling_guard") as sib:
                hits = run_pbsd_lints([TD / "onesided.c"], _cfg(pbsd=Path(td)))
            load.assert_not_called()
            sib.assert_not_called()
        held = [f for f in hits if f.extra.get("reason") == "executes-scanned-code"]
        self.assertEqual(len(held), 1)
        self.assertEqual(held[0].status, laws.NOTRUN)
        self.assertIn("--allow-exec", held[0].extra["install"])
        # The PRISM portable copies still run.
        self.assertTrue(any(f.cls == "MEM-ONESIDED-INDEX" and f.status == laws.FAILED
                            for f in hits))
        _never_proved(hits)

    def test_cli_flag_both_engines(self):
        main_py = (ROOT / "prism" / "__main__.py").read_text(encoding="utf-8")
        self.assertIn('"--pbsd"', main_py)
        self.assertIn("cfg.pbsd_root = Path(args.pbsd)", main_py)
        main_cpp = (ROOT / "src" / "prism" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('a == "--pbsd"', main_cpp)
        self.assertIn("cfg.pbsd_root = ", main_cpp)
        cpp = _CPP_PBSD.read_text(encoding="utf-8")
        self.assertIn("ParanoidBSD tree not configured", cpp)
        self.assertIn("pbsd (import ParanoidBSD tools/verify modules)", cpp)
        self.assertIn("!cfg.allow_exec", cpp)
        from prism.pipeline import EXEC_STAGES
        self.assertEqual(EXEC_STAGES["pbsd"], "part")


class TestPbsdBridge(unittest.TestCase):
    def test_missing_tree_not_empty_ok(self):
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            hits = run_pbsd_lints([TD / "abs_ok.c"], _cfg(pbsd=MISSING))
            self.assertTrue(hits, "missing tree must not return an empty-ok list")
            self.assertTrue(any(f.status == laws.NOTRUN for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
            self.assertFalse(any(f.status == laws.PROVED for f in hits))
            self.assertFalse(any(f.status == laws.PROVED_UNBOUNDED for f in hits))
            self.assertFalse(any(laws.is_proof(f.status) for f in hits))
            self.assertTrue(any(f.extra.get("install") for f in hits if f.status == laws.NOTRUN))
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_missing_tree_portable_onesided_is_a_finding(self):
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            hits = run_pbsd_lints([TD / "onesided.c"], _cfg(pbsd=MISSING))
            self.assertTrue(hits)
            self.assertTrue(any(f.cls == "MEM-ONESIDED-INDEX" for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
            self.assertFalse(any(f.status == laws.PROVED for f in hits))
            self.assertFalse(any(f.status == laws.PROVED_UNBOUNDED for f in hits))
            self.assertFalse(any(laws.is_proof(f.status) for f in hits))
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_present_tree_invokes_scanners_not_a_note(self):
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            cfg = _cfg(pbsd=Path(old) if old else None, allow_exec=True)
            if discover_pbsd_root(cfg) is None:
                self.skipTest("ParanoidBSD tree not configured (PRISM_PBSD)")
            hits = run_pbsd_lints([TD / "realloc_self.c", TD / "capacity.c"], cfg)
            self.assertFalse(any("use --pbsd-sweep" in (f.message or "") for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
            self.assertFalse(any(f.status == laws.PROVED for f in hits))
            self.assertFalse(any(f.status == laws.PROVED_UNBOUNDED for f in hits))
            self.assertFalse(any(laws.is_proof(f.status) for f in hits))
            vias = [str((f.extra or {}).get("via", "")) for f in hits]
            self.assertTrue(
                any(v.startswith("realloc_self") for v in vias),
                f"realloc_self.scan should have been invoked; vias={vias}",
            )
            self.assertTrue(
                any(f.cls == "MEM-CAPACITY-FIRST" for f in hits),
                "capacity_first should fire on testdata/capacity.c",
            )
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_heavy_notrun_never_clean_or_proved(self):
        with mock.patch("prism.pbsd.shutil.which", return_value=None):
            hits = _heavy_notrun()
        self.assertEqual(len(hits), len(HEAVY))
        self.assertTrue(all(f.stage == "pbsd" for f in hits))
        self.assertTrue(all(f.status == laws.NOTRUN for f in hits))
        self.assertNotIn(laws.CLEAN, {f.status for f in hits})
        self.assertNotIn(laws.PROVED, {f.status for f in hits})
        self.assertNotIn(laws.PROVED_UNBOUNDED, {f.status for f in hits})
        self.assertFalse(any(laws.is_proof(f.status) for f in hits))
        msgs = " ".join(f.message for f in hits)
        self.assertIn("clang not on PATH", msgs)
        self.assertIn("goto-cc not on PATH", msgs)
        self.assertIn("cbmc not on PATH", msgs)
        self.assertIn("not a clean sweep", msgs)
        tools = {(f.extra or {}).get("tool") for f in hits}
        self.assertEqual(tools, {"analyze", "classify", "cbmc"})

    def test_missing_clang_goto_cc_cbmc_with_tree_is_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "tools" / "verify").mkdir(parents=True)
            old = os.environ.pop("PRISM_PBSD", None)
            try:
                with mock.patch("prism.pbsd.shutil.which", return_value=None):
                    hits = run_pbsd_lints([TD / "abs_ok.c"], _cfg(pbsd=Path(td), allow_exec=True))
                notrun = [f for f in hits if f.status == laws.NOTRUN]
                self.assertGreaterEqual(len(notrun), 3)
                statuses = {f.status for f in hits}
                self.assertIn(laws.NOTRUN, statuses)
                self.assertNotIn(laws.CLEAN, statuses)
                self.assertNotIn(laws.PROVED, statuses)
                self.assertNotIn(laws.PROVED_UNBOUNDED, statuses)
                self.assertFalse(any(laws.is_proof(f.status) for f in hits))
                msgs = " ".join(f.message for f in notrun)
                self.assertIn("clang not on PATH", msgs)
                self.assertIn("goto-cc not on PATH", msgs)
                self.assertIn("cbmc not on PATH", msgs)
                self.assertIn("needed for analyze", msgs)
                self.assertIn("needed for classify", msgs)
                self.assertIn("needed for cbmc", msgs)
                self.assertIn("not a clean sweep", msgs)
            finally:
                if old is not None:
                    os.environ["PRISM_PBSD"] = old

    def test_each_heavy_binary_missing_with_tree_is_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "tools" / "verify").mkdir(parents=True)
            old = os.environ.pop("PRISM_PBSD", None)
            try:
                for binary, tool, _how in HEAVY:
                    with self.subTest(binary=binary, tool=tool):
                        def which(name, *args, _missing=binary, **kwargs):
                            return None if name == _missing else os.path.join("C:\\", "prism-fake-bin", name)
                        with mock.patch("prism.pbsd.shutil.which", side_effect=which):
                            hits = run_pbsd_lints([TD / "abs_ok.c"], _cfg(pbsd=Path(td), allow_exec=True))
                        _never_proved(hits)
                        statuses = {f.status for f in hits}
                        self.assertIn(laws.NOTRUN, statuses)
                        self.assertNotIn(laws.CLEAN, statuses)
                        notrun = [f for f in hits if f.status == laws.NOTRUN]
                        msgs = " ".join(f.message for f in notrun)
                        self.assertIn(f"{binary} not on PATH", msgs)
                        self.assertIn(f"needed for {tool}", msgs)
                        self.assertIn("not a clean sweep", msgs)
                        self.assertTrue(any(
                            (f.extra or {}).get("tool") == tool
                            and (f.extra or {}).get("via") == "missing-bin"
                            for f in notrun
                        ), f"missing {binary} must record tool={tool} via=missing-bin")
            finally:
                if old is not None:
                    os.environ["PRISM_PBSD"] = old

    def test_heavy_missing_with_mocked_discover_is_notrun_not_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "tools" / "verify").mkdir(parents=True)
            old = os.environ.pop("PRISM_PBSD", None)
            try:
                with mock.patch("prism.pbsd.discover_pbsd_root", return_value=root):
                    with mock.patch("prism.pbsd.shutil.which", return_value=None):
                        hits = run_pbsd_lints([TD / "abs_ok.c"],
                                              _cfg(pbsd=MISSING, allow_exec=True))
                notrun = [f for f in hits if f.status == laws.NOTRUN]
                self.assertGreaterEqual(len(notrun), len(HEAVY))
                _never_proved(hits)
                statuses = {f.status for f in hits}
                self.assertIn(laws.NOTRUN, statuses)
                self.assertNotIn(laws.CLEAN, statuses)
                msgs = " ".join(f.message for f in notrun)
                self.assertIn("clang not on PATH", msgs)
                self.assertIn("goto-cc not on PATH", msgs)
                self.assertIn("cbmc not on PATH", msgs)
                self.assertIn("not a clean sweep", msgs)
                tools = {(f.extra or {}).get("tool") for f in notrun}
                self.assertTrue({"analyze", "classify", "cbmc"} <= tools)
            finally:
                if old is not None:
                    os.environ["PRISM_PBSD"] = old

    def test_pbsd_lints_never_emit_proved_or_unbounded(self):
        plants = [TD / "abs_ok.c", TD / "onesided.c", TD / "uaf.c",
                  TD / "lock_imbalance.c", TD / "format.c"]
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            for paths in ([], plants, [TD / "abs_ok.c"]):
                with self.subTest(n=len(paths)):
                    hits = run_pbsd_lints(paths, _cfg(pbsd=MISSING))
                    _never_proved(hits)
                    self.assertNotIn(laws.PROVED, {f.status for f in hits})
                    self.assertNotIn(laws.PROVED_UNBOUNDED, {f.status for f in hits})
                    self.assertFalse(any(f.status == laws.CLEAN for f in hits))
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_empty_scope_unknown_or_empty_not_clean_as_proof(self):
        old = os.environ.pop("PRISM_PBSD", None)
        try:
            missing = run_pbsd_lints([], _cfg(pbsd=MISSING))
            _never_proved(missing)
            statuses = {f.status for f in missing}
            self.assertNotIn(laws.CLEAN, statuses)
            self.assertNotIn(laws.PROVED, statuses)
            self.assertNotIn(laws.PROVED_UNBOUNDED, statuses)
            self.assertTrue(
                (not missing) or statuses <= {laws.UNKNOWN, laws.NOTRUN},
                f"empty/missing scope must be UNKNOWN, NOTRUN, or empty; got {statuses}",
            )

            with tempfile.TemporaryDirectory() as td:
                (Path(td) / "tools" / "verify").mkdir(parents=True)
                with mock.patch("prism.pbsd.shutil.which", return_value=None):
                    present = run_pbsd_lints([], _cfg(pbsd=Path(td), allow_exec=True))
                _never_proved(present)
                present_st = {f.status for f in present}
                self.assertNotIn(laws.CLEAN, present_st)
                self.assertNotIn(laws.PROVED, present_st)
                self.assertNotIn(laws.PROVED_UNBOUNDED, present_st)
                self.assertTrue(
                    (not present) or present_st <= {laws.UNKNOWN, laws.NOTRUN},
                    f"empty scope with tree must be UNKNOWN, NOTRUN, or empty; got {present_st}",
                )
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_planted_uaf_lock_format_still_failed(self):
        plants = (
            (TD / "uaf.c", "MEM-UAF"),
            (TD / "lock_imbalance.c", "LOCK-IMBALANCE"),
            (TD / "format.c", "FMT-STRING"),
        )
        for path, cls in plants:
            with self.subTest(cls=cls):
                hits = [f for f in run_lints([path], TD) if f.cls == cls]
                self.assertTrue(hits, f"planted {cls} must still fire")
                self.assertTrue(all(f.status == laws.FAILED for f in hits))
                _never_proved(hits)
                self.assertFalse(any(f.status == laws.CLEAN for f in hits))

        old = os.environ.pop("PRISM_PBSD", None)
        try:
            pbsd_hits = run_pbsd_lints([p for p, _ in plants], _cfg(pbsd=MISSING))
            _never_proved(pbsd_hits)
            self.assertFalse(any(f.status == laws.CLEAN for f in pbsd_hits))
            self.assertFalse(any(
                f.cls in {"MEM-UAF", "LOCK-IMBALANCE", "FMT-STRING"}
                and (laws.is_proof(f.status) or f.status == laws.CLEAN)
                for f in pbsd_hits
            ))
        finally:
            if old is not None:
                os.environ["PRISM_PBSD"] = old

    def test_adapters_run_pbsd_fake_tree_is_not_clean_as_proof(self):
        from prism.adapters import run_pbsd
        py_src = inspect.getsource(run_pbsd)
        self.assertIn("run_pbsd_lints", py_src)
        self.assertNotIn("laws.CLEAN", py_src)

        with tempfile.TemporaryDirectory() as td:
            verify = Path(td) / "tools" / "verify"
            verify.mkdir(parents=True)
            (verify / "sweep_all.py").write_text("# fake sweep_all\n", encoding="utf-8")
            old = os.environ.pop("PRISM_PBSD", None)
            try:
                cfg = _cfg(pbsd=Path(td), allow_exec=True)
                with mock.patch("prism.pbsd.shutil.which", return_value=None):
                    for src in (TD / "abs_ok.c", TD / "onesided.c"):
                        with self.subTest(src=src.name):
                            adapter = run_pbsd(cfg, src)
                            lints = run_pbsd_lints([src], cfg)
                            _never_proved(adapter)
                            _never_proved(lints)
                            a_st = {f.status for f in adapter}
                            l_st = {f.status for f in lints}
                            self.assertEqual(a_st, l_st)
                            self.assertNotIn(laws.CLEAN, a_st)
                            self.assertNotIn(laws.PROVED, a_st)
                            self.assertNotIn(laws.PROVED_UNBOUNDED, a_st)
                            self.assertFalse(any(laws.is_proof(f.status) for f in adapter))
                            self.assertTrue(
                                adapter and a_st <= {laws.NOTRUN, laws.FAILED},
                                f"must be NOTRUN and/or FAILED portable; got {a_st}",
                            )
                            self.assertFalse(any(
                                "use --pbsd-sweep" in (f.message or "") for f in adapter
                            ))
                            if src.name == "onesided.c":
                                self.assertTrue(any(
                                    f.cls == "MEM-ONESIDED-INDEX" and f.status == laws.FAILED
                                    for f in adapter
                                ))
                            else:
                                self.assertTrue(any(f.status == laws.NOTRUN for f in adapter))
            finally:
                if old is not None:
                    os.environ["PRISM_PBSD"] = old

    def test_prism_is_the_real_engine_cpp_never_clean_as_proof(self):
        self.assertIn("lock_balance", VERIFY_SCANNERS)
        py_src = inspect.getsource(run_pbsd_lints)
        self.assertIn("_prism_portable", py_src)
        self.assertNotIn("laws.CLEAN", py_src)
        self.assertNotIn("laws.PROVED", py_src)
        from prism import pipeline
        self.assertIn("from prism.pbsd import run_pbsd_lints", inspect.getsource(pipeline))

        cpp = _cpp_pbsd_stub_src()
        self.assertTrue(cpp, "adapters.cpp must define run_pbsd_lints")
        self.assertIn("prism.portable", cpp)
        self.assertIn("prism.checkers", cpp)
        self.assertIn("MEM-ONESIDED-INDEX", cpp)
        self.assertIn("MEM-CAPACITY-FIRST", cpp)
        for cls in ("MEM-REALLOC-SELF", "MEM-NOWAIT", "FUNC-NORETURN",
                    "UNINIT-SWITCH", "CTRL-SIBLING-ASYMMETRY",
                    "PTR-NULL-DEREF", "LOCK-IMBALANCE"):
            self.assertIn(cls, cpp)
        self.assertIn("missing-bin", cpp)
        self.assertIn("not a clean sweep", cpp)
        self.assertIn("laws::NOTRUN", cpp)
        self.assertNotIn("laws::CLEAN", cpp)
        self.assertNotIn("laws::PROVED", cpp)
        self.assertNotIn("PROVED-UNBOUNDED", cpp)
        # Presence of tools/verify is not a proof; do not wrap sweep_all.py as CLEAN.
        self.assertNotRegex(cpp, r"laws::CLEAN")
        self.assertIn("Do not wrap sweep_all.py as CLEAN", cpp)


if __name__ == "__main__":
    unittest.main()
