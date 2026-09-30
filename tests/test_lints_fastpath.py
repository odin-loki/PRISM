"""Fast paths of the Python engine's lints that have no C++ counterpart.

- _required_literal only names text every match contains.
- Findings do not depend on the string hash seed (set iteration order).

The rest of this file (strip/brace fast paths, the interval gate) moved to
tests/cpp/test_lint_corpus.cpp. These two stay until the Python engine is
deleted: _required_literal exists only in prism/checkers.py, and
PYTHONHASHSEED only affects the Python engine.

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

from prism import checkers
from prism.cparse import strip_comments_keep_lines

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


class TestRequiredLiteral(unittest.TestCase):
    def test_cases(self):
        lit = checkers._required_literal
        self.assertEqual(lit(re.compile(r"\bgetdirentries\s*\(")), "getdirentries")
        self.assertEqual(lit(re.compile(r"(?:foo)+bar")), "foo")
        self.assertIsNone(lit(re.compile(r"memcpy", re.I)))
        self.assertIsNone(lit(re.compile(r"(?i:memcpy)")))
        self.assertIsNone(lit(re.compile(r"(?:malloc|calloc)")))
        self.assertIsNone(lit(re.compile(r"(?:abc)?x")))

    def test_every_match_contains_the_literal(self):
        pats = [p for p in vars(checkers).values() if isinstance(p, re.Pattern)]
        text = "\n".join(
            strip_comments_keep_lines(p.read_text(encoding="utf-8", errors="replace"))
            for p in sorted(TD.glob("*.c*"))
        )
        checked = 0
        for pat in pats:
            want = checkers._required_literal(pat)
            if want is None:
                continue
            for m in pat.finditer(text):
                self.assertIn(want, m.group(0), pat.pattern)
                checked += 1
        self.assertGreater(checked, 100)


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
        # The C++ engine keeps these names in a std::set: sorted order.
        self.assertIn("alpha dereferenced without has_value()", out)
        # One finding per (writer, global): t1's four, then t2's four.
        atom = re.findall(r"global '(\w+)' incremented", out)
        want = ["alpha", "beta", "delta", "gamma_"]
        self.assertEqual(atom, want + want)
        race = re.findall(r"global '(\w+)' written from", out)
        self.assertEqual(race, sorted(race))
        self.assertEqual(len(race), 4)


if __name__ == "__main__":
    unittest.main()
