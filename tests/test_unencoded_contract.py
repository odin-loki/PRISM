"""Source-contract: strcpy stays BMC-only; designated-init regex exact; goto ERROR."""

from __future__ import annotations

import inspect
import re
import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, bmc_function, unencoded_syntax_reason
from prism.concolic import concolic_function
from prism.cparse import extract_functions
from prism.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

# PLAN / C++ test_main: must not match `{ dst[i] =`
DESIGNATED_INIT_RE = r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*="


def _fn(name: str) -> FunctionInfo:
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f
    raise AssertionError(name)


def _scalar(name: str, body: str) -> FunctionInfo:
    return FunctionInfo(
        file="x.c",
        name=name,
        kind="SCALAR",
        line=1,
        signature=f"int {name}(int n)",
        params=[("int", "n")],
        body=body,
    )


class TestUnencodedSyntaxContract(unittest.TestCase):
    def test_strcpy_strcat_sprintf_stay_out_of_unencoded_syntax_reason(self):
        cases = (
            ("strcpy_only", 'strcpy(d, s); return n;'),
            ("strcat_only", 'strcat(d, s); return n;'),
            ("sprintf_only", 'sprintf(d, "%d", n); return n;'),
            ("vsprintf_only", 'vsprintf(d, "%d", ap); return n;'),
            ("gets_only", 'gets(d); return n;'),
        )
        for name, body in cases:
            with self.subTest(name=name):
                syn = unencoded_syntax_reason(_scalar(name, body), "bitvector BMC")
                self.assertIsNone(syn, msg=f"{name} leaked into unencoded_syntax_reason: {syn}")

        copy = _fn("copy_bad")
        self.assertIn("strcpy", copy.body)
        self.assertIsNone(
            unencoded_syntax_reason(copy, "bitvector BMC"),
            msg="strcpy plant must stay BMC-only so the concrete oracle can CRASH",
        )
        cat = _fn("cat_bad")
        self.assertIn("strcat", cat.body)
        self.assertIsNone(
            unencoded_syntax_reason(cat, "bitvector BMC"),
            msg="strcat plant must stay BMC-only so the concrete oracle can CRASH",
        )
        if HAS_Z3:
            for plant in (copy, cat):
                with self.subTest(bmc=plant.name):
                    r = bmc_function(plant, 8)
                    self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                    self.assertNotIn(
                        r.status,
                        {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
                    )

    def test_strlcpy_is_in_unencoded_syntax_reason_not_strcpy(self):
        syn = unencoded_syntax_reason(
            _scalar("sl", "strlcpy(d, s, 4); return n;"),
            "bitvector BMC",
        )
        self.assertIsNotNone(syn)
        self.assertIn("strlcpy", syn.lower())
        self.assertNotIn("strcpy unencoded", (syn or "").lower())

    def test_designated_init_regex_exact(self):
        src = inspect.getsource(unencoded_syntax_reason)
        self.assertIn(DESIGNATED_INIT_RE, src)
        pat = re.compile(DESIGNATED_INIT_RE)
        self.assertTrue(pat.search("{ .x = 1 }"))
        self.assertTrue(pat.search("{ [0] = 1 }"))
        self.assertIsNone(pat.search("{ dst[i] = 1 }"))
        self.assertIsNone(pat.search("{dst[i] = 1}"))
        self.assertIsNone(pat.search("{ dst[0] = n; }"))

        assign = _fn("desig_assign_ok")
        syn = unencoded_syntax_reason(assign, "bitvector BMC")
        self.assertIsNone(syn, msg=syn)
        bad = _fn("desig_init_bad")
        syn_bad = unencoded_syntax_reason(bad, "bitvector BMC")
        self.assertIsNotNone(syn_bad)
        self.assertIn("designated init", syn_bad.lower())

    def test_plain_goto_stays_error_not_harness(self):
        goto = _fn("with_goto")
        self.assertIsNone(
            unencoded_syntax_reason(goto, "bitvector BMC"),
            msg="plain goto must not be NEEDS-HARNESS via unencoded_syntax_reason",
        )
        if HAS_Z3:
            r = bmc_function(goto, 8)
            self.assertEqual(r.status, laws.ERROR, r.message)
            self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
            )
        computed = _fn("computed_goto_bad")
        syn = unencoded_syntax_reason(computed, "bitvector BMC")
        self.assertIsNotNone(syn)
        self.assertIn("computed goto", syn.lower())

    def test_strcat_overflow_still_crashes_in_oracle(self):
        cat = _fn("cat_bad")
        r = concolic_function(cat, budget=8)
        self.assertEqual(r.status, laws.CRASH, r.message)
        self.assertEqual(r.cls, "MEM-OOB-WRITE")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)


if __name__ == "__main__":
    unittest.main()
