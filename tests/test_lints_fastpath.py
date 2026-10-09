"""PYTHONHASHSEED independence for the Python engine lints (phase 5).

Strip/brace fast paths and the interval va_arg gate live in
tests/cpp/test_lint_corpus.cpp. _required_literal tests are deleted with
prism/checkers.py in phase 5.

python -m pytest tests/test_lints_fastpath.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

_SEED_PROBE = textwrap.dedent("""
    import json, sys
    from pathlib import Path
    from prism.checkers import run_lints
    from prism.cparse import extract_functions
    from prism.thread import run_thread
    root = Path(sys.argv[1])
    ps = sorted(root.iterdir())
    fs = run_lints(ps, root, jobs=1)
    fns = [f for p in ps for f in extract_functions(p, str(p))]
    fs += run_thread(fns)
    print(json.dumps([[f.cls, f.line, f.message] for f in fs]))
""")

_MULTI_NAME_CPP = textwrap.dedent("""
    #include <optional>
    int both(void) {
        std::optional<int> zeta;
        std::optional<int> alpha;
        return *zeta + *alpha;
    }
""")

_TWO_GLOBALS_C = textwrap.dedent("""
    #include <pthread.h>
    int alpha;
    int beta;
    int gamma_;
    int delta;
    void *t1(void *a) { alpha += 1; beta += 1; gamma_ += 1; delta += 1; return a; }
    void *t2(void *a) { alpha += 1; beta += 1; gamma_ += 1; delta += 1; return a; }
    int start(void) { pthread_t t; return pthread_create(&t, 0, t1, 0); }
""")


class TestHashSeedIndependent(unittest.TestCase):
    """Set iteration order used to leak into finding order and messages."""

    def _run(self, tmp: Path, seed: str) -> str:
        env = dict(os.environ, PYTHONHASHSEED=seed, PYTHONPATH=str(ROOT))
        res = subprocess.run(
            [sys.executable, "-c", _SEED_PROBE, str(tmp)],
            capture_output=True, text=True, env=env, cwd=ROOT, timeout=300,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        return res.stdout

    def test_same_output_under_every_seed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            (tmp / "multi.cpp").write_text(_MULTI_NAME_CPP)
            (tmp / "globals.c").write_text(_TWO_GLOBALS_C)
            outs = {self._run(tmp, seed) for seed in ("0", "1", "2", "3", "4", "5")}
            self.assertEqual(len(outs), 1, outs)
            out = outs.pop()
        self.assertIn("alpha dereferenced without has_value()", out)
        atom = re.findall(r"global '(\w+)' incremented", out)
        want = ["alpha", "beta", "delta", "gamma_"]
        self.assertEqual(atom, want + want)
        race = re.findall(r"global '(\w+)' written from", out)
        self.assertEqual(race, sorted(race))
        self.assertEqual(len(race), 4)


if __name__ == "__main__":
    unittest.main()
