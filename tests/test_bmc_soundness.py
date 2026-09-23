"""BMC soundness regressions (docs/CONFORMANCE.md "Known issues" S1-S6, F1, R1).

Each function below was once PROVED although it has undefined behaviour for
some input (a wrong proof, roadmap 6.1), or lost its verdict to an encoder
crash. The C++ engine runs the same snippets in tests/cpp/test_main.cpp
("bmc soundness: ...").
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, extract_enums, extract_macros, run_bmc
from prism.cparse import extract_functions


def bmc_source(src: str) -> dict[str, object]:
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "t.c"
        p.write_text(src, encoding="utf-8")
        fns = extract_functions(p, str(p))
        return {f.function: f for f in run_bmc(fns, 8)}


S_SRC = """#include <stdlib.h>
#include <limits.h>
int s1(int a, int b) { if (a < 0) return 0; return a - b; }
int s2(int a, int b) { if (b == 0) return 0; return a % b; }
int s3a(int x) { if (x < -1000 || x > 1000) return 0; return x << 2; }
int s3b(int x) { if (x < 0 || x > 4) return 0; return x << 30; }
int s3c(int x) { if (x < 0 || x > 7) return 0; return 2147483647 << x; }
unsigned s4a(unsigned short a, unsigned short b) { return a * b; }
int s4b(unsigned char c) { return c << 24; }
int s4c(int a, unsigned char b) { return a + b; }
int s4d(unsigned char c, int d) { if (c < -10 || c > 100) return 0; return 20 / d; }
int s5(int a, unsigned s) { if (s < 1 || s > 7) return 0; return (a >> s) - 2147483647; }
int s6a(int x) { return abs(x); }
void sink(int);
int s6c(int d) { sink(100 / d); return 0; }
int s6e(void) { int v = INT_MIN; if (v < 0) return v * 2; return 0; }
int add_neg(int a, int b) { if (a > 0) return 0; return a + b; }
long long mul_ll(long long a) { return a * 3; }
"""

CF_SRC = """static int g(int a) { if (a == 0) return 0; return 1; }
int inl(int a) { int r = g(a); return 10 / r; }
int loopk(int n) { int s = 0; for (int i = 0; i < n; i++) s = i; return 100 / (s - 20); }
int loopb(int n) { int i; if (n < 0) return 0; for (i = 0; i < n && i < 5; i++) { } return 100 / (n - 2); }
int brk(int a) { int x = 2; for (;;) { if (a) break; x = 3; break; } return 10 / (x - 2); }
int cont(int d) { int s = 0; for (int i = 0; i < 3; i++) { if (i == 1) continue; s += 1; } return 10 / (s - 2 + d); }
int dangling(int a, int b) { int x = 1; if (a) if (b) x = 2; else x = 0; return 10 / x; }
unsigned f1(unsigned a, unsigned b) { return b ? a / b : 0; }
int and_guard(int a, int b) { return b > 0 && a / b > 1; }
int kin(int n) { while (n > 0) { n = n - 1; } return n; }
int w(long long p0) { unsigned v0 = 12; v0 &= (p0 + (~p0)); return (int)v0; }
int innocent(int a) { if (a < 0 || a > 10) return 0; return a * 2; }
"""


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestKnownWrongProofs(unittest.TestCase):
    def test_s1_to_s6_are_refuted(self):
        by = bmc_source(S_SRC)
        want = {
            "s1": "INT-SIGNED-OVF", "s2": "INT-SIGNED-OVF",
            "s3a": "INT-SHIFT-UB", "s3b": "INT-SHIFT-UB", "s3c": "INT-SHIFT-UB",
            "s4a": "INT-SIGNED-OVF", "s4b": "INT-SHIFT-UB", "s4c": "INT-SIGNED-OVF",
            "s4d": "INT-DIV-ZERO", "s5": "INT-SIGNED-OVF", "s6a": "INT-SIGNED-OVF",
            "s6c": "INT-DIV-ZERO", "s6e": "INT-SIGNED-OVF",
            "add_neg": "INT-SIGNED-OVF", "mul_ll": "INT-SIGNED-OVF",
        }
        for name, cls in want.items():
            with self.subTest(name):
                f = by[name]
                self.assertEqual(f.status, laws.FAILED, f.message)
                self.assertEqual(f.cls, cls)

    def test_unmodelled_is_never_a_proof(self):
        by = bmc_source(
            "#define SQ(x) ((x) * (x))\n"
            "int b6(int x) { return SQ(x); }\n"
            "int g6(int x) { return UNKNOWN_BOUND + x; }\n"
            "int glob;\n"
            "int h6(int x) { return glob / x; }\n"
        )
        for name in ("b6", "g6"):
            with self.subTest(name):
                self.assertEqual(by[name].status, laws.NEEDS_HARNESS, by[name].message)
                self.assertIn("UNENCODED", by[name].message)
        self.assertFalse(laws.is_proof(by["h6"].status), by["h6"].message)

    def test_control_flow_keeps_every_path(self):
        by = bmc_source(CF_SRC)
        for name in ("inl", "loopb", "brk", "cont", "dangling"):
            with self.subTest(name):
                self.assertEqual(by[name].status, laws.FAILED, by[name].message)
                self.assertEqual(by[name].cls, "INT-DIV-ZERO")
        self.assertFalse(laws.is_proof(by["loopk"].status), by["loopk"].message)
        for name in ("f1", "and_guard", "kin", "w", "innocent"):
            with self.subTest(name):
                self.assertEqual(by[name].status, laws.PROVED_UNBOUNDED, by[name].message)


class TestConstantTables(unittest.TestCase):
    def test_enum_with_uncomputed_value_is_left_out(self):
        d = extract_enums("enum { A = 1 << 3, B, C = 7, D };")
        self.assertNotIn("A", d)
        self.assertNotIn("B", d)  # B = A + 1 is unknown too, never 0 or 1
        self.assertEqual(d["C"], 7)
        self.assertEqual(d["D"], 8)

    def test_enum_octal_and_conflicts(self):
        d = extract_enums("enum { A = 010 }; enum { X = 1 }; enum { X = 2 };")
        self.assertEqual(d["A"], 8)
        self.assertNotIn("X", d)

    def test_object_macros_only(self):
        m = extract_macros(
            "#define LIM 100\n#define SQ(x) ((x)*(x))\n#define TWICE 1\n#define TWICE 2\n"
            "#define GONE 3\n#undef GONE\n"
        )
        self.assertEqual(m, {"LIM": "100"})


if __name__ == "__main__":
    unittest.main()
