"""Frama-C WP stage: PROVED-ASSUMING never PROVED. python -m unittest tests.test_wp"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.bmc import HAS_Z3
from prism.cparse import extract_functions
from prism.models import Finding, FunctionInfo
from prism.pipeline import STAGE_ORDER
from prism.wp import encode_predicate, run_wp

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

_WP_PLANTS = (
    TD / "acsl_abs.c",
    TD / "contract_add.c",
    TD / "wp_ptr.c",
    TD / "wp_unenc.c",
)


def fn(name: str):
    for p in _WP_PLANTS:
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f
    raise AssertionError(name)


class TestWp(unittest.TestCase):
    def test_stage_after_contracts(self):
        self.assertIn("wp", STAGE_ORDER)
        self.assertEqual(STAGE_ORDER[STAGE_ORDER.index("contracts") + 1], "wp")

    def test_encode_scalar_ok(self):
        self.assertEqual(encode_predicate("x < 100"), "x < 100")
        self.assertEqual(encode_predicate("result == x+1"), "result == x+1")
        self.assertIsNotNone(encode_predicate("x > -2147483647"))

    def test_encode_acsl_unencodable(self):
        self.assertIsNone(encode_predicate(r"\valid(&x)"))
        self.assertIsNone(encode_predicate(r"result == \old(x) + 1"))
        self.assertIsNone(encode_predicate(r"\forall integer k; k == x"))
        self.assertIsNone(encode_predicate(r"\exists integer k; k == x"))
        self.assertIsNone(encode_predicate(r"\at(x, Pre)"))
        self.assertIsNone(encode_predicate(r"\separated(p, q)"))
        self.assertIsNone(encode_predicate("p->x == 0"))
        self.assertIsNone(encode_predicate("p->x == p->x"))
        self.assertIsNone(encode_predicate("result == foo(x)"))

    def test_inc_qed_assuming_never_proved(self):
        recs = run_wp([fn("inc")], unwind=8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.stage, "wp")
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.BOUNDED)
        self.assertIn("never", r.message.lower())
        extra = r.extra or {}
        self.assertEqual(extra.get("engine"), "prism-wp")
        self.assertEqual(extra.get("wp"), "return-substitution")
        self.assertTrue(extra.get("wp_qed"))

    def test_plain_skipped(self):
        recs = run_wp([fn("acsl_plain")], unwind=4)
        self.assertEqual(recs, [])

    def test_pointer_needs_harness(self):
        recs = run_wp([fn("wp_ptr_get")], unwind=4)
        self.assertTrue(recs)
        self.assertTrue(all(r.status == laws.NEEDS_HARNESS for r in recs))
        self.assertTrue(all(r.status != laws.PROVED for r in recs))
        self.assertTrue(all(r.status != laws.CLEAN for r in recs))
        self.assertEqual(recs[0].stage, "wp")

    def test_void_other_are_needs_harness(self):
        void_fn = FunctionInfo(
            file="x.c", name="v", kind="VOID", line=1,
            signature="void v(void)", params=[], body="return;", return_type="void",
        )
        other_fn = FunctionInfo(
            file="x.c", name="o", kind="OTHER", line=1,
            signature="int o(S s)", params=[("S", "s")], body="return 0;",
        )
        spec = {
            "requires": [], "ensures": ["result == 0"],
            "invariant": [], "decreases": [], "diff": None,
        }
        with mock.patch("prism.wp.parse_comments", return_value=spec):
            void_recs = run_wp([void_fn], unwind=4)
            other_recs = run_wp([other_fn], unwind=4)
        self.assertEqual(void_recs[0].status, laws.NEEDS_HARNESS)
        self.assertEqual(other_recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(void_recs[0].status, laws.PROVED)
        self.assertNotEqual(other_recs[0].status, laws.PROVED_ASSUMING)
        self.assertIn("VOID", void_recs[0].message)
        self.assertIn("OTHER", other_recs[0].message)

    def test_unencodable_valid_is_error(self):
        recs = run_wp([fn("wp_valid_bad")], unwind=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertTrue(r.extra.get("wp_unencoded"))
        self.assertIn("\\valid", r.message)

    def test_unencodable_old_is_error(self):
        recs = run_wp([fn("wp_old_bad")], unwind=4)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.ERROR)
        self.assertNotEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertTrue(recs[0].extra.get("wp_unencoded"))
        self.assertIn("\\old", recs[0].message)

    def test_unencodable_forall_is_error(self):
        recs = run_wp([fn("wp_forall_bad")], unwind=4)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.ERROR)
        self.assertNotEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertTrue(recs[0].extra.get("wp_unencoded"))

    def test_acsl_block_valid_is_error(self):
        recs = run_wp([fn("wp_acsl_valid_bad")], unwind=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertTrue(r.extra.get("wp_unencoded"))
        self.assertIn("\\valid", r.message)

    def test_arrow_member_is_error_not_qed(self):
        recs = run_wp([fn("wp_arrow_bad")], unwind=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertTrue(r.extra.get("wp_unencoded"))
        self.assertIn("->", r.message)

    def test_bmc_proved_is_rewritten_assuming(self):
        fake = Finding(
            stage="contracts", status=laws.PROVED, file="a.c",
            function="acsl_abs", line=1, cls="FUNC-CONTRACT",
            message="proved", strength=laws.STRENGTH_PROVES, extra={},
        )
        with mock.patch("prism.wp.bmc_function_with_assume", return_value=fake):
            recs = run_wp([fn("acsl_abs")], unwind=8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertIn("never", recs[0].message.lower())

    def test_bmc_proved_unbounded_is_rewritten_assuming(self):
        fake = Finding(
            stage="contracts", status=laws.PROVED_UNBOUNDED, file="a.c",
            function="acsl_abs", line=1, cls="FUNC-CONTRACT",
            message="unbounded", strength=laws.STRENGTH_PROVES, extra={},
        )
        with mock.patch("prism.wp.bmc_function_with_assume", return_value=fake):
            recs = run_wp([fn("acsl_abs")], unwind=8)
        self.assertEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertNotEqual(recs[0].status, laws.PROVED_UNBOUNDED)
        self.assertIn("never", recs[0].message.lower())

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_acsl_abs_assuming_never_proved(self):
        recs = run_wp([fn("acsl_abs")], unwind=8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.stage, "wp")
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertIn("never", r.message.lower())
        self.assertEqual((r.extra or {}).get("engine"), "prism-wp")

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_failed_ensures_not_a_proof(self):
        recs = run_wp([fn("not_inc")], unwind=8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.CLEAN)


if __name__ == "__main__":
    unittest.main()
