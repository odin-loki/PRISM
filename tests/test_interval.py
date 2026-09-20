"""Interval analysis: over-approx FAILED, never a proof.

python -m unittest tests.test_interval
"""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.cparse import extract_functions
from helix.interval import interval_function, run_interval

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f
    raise AssertionError(name)


class TestInterval(unittest.TestCase):
    def test_add_overflow_failed(self):
        rec = interval_function(fn("add_overflow"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_div0_failed(self):
        rec = interval_function(fn("div_param"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-DIV-ZERO")

    def test_shift_failed(self):
        rec = interval_function(fn("shift_ub"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SHIFT-UB")

    def test_abs_ok_is_silent_not_proved(self):
        rec = interval_function(fn("abs_ok"))
        self.assertIsNone(rec)

    def test_saturate_silent(self):
        rec = interval_function(fn("saturate"))
        self.assertIsNone(rec)

    def test_pointer_skipped(self):
        rec = interval_function(fn("null_branch"))
        self.assertIsNone(rec)

    def test_array_decay_skipped_not_proved(self):
        rec = interval_function(fn("arr_esc_bad"))
        self.assertIsNone(rec)

    def test_array_decay_plus_skipped_not_proved(self):
        rec = interval_function(fn("arr_esc_plus0"))
        self.assertIsNone(rec)
        rec2 = interval_function(fn("arr_esc_plusi"))
        self.assertIsNone(rec2)
        rec3 = interval_function(fn("arr_esc_plus_rhs"))
        self.assertIsNone(rec3)

    def test_atomic_qual_skipped_not_proved(self):
        rec = interval_function(fn("atom_qual_bad"))
        self.assertIsNone(rec)

    def test_const_local_skipped_not_proved(self):
        rec = interval_function(fn("const_local_bad"))
        self.assertIsNone(rec)

    def test_struct_local_skipped_not_proved(self):
        rec = interval_function(fn("struct_local_bad"))
        self.assertIsNone(rec)
        rec2 = interval_function(fn("typedef_local_bad"))
        self.assertIsNone(rec2)

    def test_return_paren_buf_skipped_not_proved(self):
        rec = interval_function(fn("arr_esc_paren"))
        self.assertIsNone(rec)

    def test_return_paren_addr_skipped_not_proved(self):
        rec = interval_function(fn("esc_paren_addr"))
        self.assertIsNone(rec)

    def test_storage_class_skipped_not_proved(self):
        rec = interval_function(fn("register_local_bad"))
        self.assertIsNone(rec)
        rec2 = interval_function(fn("auto_type_bad"))
        self.assertIsNone(rec2)
        rec3 = interval_function(fn("auto_type_gnu_bad"))
        self.assertIsNone(rec3)
        rec4 = interval_function(fn("static_local_bad"))
        self.assertIsNone(rec4)
        rec5 = interval_function(fn("extern_local_bad"))
        self.assertIsNone(rec5)

    def test_layout_syntax_skipped_not_proved(self):
        for name in (
            "anon_enum_bad", "alignas_bad", "compound_bad",
        ):
            self.assertIsNone(interval_function(fn(name)))
        rec = interval_function(fn("enum_const_ok"))
        self.assertIsNone(rec)
        rec2 = interval_function(fn("alignas_ok"))
        self.assertIsNone(rec2)
        rec3 = interval_function(fn("compound_ok"))
        self.assertIsNone(rec3)

    def test_const_for_skipped_not_proved(self):
        rec = interval_function(fn("const_for_bad"))
        self.assertIsNone(rec)

    def test_anon_struct_skipped_not_proved(self):
        rec = interval_function(fn("anon_struct_bad"))
        self.assertIsNone(rec)

    def test_run_never_proved(self):
        recs = run_interval([fn("add_overflow"), fn("abs_ok"), fn("saturate")])
        self.assertTrue(any(r.cls == "INT-SIGNED-OVF" for r in recs))
        self.assertFalse(any(r.status in {laws.PROVED, laws.CLEAN} for r in recs))

    def test_abs_ter_failed(self):
        rec = interval_function(fn("abs_ter"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_do_overflow_failed(self):
        rec = interval_function(fn("do_overflow"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_comma_ovf_failed(self):
        rec = interval_function(fn("comma_ovf"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")

    def test_unsigned_add_is_silent_not_proved(self):
        rec = interval_function(fn("add_u"))
        self.assertIsNone(rec)

    def test_trunc_ok_char_literal_is_silent_not_proved(self):
        rec = interval_function(fn("trunc_ok"))
        self.assertIsNone(rec)

    def test_char_literal_evaluates(self):
        from helix.interval import _Engine, _eval

        e = _Engine([])
        r = _eval(e, "'A'")
        self.assertEqual(r.lo, r.hi)
        self.assertEqual(r.lo, ord("A"))

    def test_strcpy_arg_is_silent_not_proved(self):
        rec = interval_function(fn("copy_bad"))
        self.assertIsNone(rec)

    def test_throw_is_silent_not_proved(self):
        path = TD / "throw_dtor.cpp"
        hits = [f for f in extract_functions(path, path.name)
                if f.name == "throws_not_dtor"]
        self.assertTrue(hits)
        rec = interval_function(hits[0])
        self.assertIsNone(rec)

    def test_try_catch_is_silent_not_proved(self):
        path = TD / "try_catch.cpp"
        hits = [f for f in extract_functions(path, path.name)
                if f.name == "try_ok"]
        self.assertTrue(hits)
        rec = interval_function(hits[0])
        self.assertIsNone(rec)

    def test_asm_is_silent_not_proved(self):
        rec = interval_function(fn("asm_vol"))
        self.assertIsNone(rec)

    def test_mod_param_failed(self):
        rec = interval_function(fn("mod_param"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-DIV-ZERO")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_intmin_div_failed(self):
        rec = interval_function(fn("intmin_div"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SIGNED-OVF")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_shift_wide_failed(self):
        rec = interval_function(fn("shift_wide"))
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.cls, "INT-SHIFT-UB")
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_unsigned_local_wrap_is_silent_not_proved(self):
        rec = interval_function(fn("wrap_u_local"))
        self.assertIsNone(rec)

    def test_unsigned_branch_wrap_is_silent_not_proved(self):
        rec = interval_function(fn("wrap_u_branch"))
        self.assertIsNone(rec)

    def test_interval_ops_never_proved_or_clean(self):
        recs = run_interval([
            fn("mod_param"), fn("intmin_div"), fn("shift_wide"),
            fn("wrap_u_local"), fn("wrap_u_branch"),
        ])
        self.assertTrue(any(r.cls == "INT-DIV-ZERO" for r in recs))
        self.assertTrue(any(r.cls == "INT-SIGNED-OVF" for r in recs))
        self.assertTrue(any(r.cls == "INT-SHIFT-UB" for r in recs))
        self.assertFalse(any(r.status in {laws.PROVED, laws.BOUNDED, laws.CLEAN} for r in recs))


if __name__ == "__main__":
    unittest.main()
