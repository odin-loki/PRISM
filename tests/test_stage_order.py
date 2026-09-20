"""C++ STAGE_ORDER must match Helix. A missing _stage call is a quiet skip."""

from __future__ import annotations

import inspect
import io
import re
import unittest
from pathlib import Path
from unittest.mock import patch

from helix.pipeline import STAGE_ORDER, Pipeline

ROOT = Path(__file__).resolve().parents[1]
HPP = ROOT / "include" / "prism" / "pipeline.hpp"
CPP = ROOT / "src" / "prism" / "pipeline.cpp"
MAIN_CPP = ROOT / "src" / "prism" / "main.cpp"


def _cpp_stage_order() -> list[str]:
    text = HPP.read_text(encoding="utf-8")
    m = re.search(r"STAGE_ORDER\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise AssertionError("include/prism/pipeline.hpp STAGE_ORDER missing")
    return re.findall(r'"([^"]+)"', m.group(1))


def _helix_run_stages() -> list[str]:
    src = inspect.getsource(Pipeline.run)
    return re.findall(r'(?:self\.)?_stage\(\s*"([^"]+)"', src)


def _cpp_run_stages() -> list[str]:
    src = CPP.read_text(encoding="utf-8")
    return re.findall(r'\bstage\(\s*"([^"]+)"', src)


class TestStageOrderContract(unittest.TestCase):
    def test_helix_matches_cpp_header(self):
        cpp = _cpp_stage_order()
        self.assertEqual(list(STAGE_ORDER), cpp)

    def test_pipeline_run_invokes_every_stage_in_order(self):
        """A name in STAGE_ORDER that Pipeline.run never calls is skipped quietly."""
        invoked = _helix_run_stages()
        self.assertEqual(invoked, list(STAGE_ORDER))
        cpp = _cpp_run_stages()
        self.assertEqual(cpp, list(STAGE_ORDER))

    def test_helix_list_stages_prints_stage_order(self):
        from helix.__main__ import main

        buf = io.StringIO()
        with patch("sys.stdout", buf):
            rc = main(["--list-stages"])
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().splitlines(), list(STAGE_ORDER))


class TestResumeHelp(unittest.TestCase):
    def test_helix_help_documents_resume(self):
        from helix.__main__ import main

        buf = io.StringIO()
        with patch("sys.stdout", buf), self.assertRaises(SystemExit) as cm:
            main(["--help"])
        self.assertEqual(cm.exception.code, 0)
        text = buf.getvalue()
        self.assertIn("--resume", text)

    def test_main_cpp_help_lists_list_stages(self):
        src = MAIN_CPP.read_text(encoding="utf-8")
        self.assertIn("--list-stages", src)
        help_blob = src[src.find('"prism PATH') : src.find("Adapter search")]
        self.assertIn("--list-stages", help_blob)
        self.assertIn('a == "--list-stages"', src)

    def test_main_cpp_parses_resume(self):
        src = MAIN_CPP.read_text(encoding="utf-8")
        self.assertIn('a == "--resume"', src)
        self.assertIn("cfg.resume = true", src)
