"""C++ --jobs / std::jthread source-contract. python -m unittest tests.test_prism_jobs

Python engine tests/test_jobs.py owns runtime Config.jobs. This module locks the
PRISM C++ CLI, ISO jthread workers, CMake Threads, and run_lints(jobs).
"""

from __future__ import annotations

import inspect
import io
import re
import unittest
from pathlib import Path
from unittest.mock import patch

from prism.checkers import run_lints

ROOT = Path(__file__).resolve().parents[1]
MAIN_CPP = ROOT / "src" / "prism" / "main.cpp"
THREADS_HPP = ROOT / "include" / "prism" / "threads.hpp"
CHECKERS_HPP = ROOT / "include" / "prism" / "checkers.hpp"
STAGES_HPP = ROOT / "include" / "prism" / "stages.hpp"
DISPATCH_CPP = ROOT / "src" / "prism" / "checkers_dispatch.cpp"
CMAKE = ROOT / "CMakeLists.txt"


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


class TestPrismJobsContract(unittest.TestCase):
    def test_main_parses_jobs_and_j(self):
        src = _read(MAIN_CPP)
        self.assertIn('--jobs"', src)
        self.assertIn('"-j"', src)
        self.assertIn('a == "--jobs" || a == "-j"', src)
        self.assertIn("cfg.jobs", src)

    def test_threads_hpp_is_iso_jthread(self):
        src = _read(THREADS_HPP)
        code = _strip_comments(src)
        self.assertIn("#include <thread>", src)
        self.assertIn("std::jthread", code)
        self.assertIn("std::vector<std::jthread>", code)
        self.assertNotIn("#include <pthread.h>", code)
        self.assertNotIn("#include <pthread>", code)
        self.assertNotIn("winpthread", code.lower())
        self.assertIsNone(re.search(r"\bpthread_\w+", code))

    def test_cmake_threads_and_mingw_fatal(self):
        src = _read(CMAKE)
        self.assertIn("Threads::Threads", src)
        self.assertIn("find_package(Threads REQUIRED)", src)
        self.assertIn("if(MINGW)", src)
        self.assertIn("FATAL_ERROR", src)
        fatal = src[src.find("if(MINGW)") : src.find("endif()", src.find("if(MINGW)"))]
        self.assertIn("FATAL_ERROR", fatal)
        self.assertIn("MinGW", fatal)

    def test_run_lints_signature_takes_jobs(self):
        sig = inspect.signature(run_lints)
        self.assertIn("jobs", sig.parameters)
        self.assertEqual(sig.parameters["jobs"].default, 0)
        for path in (CHECKERS_HPP, STAGES_HPP, DISPATCH_CPP):
            src = _read(path)
            self.assertRegex(
                src,
                r"run_lints\s*\([^;]*\bjobs\b",
                msg=f"{path.name} run_lints must take jobs",
            )

    def test_help_documents_jobs_flag(self):
        src = _read(MAIN_CPP)
        help_text = _help_block(src)
        self.assertIn("--jobs", help_text)
        self.assertNotEqual(help_text.find("--jobs"), -1)
        self.assertIn("--no-llm", help_text)
        self.assertIn("--fuzz-budget", help_text)
        self.assertIn("--fuzz-iters", help_text)
        self.assertIn("--repair-rounds", help_text)

        from prism.__main__ import main

        buf = io.StringIO()
        with patch("sys.stdout", buf), self.assertRaises(SystemExit) as cm:
            main(["--help"])
        self.assertEqual(cm.exception.code, 0)
        prism_help = buf.getvalue()
        self.assertIn("--jobs", prism_help)
        self.assertIn("-j", prism_help)

    def test_clamp_jobs_zero_is_half_hardware_like_python(self):
        src = _read(THREADS_HPP)
        start = src.find("inline int clamp_jobs")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 500]
        self.assertIn("jobs <= 0", body)
        self.assertIn("hc / 2", body)
        self.assertNotIn("return static_cast<int>(hc);", body)


if __name__ == "__main__":
    unittest.main()
