"""ISO C11 remaining thrd_* plants: NEEDS-HARNESS, never a vacuous proof."""

from __future__ import annotations

import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, bmc_function, unencoded_syntax_reason
from prism.cparse import extract_functions
from prism.thread import run_thread

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

_THRD_REST = (
    "thrd_sleep_unenc_bad",
    "thrd_yield_unenc_bad",
    "thrd_current_unenc_bad",
    "thrd_equal_unenc_bad",
    "thrd_exit_unenc_bad",
    "thrd_join_unenc_bad",
    "thrd_detach_unenc_bad",
)


def _fn(name: str):
    path = TD / "thrd_unenc.c"
    for f in extract_functions(path, path.name):
        if f.name == name:
            return f
    raise AssertionError(name)


class TestIsoThrdRemainingPlants(unittest.TestCase):
    def test_remaining_thrd_calls_are_unencoded_syntax(self):
        for name in _THRD_REST:
            with self.subTest(name=name):
                fn = _fn(name)
                syn = unencoded_syntax_reason(fn, "bitvector BMC")
                self.assertIsNotNone(syn, msg=name)
                self.assertIn("thrd", syn.lower())
                self.assertNotIn("pthread", syn.lower())

    def test_thrd_ok_twin_is_not_unencoded(self):
        syn = unencoded_syntax_reason(_fn("thrd_unenc_ok"), "bitvector BMC")
        self.assertIsNone(syn)

    def test_remaining_thrd_is_not_a_race_finding(self):
        path = TD / "thrd_unenc.c"
        hits = run_thread(extract_functions(path, path.name))
        self.assertFalse([f for f in hits if f.cls == "RACE-SHARED"])

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_remaining_thrd_is_not_a_vacuous_proof(self):
        for name in _THRD_REST:
            with self.subTest(name=name):
                r = bmc_function(_fn(name), 8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertIn("thrd", r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
                )
        r_ok = bmc_function(_fn("thrd_unenc_ok"), 8)
        self.assertIn(
            r_ok.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
            r_ok.message,
        )
        self.assertNotEqual(r_ok.status, laws.NEEDS_HARNESS, r_ok.message)


if __name__ == "__main__":
    unittest.main()
