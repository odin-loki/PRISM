"""CUDA mutation honesty. python -m unittest tests.test_cuda

Missing or unusable nvcc (including clang host) is a CMake WARNING NOTRUN,
never a silent disable that looks CLEAN. src/cuda/mutate.cu overlays the
same INTERESTING_8 / INTERESTING_16 / INTERESTING_32 tables as host
havoc.cpp. Helix is law: missing GPU does not invent mutation coverage.

helix/simdmut.py may search build_wsl/libprism_cuda.so. CUDA is not
required at import; GPU failure is a CPU / Python fallback, never CLEAN.

These tests read sources. CUDA may be OFF.
"""

from __future__ import annotations

import inspect
import re
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.models import Finding, RunReport, StageResult
from helix.simdmut import _cuda_lib_candidates, _try_preload_cuda, havoc
from helix.taxonomy import coverage_from_report

ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
HAVOC_CPP = ROOT / "src" / "prism" / "havoc.cpp"
MUTATE_CU = ROOT / "src" / "cuda" / "mutate.cu"

WARN_NOTRUN = "CUDA kernel skipped (NOTRUN, not a silent disable)"
AFL_8 = (-128, -1, 0, 1, 16, 32, 64, 100, 127)
AFL_16 = (-32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767)
AFL_32 = (
    -2147483648,
    -100663046,
    -32769,
    32768,
    65535,
    65536,
    100663045,
    2139095040,
    2147483647,
)


def _brace_ints(src: str, name: str) -> tuple[int, ...]:
    i = src.index(name)
    a = src.index("{", i)
    b = src.index("}", a)
    body = src[a : b + 1]
    body = body.replace("std::numeric_limits<int32_t>::min()", "-2147483648")
    body = re.sub(r"\(int32_t\)\s*0x80000000", "-2147483648", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return tuple(int(x, 0) for x in re.findall(r"-?\d+|0x[0-9A-Fa-f]+", body))


def _cuda_block(cmake: str) -> str:
    m = re.search(
        r"if\s*\(\s*PRISM_CUDA\s*\).(.*?)(?:\nendif\(\)\s*\n\s*\n|\nendif\(\)\s*\n"
        r"if\(PRISM_LLAMA)",
        cmake,
        re.S,
    )
    if not m:
        raise AssertionError("PRISM_CUDA block missing in CMakeLists.txt")
    return m.group(0)


class TestCmakeNvccClangIsNotrun(unittest.TestCase):
    """nvcc unusable with clang host → WARNING NOTRUN, not a silent CLEAN skip."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = CMAKE.read_text(encoding="utf-8", errors="replace")
        cls.block = _cuda_block(cls.cmake)

    def test_warning_text_when_nvcc_unusable_with_clang_host(self):
        self.assertIn('CMAKE_CXX_COMPILER_ID MATCHES "Clang"', self.block)
        self.assertIn("NOT CMAKE_CXX_COMPILER_ID MATCHES", self.block)
        self.assertIn(WARN_NOTRUN, self.block)
        warn_lines = [
            ln for ln in self.block.splitlines() if WARN_NOTRUN in ln
        ]
        self.assertTrue(warn_lines, "WARNING line with NOTRUN skip text missing")
        self.assertTrue(
            any("message(WARNING" in ln for ln in warn_lines),
            warn_lines,
        )
        self.assertIn("enable_language(CUDA)", self.block)
        # Clang without a gcc CUDA host compiler warns before enable_language
        # (skips check_language). Missing nvcc still warns in the else of
        # CMAKE_CUDA_COMPILER — not a quiet no-op that looks CLEAN.
        clang_else = re.search(
            r'if\s*\(\s*CMAKE_CXX_COMPILER_ID MATCHES "Clang"\s*\).(.*?)endif\(\)',
            self.block,
            re.S,
        )
        self.assertIsNotNone(clang_else, "Clang host-compiler branch missing")
        self.assertIn(WARN_NOTRUN, clang_else.group(0))
        self.assertIn("message(WARNING", clang_else.group(0))
        enable_at = self.block.index("enable_language(CUDA)")
        last_warn = self.block.rindex(WARN_NOTRUN)
        self.assertLess(enable_at, last_warn)
        self.assertIn("else()", self.block[enable_at:last_warn])

    def test_skip_is_warning_not_status_or_clean(self):
        self.assertNotIn("CUDA kernel skipped (CLEAN", self.cmake)
        self.assertNotIn(f'message(STATUS "{WARN_NOTRUN}"', self.block)
        self.assertNotIn('message(STATUS "CUDA kernel skipped', self.block)
        self.assertNotRegex(
            self.block,
            r"message\s*\(\s*STATUS\s+.*CUDA kernel skipped",
        )
        self.assertIn("NOTRUN", self.block)
        self.assertNotEqual(laws.NOTRUN, laws.CLEAN)


class TestMutateCuMatchesHostHavoc(unittest.TestCase):
    """mutate.cu INTERESTING_8/16/32 overlays match havoc.cpp even if CUDA is OFF."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cu = MUTATE_CU.read_text(encoding="utf-8", errors="replace")
        cls.host = HAVOC_CPP.read_text(encoding="utf-8", errors="replace")

    def test_interesting_8_16_32_match_havoc(self):
        for name, expected in (
            ("interesting8", AFL_8),
            ("interesting16", AFL_16),
            ("interesting32", AFL_32),
        ):
            cu_vals = _brace_ints(self.cu, name)
            host_vals = _brace_ints(self.host, name)
            self.assertEqual(cu_vals, expected, name)
            self.assertEqual(cu_vals, host_vals, name)

    def test_overlay_widths_1_2_4(self):
        widths = [int(w) for w in re.findall(r"overlay_le\s*\([^;]+,\s*([124])\s*\)", self.cu)]
        host_w = [int(w) for w in re.findall(r"overlay_le\s*\([^;]+,\s*([124])\s*\)", self.host)]
        self.assertEqual(widths, [1, 2, 4])
        self.assertEqual(widths, host_w)
        self.assertIn("kind == 4", self.cu)
        self.assertIn("kind == 5", self.cu)
        self.assertIn("kind == 6", self.cu)


class TestMissingGpuNeverCleanCoverage(unittest.TestCase):
    """Missing GPU/nvcc is Helix NOTRUN. It must not look like CLEAN mutation coverage."""

    def test_helix_notrun_is_no_answer_not_clean(self):
        self.assertIn(laws.NOTRUN, laws.NO_ANSWER)
        self.assertNotIn(laws.CLEAN, laws.NO_ANSWER)
        self.assertNotEqual(laws.NOTRUN, laws.CLEAN)
        self.assertFalse(laws.is_proof(laws.NOTRUN))
        self.assertFalse(laws.is_proof(laws.CLEAN))
        self.assertIn(laws.NOTRUN, laws.FUZZ_VERDICTS)
        self.assertIn(laws.CLEAN, laws.FUZZ_VERDICTS)

    def test_mutate_cu_documents_missing_nvcc_as_notrun(self):
        cu = MUTATE_CU.read_text(encoding="utf-8", errors="replace")
        self.assertIn("NOTRUN", cu)
        self.assertIn("Missing nvcc", cu)
        self.assertNotIn("CLEAN", cu)

    def test_notrun_fuzz_stage_does_not_cover(self):
        rec = RunReport(root="x")
        rec.stages.append(
            StageResult(
                name="fuzz",
                status="NOTRUN",
                detail="nvcc unusable / no GPU — CUDA kernel skipped",
                findings=[
                    Finding(
                        stage="fuzz",
                        status=laws.NOTRUN,
                        file="",
                        function=None,
                        line=None,
                        cls="INT-SIGNED-OVF",
                        message="CUDA kernel skipped (NOTRUN, not a silent disable)",
                        strength=laws.STRENGTH_FINDS,
                    )
                ],
                records=1,
            )
        )
        rows = {r["id"]: r for r in coverage_from_report(rec)}
        self.assertEqual(rows["INT-SIGNED-OVF"]["verdict"], "GAP")
        self.assertNotEqual(rows["INT-SIGNED-OVF"]["verdict"], "COVERED")

    def test_missing_gpu_must_not_be_reported_clean(self):
        rec = RunReport(root="x")
        rec.stages.append(
            StageResult(
                name="muttest",
                status="NOTRUN",
                findings=[
                    Finding(
                        stage="muttest",
                        status=laws.NOTRUN,
                        file="",
                        function=None,
                        line=None,
                        cls="FUNC-CONTRACT",
                        message="missing GPU is not mutation coverage",
                        strength=laws.STRENGTH_SOME,
                    )
                ],
                records=1,
            )
        )
        rows = {r["id"]: r for r in coverage_from_report(rec)}
        self.assertEqual(rows["FUNC-CONTRACT"]["verdict"], "GAP")
        self.assertNotEqual(rows["FUNC-CONTRACT"]["verdict"], "COVERED")
        # A CLEAN finding from a skipped GPU would still not COVERED, and Helix
        # forbids treating that skip as CLEAN in the first place.
        self.assertNotEqual(laws.NOTRUN, laws.CLEAN)
        fake_clean = Finding(
            stage="fuzz",
            status=laws.CLEAN,
            file="",
            function=None,
            line=None,
            cls="INT-SIGNED-OVF",
            message="would be dishonest if this came from missing GPU",
            strength=laws.STRENGTH_FINDS,
        )
        rec2 = RunReport(root="x")
        rec2.stages.append(
            StageResult(name="fuzz", status="ok", findings=[fake_clean], records=1)
        )
        rows2 = {r["id"]: r for r in coverage_from_report(rec2)}
        self.assertNotEqual(rows2["INT-SIGNED-OVF"]["verdict"], "COVERED")
        self.assertFalse(laws.is_proof(fake_clean.status))


class TestHelixDoesNotRequireCudaAtImport(unittest.TestCase):
    """libprism_cuda.so is optional. GPU failure is CPU fallback, never CLEAN."""

    def test_simdmut_searches_wsl_cuda_so_but_does_not_import_gpu(self):
        src = inspect.getsource(__import__("helix.simdmut", fromlist=["havoc"]))
        self.assertIn("libprism_cuda.so", src)
        self.assertIn("build_wsl", src)
        self.assertNotIn("import cupy", src)
        self.assertNotIn("import pycuda", src)
        self.assertNotRegex(src, r"^import cuda\b", re.M)
        cands = _cuda_lib_candidates()
        self.assertIn(ROOT / "build_wsl" / "libprism_cuda.so", cands)

    def test_gpu_load_failure_is_cpu_fallback_not_proof(self):
        with mock.patch("helix.simdmut._cdll", side_effect=OSError("GPU failure")):
            self.assertFalse(_try_preload_cuda())
        out = havoc(b"\x00\x01\x02\x03\x04\x05\x06\x07")
        self.assertEqual(len(out), 8)
        self.assertFalse(laws.is_proof(laws.CLEAN))
        self.assertTrue(laws.is_proof(laws.PROVED))
        self.assertNotEqual(laws.CLEAN, laws.PROVED)
        self.assertIn(laws.NOTRUN, laws.NO_ANSWER)

    def test_simdmut_source_says_gpu_failure_is_cpu_fallback(self):
        py = (ROOT / "helix" / "simdmut.py").read_text(encoding="utf-8")
        self.assertIn("GPU", py)
        self.assertIn("CPU fallback", py)
        self.assertIn("not required at import", py)
        self.assertIn("never CLEAN/PROVED", py)


if __name__ == "__main__":
    unittest.main()
