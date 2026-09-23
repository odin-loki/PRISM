"""Fast paths in the lints / interval / thread stages keep their output.

- strip_comments_keep_lines and _match_brace equal the reference loops.
- _required_literal only names text every match contains.
- Findings do not depend on the string hash seed (set iteration order).
- interval still suppresses an alarm on syntax it does not encode.

python -m pytest tests/test_lints_fastpath.py
"""

from __future__ import annotations

import os
import random
import re
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path

from prism import checkers
from prism.cparse import _match_brace, extract_functions, strip_comments_keep_lines
from prism.interval import interval_function

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def _ref_strip(text: str, blank_strings: bool = True) -> str:
    """The original character loop, kept here as the reference."""
    out: list[str] = []
    i = 0
    n = len(text)
    in_block = False
    while i < n:
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                i += 2
            else:
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        c = text[i]
        if c == "'":
            out.append(c)
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        if c == '"':
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    if blank_strings:
                        out.append(" ")
                        i += 1
                        if i < n:
                            out.append(" ")
                            i += 1
                    else:
                        out.append(text[i])
                        i += 1
                        if i < n:
                            out.append(text[i])
                            i += 1
                    continue
                if blank_strings:
                    out.append("\n" if text[i] == "\n" else " ")
                else:
                    out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _ref_match_brace(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


class TestStripAndBrace(unittest.TestCase):
    def test_strip_equals_reference_on_fuzz(self):
        rng = random.Random(7)
        alphabet = "/*'\"\\\n{}ab ;"
        for _ in range(20000):
            s = "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 24)))
            for blank in (True, False):
                self.assertEqual(
                    strip_comments_keep_lines(s, blank_strings=blank),
                    _ref_strip(s, blank), (s, blank),
                )

    def test_strip_equals_reference_on_testdata(self):
        for p in sorted(TD.glob("*.c*"))[:400]:
            text = p.read_text(encoding="utf-8", errors="replace")
            for blank in (True, False):
                self.assertEqual(
                    strip_comments_keep_lines(text, blank_strings=blank),
                    _ref_strip(text, blank), p.name,
                )

    def test_strip_edge_cases(self):
        # Delimiters vanish, the interior keeps its newlines.
        self.assertEqual(strip_comments_keep_lines("a/*x\ny*/b"), "a \n b")
        self.assertEqual(strip_comments_keep_lines('s = "a\\"b";'), 's = "    ";')
        # Escaped newline in a string is two spaces, as it always was.
        self.assertEqual(strip_comments_keep_lines('"\\\nx"'), '"   "')
        self.assertEqual(strip_comments_keep_lines("c = '\"'; // q"), "c = '\"';     ")
        self.assertEqual(strip_comments_keep_lines("x /* open"), "x      ")

    def test_match_brace_equals_reference(self):
        rng = random.Random(11)
        for _ in range(5000):
            s = "".join(rng.choice("{}a") for _ in range(rng.randint(0, 16)))
            for i in range(-len(s), len(s) + 2):
                self.assertEqual(_match_brace(s, i), _ref_match_brace(s, i), (s, i))


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


class TestIntervalGate(unittest.TestCase):
    def _fn(self, name: str):
        for f in extract_functions(TD / "vaarg_unenc.c", "vaarg_unenc.c"):
            if f.name == name:
                return f
        raise AssertionError(name)

    def test_unencoded_alarm_still_suppressed(self):
        # The engine alone alarms on `x + n`; va_arg is unencoded, so no finding.
        self.assertIsNone(interval_function(self._fn("vaarg_unenc_bad")))
        self.assertIsNone(interval_function(self._fn("vaarg_unenc_ok")))


if __name__ == "__main__":
    unittest.main()
