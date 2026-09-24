"""Differential tests of PRISM's C++ library model headers (roadmap 2.6,
docs/PIR.md "C++ library models").

Each program in tests/cxx_models/ is compiled twice with clang++ under
ASan/UBSan and _GLIBCXX_ASSERTIONS: once against libstdc++, once with the
model headers (src/prism/pir/models/cxx) first on the include path, as the
pir stage lowers C++ units. The traces (contents, sizes, capacities,
constructor/destructor calls, exceptions) must be identical, and the second
build must really have used the models. The pir stage itself is exercised on
the models by the conformance suite (tests/conformance/prism/cxx and
libc-models).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODELS = REPO / "src" / "prism" / "pir" / "models" / "cxx"
PROGRAMS = REPO / "tests" / "cxx_models"
FLAGS = ["-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-g", "-O1", "-w"]
ENV = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}


def _cxx() -> str | None:
    for c in ("clang++-18", "clang++"):
        if shutil.which(c):
            return c
    return None


class CxxModelDifferential(unittest.TestCase):
    tmp: Path
    cxx: str

    @classmethod
    def setUpClass(cls) -> None:
        cxx = _cxx()
        if cxx is None:
            raise unittest.SkipTest("clang++ not found")
        cls.cxx = cxx
        cls.tmp = Path(tempfile.mkdtemp(prefix="prism-cxxmodels-"))
        probe = cls.tmp / "probe.cpp"
        probe.write_text("#include <vector>\nint main() { std::vector<int> v{1}; return v[0] - 1; }\n")
        r = subprocess.run([cxx, *FLAGS, str(probe), "-o", str(cls.tmp / "probe")], capture_output=True, text=True)
        if r.returncode != 0 or subprocess.run([str(cls.tmp / "probe")], env=ENV).returncode != 0:
            raise unittest.SkipTest("clang++ with ASan/UBSan runtimes not usable here")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _build(self, prog: str, with_models: bool) -> Path:
        out = self.tmp / (prog + (".model" if with_models else ".lib"))
        argv = [self.cxx, *FLAGS]
        if with_models:
            argv += ["-isystem", str(MODELS)]
        argv += [str(PROGRAMS / (prog + ".cpp")), "-o", str(out)]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        return out

    def _headers(self, prog: str) -> str:
        r = subprocess.run([self.cxx, *FLAGS, "-isystem", str(MODELS), "-M", str(PROGRAMS / (prog + ".cpp"))],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        return r.stdout

    def _run(self, exe: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=300, env=ENV)

    def _same_trace(self, prog: str, models: list[str]) -> None:
        deps = self._headers(prog)
        for m in models:
            self.assertIn(str(MODELS / m), deps, f"{prog} did not use the model {m}")
        lib, mod = self._build(prog, False), self._build(prog, True)
        a, b = self._run(lib), self._run(mod)
        self.assertEqual(a.returncode, 0, a.stderr[-3000:])
        self.assertEqual(b.returncode, 0, b.stderr[-3000:])
        self.assertGreater(len(a.stdout.splitlines()), 20)
        self.assertEqual(a.stdout.splitlines(), b.stdout.splitlines())

    def test_vector(self) -> None:
        self._same_trace("vector_trace", ["vector"])

    def test_map_set(self) -> None:
        self._same_trace("map_set_trace", ["map", "set", "prism_tree.h"])
        # an iterator to an erased element: heap-use-after-free with both
        for with_models in (False, True):
            r = self._run(self.tmp / ("map_set_trace" + (".model" if with_models else ".lib")), "uaf")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("heap-use-after-free", r.stderr)


if __name__ == "__main__":
    unittest.main()
