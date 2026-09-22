"""C++ --gui spawn and window source-contract. python -m unittest tests.test_prism_gui -q

Python engine tests/test_gui.py owns widgets. This module locks the PRISM C++ CLI
`--gui` path to the Python engine: missing prism_gui is NOTRUN (never CLEAN, never
run_pipeline), execv/CreateProcessW with `--gui` stripped from forwarded args.
Missing PySide on `python -m prism --gui` is the same NOTRUN contract.
src/gui MainWindow: missing report.json is confidence 0 (not em-dash), no
taxonomy.json fallback, CLEAN stays #2a4a6b. src/gui/main.cpp: no display
is NOTRUN unless QT_QPA_PLATFORM=offscreen.
"""

from __future__ import annotations

import io
import re
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from prism import laws
from prism.__main__ import main as py_main

ROOT = Path(__file__).resolve().parents[1]
MAIN_CPP = ROOT / "src" / "prism" / "main.cpp"
CONFIG_HPP = ROOT / "include" / "prism" / "config.hpp"
PRISM_MAIN = ROOT / "prism" / "__main__.py"
GUI_WINDOW = ROOT / "src" / "gui" / "MainWindow.cpp"
GUI_ENTRY = ROOT / "src" / "gui" / "main.cpp"

NOTRUN_CPP = "NOTRUN gui: prism_gui not found — not a clean window"
INSTALL_CPP = "install: build prism_gui with WSL clang++ Qt6 Widgets (never MinGW)"
ERROR_CPP = "ERROR gui: failed to spawn prism_gui — not a clean window"
NOTRUN_PY = "NOTRUN gui: PySide6 not installed — not a clean window"
NOTRUN_DISPLAY = "NOTRUN gui: no display (not a clean window)"
MISSING_REPORT_CPP = (
    "confidence 0  (vis 0 x ans 0 x res 0) — report.json missing; not a proof"
)
CLEAN_BLUE = "QColor(0x2a, 0x4a, 0x6b)"
PROOF_GREEN = "QColor(0x2a, 0x81, 0x48)"
UNBOUNDED_GREEN = "QColor(0x1f, 0x6f, 0x3a)"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//.*?$", "", src, flags=re.M)


def _help_block(main_cpp: str) -> str:
    start = main_cpp.find('a == "-h" || a == "--help"')
    if start < 0:
        start = main_cpp.find("--help")
    if start < 0:
        return ""
    end = main_cpp.find("return 0", start)
    return main_cpp[start:] if end < 0 else main_cpp[start:end]


def _fn(src: str, name: str) -> str:
    marker = f"{name}("
    start = src.find(marker)
    if start < 0:
        return ""
    brace = src.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for i, ch in enumerate(src[brace:], brace):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return src[start:]


class TestPrismGuiCliContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read(MAIN_CPP)
        cls.code = _strip_comments(cls.src)

    def test_parses_gui_flag(self):
        self.assertIn('a == "--gui"', self.src)
        self.assertIn("cfg.gui = true", self.src)
        cfg = _read(CONFIG_HPP)
        self.assertIn("bool gui = false", cfg)

    def test_help_documents_gui_flag(self):
        help_text = _help_block(self.src)
        self.assertIn("--gui", help_text)
        buf = io.StringIO()
        with patch("sys.stdout", buf), self.assertRaises(SystemExit) as cm:
            py_main(["--help"])
        self.assertEqual(cm.exception.code, 0)
        self.assertIn("--gui", buf.getvalue())

    def test_missing_prism_gui_is_notrun_never_clean(self):
        notrun = _fn(self.src, "notrun_missing_gui")
        self.assertTrue(notrun, msg="notrun_missing_gui missing")
        self.assertIn(NOTRUN_CPP, notrun)
        self.assertIn(INSTALL_CPP, notrun)
        self.assertIn("WSL", notrun)
        self.assertIn("clang++", notrun)
        self.assertIn("Qt6", notrun)
        self.assertIn("Widgets", notrun)
        self.assertIn("never MinGW", notrun)
        self.assertIn("return 0", notrun)
        self.assertNotIn(laws.CLEAN, notrun)
        self.assertNotIn("run_pipeline", notrun)

    def test_gui_does_not_fall_through_to_run_pipeline(self):
        """Python engine returns launch(); C++ must return launch_gui, not CLI CLEAN."""
        helpers = self.code.split("int main(", 1)[0]
        self.assertNotIn("run_pipeline", helpers)
        main = self.code.split("int main(", 1)[1]
        self.assertIn("if (cfg.gui) return launch_gui(argc, argv);", main)
        gui_ret = main.find("if (cfg.gui) return launch_gui")
        pipe = main.find("run_pipeline")
        self.assertGreaterEqual(gui_ret, 0)
        self.assertGreater(pipe, gui_ret)
        launch = _fn(self.src, "launch_gui")
        self.assertIn("notrun_missing_gui", launch)
        self.assertIn("spawn_prism_gui", launch)
        self.assertNotIn("run_pipeline", launch)
        self.assertNotIn("CLEAN", launch)

    def test_linux_execv_windows_createprocessw(self):
        spawn = _fn(self.src, "spawn_prism_gui")
        self.assertTrue(spawn, msg="spawn_prism_gui missing")
        self.assertIn("#ifdef _WIN32", spawn)
        self.assertIn("CreateProcessW", spawn)
        self.assertIn("WaitForSingleObject", spawn)
        self.assertIn("::execv", spawn)
        self.assertNotIn("CREATE_NO_WINDOW", spawn)
        self.assertNotIn("MinGW", spawn)
        self.assertNotIn("mingw", spawn.lower())

    def test_gui_stripped_from_forwarded_args(self):
        spawn = _fn(self.src, "spawn_prism_gui")
        self.assertIn("args.push_back(gui.string());", spawn)
        self.assertIn("for (int i = 1; i < argc; ++i)", spawn)
        self.assertIn('if (std::strcmp(argv[i], "--gui") == 0) continue;', spawn)
        self.assertIn("args.push_back(argv[i]);", spawn)
        self.assertNotRegex(
            spawn,
            r'strcmp\(argv\[i\], "--out"\)',
            msg="--out and other user args must be forwarded",
        )

    def test_finds_prism_gui_beside_self_or_path(self):
        find = _fn(self.src, "find_prism_gui")
        self.assertIn("self_dirs", find)
        self.assertIn("search_path_gui", find)
        self_dirs = _fn(self.src, "self_dirs")
        self.assertIn("GetModuleFileNameA", self_dirs)
        self.assertIn("readlink", self_dirs)
        self.assertIn("/proc/self/exe", self_dirs)
        search = _fn(self.src, "search_path_gui")
        self.assertIn("SearchPathA", search)
        self.assertIn("prism_gui.exe", search)
        self.assertIn('std::getenv("PATH")', search)
        name = _fn(self.src, "gui_name")
        self.assertIn("prism_gui.exe", name)
        self.assertIn('"prism_gui"', name)

    def test_spawn_failure_is_error_never_clean(self):
        err = _fn(self.src, "error_spawn_gui")
        self.assertIn(ERROR_CPP, err)
        self.assertIn("return 2", err)
        self.assertNotIn(laws.CLEAN, err)
        self.assertNotIn("run_pipeline", err)
        spawn = _fn(self.src, "spawn_prism_gui")
        self.assertIn("error_spawn_gui", spawn)
        self.assertIn("CreateProcess error", spawn)
        self.assertIn("execv:", spawn)


class TestPRISMGuiCliContract(unittest.TestCase):
    """python -m prism --gui: missing PySide is NOTRUN, never CLEAN, never pipeline."""

    def test_prism_source_returns_before_pipeline(self):
        src = _read(PRISM_MAIN)
        gui = src.split("if args.gui:", 1)[1]
        gui = gui.split("tools:", 1)[0]
        self.assertIn("from prism.gui import launch", gui)
        self.assertIn(NOTRUN_PY, gui)
        self.assertIn("return 0", gui)
        self.assertIn("return launch(args)", gui)
        self.assertNotIn("run_pipeline", gui)
        self.assertNotIn(laws.CLEAN, gui)

    def test_prism_gui_import_error_is_notrun_never_clean(self):
        fake = types.ModuleType("prism.gui")
        buf = io.StringIO()
        with patch.dict(sys.modules, {"prism.gui": fake}):
            with patch("sys.stdout", buf), \
                 patch("prism.__main__.run_pipeline") as rp:
                rc = py_main(["--gui"])
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn(NOTRUN_PY, text)
        self.assertIn("not a clean window", text)
        self.assertIn("pip install PySide6", text)
        self.assertIn("prism_gui", text)
        self.assertNotIn(laws.CLEAN, text)
        self.assertNotIn("confidence", text)
        rp.assert_not_called()

    def test_prism_gui_calls_launch_not_pipeline(self):
        buf = io.StringIO()
        with patch("prism.gui.launch", return_value=0) as launch_fn, \
             patch("prism.__main__.run_pipeline") as rp, \
             patch("sys.stdout", buf):
            rc = py_main(["--gui"])
        self.assertEqual(rc, 0)
        launch_fn.assert_called_once()
        args = launch_fn.call_args[0][0]
        self.assertTrue(args.gui)
        rp.assert_not_called()
        self.assertNotIn(laws.CLEAN, buf.getvalue())
        self.assertNotIn("confidence", buf.getvalue())


class TestPRISMGuiLaunchHonesty(unittest.TestCase):
    """prism/gui.py launch: missing display is NOTRUN in source, never CLEAN."""

    def test_launch_source_no_display_is_notrun(self):
        src = _read(ROOT / "prism" / "gui.py")
        launch_src = src.split("def launch(", 1)[1]
        launch_src = launch_src.split("def ", 1)[0] if "def " in launch_src else launch_src
        self.assertIn('_notrun_gui("no display"', launch_src)
        self.assertIn("except Exception", launch_src)
        self.assertNotIn(f'"{laws.CLEAN}"', launch_src)
        self.assertNotIn(f"'{laws.CLEAN}'", launch_src)
        self.assertIn("def missing_display_finding", src)
        self.assertIn("MISSING_REPORT_LABEL", src)
        self.assertIn("def refuse_llm_cover", src)
        self.assertIn("def status_background", src)
        self.assertIn("def taxonomy_background", src)
        tax = src.split("def taxonomy_rows", 1)[1].split("def finding_rows", 1)[0]
        self.assertIn("coverage_from_report", tax)
        self.assertIn("refuse_llm_cover", tax)
        label = src.split("def confidence_label", 1)[1].split("def taxonomy_rows", 1)[0]
        self.assertIn("confidence_product", label)
        self.assertNotIn('"n/a"', label)
        self.assertNotIn("'n/a'", label)


class TestCppGuiWindowSourceContract(unittest.TestCase):
    """src/gui MainWindow + main.cpp: missing report is 0, no taxonomy.json."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.window = _read(GUI_WINDOW)
        cls.window_code = _strip_comments(cls.window)
        cls.entry = _read(GUI_ENTRY)
        cls.entry_code = _strip_comments(cls.entry)

    def _load_same_report(self, comments: bool = True) -> str:
        src = self.window if comments else self.window_code
        load = _fn(src, "MainWindow::loadSameReport")
        self.assertTrue(load, msg="MainWindow::loadSameReport missing")
        return load

    def test_missing_report_json_is_confidence_zero_not_emdash(self):
        """loadSameReport else: 0, never n/a, never the ctor em-dash value."""
        load = self._load_same_report()
        self.assertIn("reportFile.open", load)
        else_arm = load.split("} else {", 1)
        self.assertEqual(len(else_arm), 2, msg="missing-report else branch gone")
        missing = else_arm[1].split("fillTaxonomyTable", 1)[0]
        self.assertIn(MISSING_REPORT_CPP, missing)
        self.assertIn('QStringLiteral("visibility 0")', missing)
        self.assertIn('QStringLiteral("answer 0")', missing)
        self.assertIn('QStringLiteral("resolution 0")', missing)
        self.assertIn("confidence 0", missing)
        self.assertIn("report.json missing", missing)
        self.assertIn("not a proof", missing)
        self.assertNotIn("confidence —", missing)
        self.assertNotIn("visibility —", missing)
        self.assertNotIn("n/a", missing.lower())
        self.assertNotIn("N/A", missing)
        self.assertNotIn(laws.CLEAN, missing)
        self.assertNotIn("PROVED", missing)

    def test_does_not_fall_back_to_taxonomy_json(self):
        """Stale taxonomy.json COVERED must not paint after findings are GAP."""
        load = self._load_same_report(comments=False)
        self.assertNotIn("taxonomy.json", load)
        self.assertNotIn("/taxonomy.json", self.window_code)
        self.assertNotIn('QStringLiteral("/taxonomy.json")', self.window_code)
        self.assertNotRegex(
            load,
            r"QFile\s+\w+\s*\([^)]*taxonomy",
            msg="must not QFile-open taxonomy.json",
        )
        self.assertIn("coverage_from_report", load)
        self.assertIn("refuse_llm_cover", load)
        self.assertIn("tax_->setRowCount(0)", load)
        self.assertIn("taxArr.isEmpty()", load)
        # Empty taxArr clears the table; it does not open another JSON file.
        after_empty = load.split("taxArr.isEmpty()", 1)[1]
        self.assertIn("setRowCount(0)", after_empty)
        self.assertNotIn("QFile", after_empty)

    def test_no_display_is_notrun_unless_offscreen(self):
        """DISPLAY/WAYLAND unset and QT_QPA_PLATFORM != offscreen → NOTRUN."""
        main = _fn(self.entry, "main")
        self.assertTrue(main, msg="src/gui/main.cpp main() missing")
        self.assertIn('std::getenv("DISPLAY")', main)
        self.assertIn('std::getenv("WAYLAND_DISPLAY")', main)
        self.assertIn('std::getenv("QT_QPA_PLATFORM")', main)
        self.assertIn('std::strcmp(platform, "offscreen")', main)
        self.assertIn("!offscreen", main)
        self.assertIn(NOTRUN_DISPLAY, main)
        self.assertIn("not a clean window", main)
        self.assertIn("QT_QPA_PLATFORM=offscreen", main)
        self.assertIn("return 2", main)
        self.assertNotIn(laws.CLEAN, _strip_comments(main))
        self.assertNotIn("run_pipeline", main)
        # NOTRUN is only printed when the platform is not already offscreen.
        gate = main.split("if (!offscreen", 1)
        self.assertEqual(len(gate), 2, msg="offscreen bypass missing")
        notrun_arm = gate[1].split("QApplication", 1)[0]
        self.assertIn(NOTRUN_DISPLAY, notrun_arm)
        self.assertIn("return 2", notrun_arm)
        self.assertIn("#ifndef Q_OS_WIN", self.entry_code)

    def test_clean_status_stays_blue_0x2a4a6b(self):
        """CLEAN is #2a4a6b, never PROVED green. Same palette as prism/gui.py."""
        self.assertIn(CLEAN_BLUE, self.window)
        status_fn = _fn(self.window, "findingStatusBg")
        self.assertTrue(status_fn, msg="findingStatusBg missing")
        clean_line = next(
            ln for ln in status_fn.splitlines() if 'QLatin1String("CLEAN")' in ln
        )
        self.assertIn(CLEAN_BLUE, clean_line)
        self.assertNotIn("0x2a, 0x81, 0x48", clean_line)
        self.assertNotIn("0x1f, 0x6f, 0x3a", clean_line)
        verdict_fn = _fn(self.window, "taxonomyVerdictBg")
        self.assertTrue(verdict_fn, msg="taxonomyVerdictBg missing")
        clean_verdict = next(
            ln for ln in verdict_fn.splitlines() if 'QLatin1String("CLEAN")' in ln
        )
        self.assertIn(CLEAN_BLUE, clean_verdict)
        self.assertNotIn("0x2a, 0x81, 0x48", clean_verdict)
        self.assertNotIn("0x1f, 0x6f, 0x3a", clean_verdict)
        covered_line = next(
            ln for ln in verdict_fn.splitlines() if 'QLatin1String("COVERED")' in ln
        )
        self.assertIn(PROOF_GREEN, covered_line)
        self.assertNotIn("0x2a, 0x4a, 0x6b", covered_line)
        proved = next(
            ln for ln in status_fn.splitlines() if 'QLatin1String("PROVED")' in ln
        )
        self.assertIn(PROOF_GREEN, proved)
        self.assertIn(UNBOUNDED_GREEN, status_fn)


if __name__ == "__main__":
    unittest.main()
