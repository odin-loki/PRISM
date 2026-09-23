"""AFL++ interesting-value havoc. python -m unittest tests.test_simdmut

Host havoc.cpp and src/cuda/mutate.cu must overlay the same INTERESTING_8 /
INTERESTING_16 / INTERESTING_32 tables as AFL++ config.h. Missing nvcc is
NOTRUN, never a fake CLEAN proof. CUDA may be OFF; these tests read sources.

prism/simdmut.py may ctypes-load libprism_native.so (src/prism/capi.cpp).
A missing library is a Python fallback, never CLEAN/PROVED.
"""

from __future__ import annotations

import os
import re
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.simdmut import (
    INTERESTING_8,
    INTERESTING_16,
    INTERESTING_32,
    _CPU_LIB_NAMES,
    _CUDA_LIB_NAMES,
    _bind_native_symbols,
    _cuda_lib_candidates,
    _native_lib_candidates,
    _native_search_dirs,
    _overlay_le,
    _try_load_dll,
    _try_preload_cuda,
    havoc,
)

ROOT = Path(__file__).resolve().parents[1]
HAVOC_CPP = ROOT / "src" / "prism" / "havoc.cpp"
MUTATE_CU = ROOT / "src" / "cuda" / "mutate.cu"
# The mined AFL++ tree is gone (roadmap 1.1). This is the verbatim
# "interesting values" block of AFL++ include/config.h at the pinned commit
# (third_party/MANIFEST.toml: aflplusplus v5.03c, dbaf11913c1b2702dee5b4d3dcfffd52f1defe50,
# lines 384-422), kept as a fixture so the havoc tables stay locked to upstream.
AFL_CONFIG = "AFL++ v5.03c include/config.h"
AFL_CONFIG_TEXT = r"""
#define INTERESTING_8                                    \
  -128,    /* Overflow signed 8-bit when decremented  */ \
      -1,  /*                                         */ \
      0,   /*                                         */ \
      1,   /*                                         */ \
      16,  /* One-off with common buffer size         */ \
      32,  /* One-off with common buffer size         */ \
      64,  /* One-off with common buffer size         */ \
      100, /* One-off with common buffer size         */ \
      127                        /* Overflow signed 8-bit when incremented  */

#define INTERESTING_8_LEN 9

#define INTERESTING_16                                    \
  -32768,   /* Overflow signed 16-bit when decremented */ \
      -129, /* Overflow signed 8-bit                   */ \
      128,  /* Overflow signed 8-bit                   */ \
      255,  /* Overflow unsig 8-bit when incremented   */ \
      256,  /* Overflow unsig 8-bit                    */ \
      512,  /* One-off with common buffer size         */ \
      1000, /* One-off with common buffer size         */ \
      1024, /* One-off with common buffer size         */ \
      4096, /* One-off with common buffer size         */ \
      32767                      /* Overflow signed 16-bit when incremented */

#define INTERESTING_16_LEN 10

#define INTERESTING_32                                          \
  -2147483648LL,  /* Overflow signed 32-bit when decremented */ \
      -100663046, /* Large negative number (endian-agnostic) */ \
      -32769,     /* Overflow signed 16-bit                  */ \
      32768,      /* Overflow signed 16-bit                  */ \
      65535,      /* Overflow unsig 16-bit when incremented  */ \
      65536,      /* Overflow unsig 16 bit                   */ \
      100663045,  /* Large positive number (endian-agnostic) */ \
      2139095040, /* float infinite                          */ \
      2147483647                 /* Overflow signed 32-bit when incremented */

#define INTERESTING_32_LEN 9
"""
CMAKE = ROOT / "CMakeLists.txt"

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


def _afl_macro_ints(name: str) -> tuple[int, ...]:
    text = AFL_CONFIG_TEXT
    m = re.search(
        rf"#define {re.escape(name)}\b(.*?)#define {re.escape(name)}_LEN",
        text,
        re.S,
    )
    if not m:
        raise AssertionError(f"AFL++ {name} missing in {AFL_CONFIG}")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S).replace("LL", "")
    return tuple(int(x) for x in re.findall(r"-?\d+", body))


def _brace_ints(src: str, name: str) -> tuple[int, ...]:
    i = src.index(name)
    a = src.index("{", i)
    b = src.index("}", a)
    body = src[a : b + 1]
    body = body.replace("std::numeric_limits<int32_t>::min()", "-2147483648")
    body = re.sub(r"\(int32_t\)\s*0x80000000", "-2147483648", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return tuple(int(x, 0) for x in re.findall(r"-?\d+|0x[0-9A-Fa-f]+", body))


def _overlay_widths(src: str) -> list[int]:
    return [int(w) for w in re.findall(r"overlay_le\s*\([^;]+,\s*([124])\s*\)", src)]


class TestAflInteresting(unittest.TestCase):
    def test_interesting_8_from_afl(self):
        self.assertEqual(INTERESTING_8, AFL_8)
        self.assertEqual(INTERESTING_8, _afl_macro_ints("INTERESTING_8"))

    def test_interesting_16_from_afl(self):
        self.assertEqual(INTERESTING_16, AFL_16)
        self.assertEqual(INTERESTING_16, _afl_macro_ints("INTERESTING_16"))

    def test_interesting_32_from_afl(self):
        self.assertEqual(INTERESTING_32, AFL_32)
        self.assertEqual(INTERESTING_32, _afl_macro_ints("INTERESTING_32"))
        self.assertEqual(INTERESTING_32[0], -2147483648)

    def test_overlay_le_8(self):
        buf = bytearray(4)
        _overlay_le(buf, 0, -128, 1)
        self.assertEqual(buf[0], 0x80)
        _overlay_le(buf, 1, 127, 1)
        self.assertEqual(buf[1], 127)

    def test_overlay_le_16(self):
        buf = bytearray(4)
        _overlay_le(buf, 0, 256, 2)
        self.assertEqual(buf[0], 0)
        self.assertEqual(buf[1], 1)
        _overlay_le(buf, 0, -32768, 2)
        self.assertEqual(bytes(buf[:2]), b"\x00\x80")

    def test_overlay_le_32(self):
        buf = bytearray(8)
        _overlay_le(buf, 0, 32768, 4)
        self.assertEqual(bytes(buf[:4]), b"\x00\x80\x00\x00")
        _overlay_le(buf, 0, -2147483648, 4)
        self.assertEqual(bytes(buf[:4]), b"\x00\x00\x00\x80")
        _overlay_le(buf, 0, 2139095040, 4)
        self.assertEqual(int.from_bytes(buf[:4], "little", signed=True), 2139095040)

    def test_overlay_le_clips_at_end(self):
        buf = bytearray(b"\xff\xff")
        _overlay_le(buf, 1, 0x01020304, 4)
        self.assertEqual(buf[0], 0xFF)
        self.assertEqual(buf[1], 0x04)

    def test_havoc_preserves_length(self):
        src = b"\x00\x01\x02\x03\x04\x05\x06\x07"
        out = havoc(src)
        self.assertEqual(len(out), len(src))
        self.assertNotEqual(INTERESTING_32[0], 0)
        self.assertFalse(laws.is_proof(laws.CLEAN))


class TestHostHavocSource(unittest.TestCase):
    """src/prism/havoc.cpp tables must match AFL++. Host unit + source contract."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = HAVOC_CPP.read_text(encoding="utf-8", errors="replace")

    def test_interesting_tables_match_afl(self):
        self.assertEqual(_brace_ints(self.src, "interesting8"), AFL_8)
        self.assertEqual(_brace_ints(self.src, "interesting16"), AFL_16)
        self.assertEqual(_brace_ints(self.src, "interesting32"), AFL_32)
        self.assertIn("std::numeric_limits<int32_t>::min()", self.src)

    def test_overlay_widths_1_2_4(self):
        self.assertEqual(_overlay_widths(self.src), [1, 2, 4])
        self.assertIn("case 4:", self.src)
        self.assertIn("case 5:", self.src)
        self.assertIn("case 6:", self.src)

    def test_python_binding_uses_same_overlays(self):
        py = (ROOT / "prism" / "simdmut.py").read_text(encoding="utf-8")
        self.assertIn("_overlay_le(b, i, v, 1)", py)
        self.assertIn("_overlay_le(b, i, v, 2)", py)
        self.assertIn("_overlay_le(b, i, v, 4)", py)
        out = havoc(b"\x00\x01\x02\x03\x04\x05\x06\x07")
        self.assertEqual(len(out), 8)


class TestCudaMutateSource(unittest.TestCase):
    """mutate.cu must overlay 8/16/32 like host havoc, even if CUDA is OFF."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = MUTATE_CU.read_text(encoding="utf-8", errors="replace")
        cls.host = HAVOC_CPP.read_text(encoding="utf-8", errors="replace")

    def test_interesting_16_and_32_not_just_8(self):
        self.assertIn("interesting8", self.src)
        self.assertIn("interesting16", self.src)
        self.assertIn("interesting32", self.src)
        self.assertEqual(_brace_ints(self.src, "interesting8"), AFL_8)
        self.assertEqual(_brace_ints(self.src, "interesting16"), AFL_16)
        self.assertEqual(_brace_ints(self.src, "interesting32"), AFL_32)

    def test_overlays_match_host_havoc(self):
        self.assertEqual(
            _brace_ints(self.src, "interesting8"),
            _brace_ints(self.host, "interesting8"),
        )
        self.assertEqual(
            _brace_ints(self.src, "interesting16"),
            _brace_ints(self.host, "interesting16"),
        )
        self.assertEqual(
            _brace_ints(self.src, "interesting32"),
            _brace_ints(self.host, "interesting32"),
        )
        self.assertEqual(_overlay_widths(self.src), [1, 2, 4])
        self.assertEqual(_overlay_widths(self.src), _overlay_widths(self.host))
        self.assertIn("kind == 5", self.src)
        self.assertIn("kind == 6", self.src)

    def test_missing_nvcc_is_notrun_not_clean(self):
        self.assertIn("NOTRUN", self.src)
        self.assertIn("Missing nvcc", self.src)
        cmake = CMAKE.read_text(encoding="utf-8", errors="replace")
        self.assertIn("CUDA kernel skipped (NOTRUN, not a silent disable)", cmake)
        self.assertNotIn("CUDA kernel skipped (CLEAN", cmake)
        self.assertFalse(laws.is_proof(laws.CLEAN))
        self.assertIn(laws.NOTRUN, laws.NO_ANSWER)
        self.assertNotEqual(laws.NOTRUN, laws.CLEAN)


class TestNativeLoadOrder(unittest.TestCase):
    """ctypes search: PRISM_NATIVE_DLL, build_wsl/, build/. Missing lib ≠ proof."""

    def test_search_dirs_include_wsl_and_build_tree(self):
        dirs = _native_search_dirs()
        self.assertIn(ROOT / "build_wsl", dirs)
        self.assertIn(ROOT / "build", dirs)
        self.assertLess(dirs.index(ROOT / "build_wsl"), dirs.index(ROOT / "build"))
        self.assertNotIn(ROOT / "native", dirs)

    def test_cpu_names_are_prism_native(self):
        self.assertIn("libprism_native.so", _CPU_LIB_NAMES)
        self.assertIn("prism_native.dll", _CPU_LIB_NAMES)
        for name in _CPU_LIB_NAMES:
            self.assertIn("prism_native", name)

    def test_candidates_include_wsl_prism_native(self):
        cands = _native_lib_candidates()
        self.assertIn(ROOT / "build_wsl" / "libprism_native.so", cands)
        self.assertIn(ROOT / "build" / "prism_native.dll", cands)

    def test_prism_native_dll_env_is_first(self):
        custom = ROOT / "custom_python.so"
        with mock.patch.dict(os.environ, {"PRISM_NATIVE_DLL": str(custom)}):
            cands = _native_lib_candidates()
        self.assertEqual(cands[0], custom)

    def test_cuda_candidates_include_wsl_libprism_cuda(self):
        self.assertIn("libprism_cuda.so", _CUDA_LIB_NAMES)
        cands = _cuda_lib_candidates()
        self.assertIn(ROOT / "build_wsl" / "libprism_cuda.so", cands)
        self.assertIn(ROOT / "build" / "libprism_cuda.so", cands)

    def test_bind_prism_capi(self):
        class _Sym:
            def __init__(self):
                self.argtypes = None
                self.restype = None

        prism_h, prism_v = _Sym(), _Sym()

        class PrismOnly:
            prism_coverage_hash = prism_h
            prism_havoc = prism_v

        h, v = _bind_native_symbols(PrismOnly())
        self.assertIsNotNone(h)
        self.assertIsNotNone(v)
        self.assertIsNotNone(prism_h.argtypes)

        class Neither:
            pass

        self.assertEqual(_bind_native_symbols(Neither()), (None, None))

    def test_missing_library_is_python_fallback_never_proof(self):
        with mock.patch("prism.simdmut._cdll", side_effect=OSError("no native lib")):
            h, v = _try_load_dll()
        self.assertIsNone(h)
        self.assertIsNone(v)
        out = havoc(b"\x00\x01\x02\x03")
        self.assertEqual(len(out), 4)
        self.assertFalse(laws.is_proof(laws.CLEAN))
        self.assertTrue(laws.is_proof(laws.PROVED))
        self.assertNotEqual(laws.CLEAN, laws.PROVED)

    def test_source_documents_wsl_capi_and_no_proof(self):
        py = (ROOT / "prism" / "simdmut.py").read_text(encoding="utf-8")
        self.assertIn("build_wsl", py)
        self.assertIn("libprism_native.so", py)
        self.assertIn("libprism_cuda.so", py)
        self.assertIn("PRISM_NATIVE_DLL", py)
        self.assertIn("prism_havoc", py)
        self.assertIn("never CLEAN/PROVED", py)
        capi = (ROOT / "src" / "prism" / "capi.cpp").read_text(encoding="utf-8")
        self.assertIn("prism_havoc", capi)
        self.assertIn("prism_coverage_hash", capi)

    def test_cuda_preload_failure_is_not_required(self):
        with mock.patch("prism.simdmut._cdll", side_effect=OSError("no gpu")):
            self.assertFalse(_try_preload_cuda())
        self.assertFalse(laws.is_proof(laws.CLEAN))


if __name__ == "__main__":
    unittest.main()
