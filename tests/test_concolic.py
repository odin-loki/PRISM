"""Concolic engine tests. python -m unittest tests.test_concolic -v"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path
from unittest.mock import patch

from prism import laws
from prism.bmc import HAS_Z3
from prism.concolic import (
    _UNSAT,
    _branch_conditions,
    _neighbor_for_cond,
    _z3_solve_flip,
    concolic_function,
    run_concolic,
)
from prism.cparse import extract_functions

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in list(TD.glob("*.c")) + list(TD.glob("*.cpp")):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestConcolicAPI(unittest.TestCase):
    def test_run_concolic_signature(self):
        sig = inspect.signature(run_concolic)
        self.assertEqual(list(sig.parameters.keys()), ["functions", "budget"])
        self.assertEqual(sig.parameters["budget"].default, 32)
        self.assertIn("list", str(sig.return_annotation))


class TestConcolicPlantedBugs(unittest.TestCase):
    def test_planted_bugs_crash(self):
        cases = [
            ("add_overflow", "INT-SIGNED-OVF"),
            ("div_param", "INT-DIV-ZERO"),
            ("oob_write", "MEM-OOB-WRITE"),
            ("shift_ub", "INT-SHIFT-UB"),
        ]
        for name, cls in cases:
            with self.subTest(name=name):
                f, _ = fn(name)
                r = run_concolic([f], budget=32)[0]
                self.assertEqual(r.stage, "concolic")
                self.assertEqual(r.status, laws.CRASH, r.message)
                self.assertEqual(r.cls, cls)
                self.assertTrue(r.counterexample)
                self.assertEqual(r.strength, laws.STRENGTH_FINDS)
                self.assertFalse(laws.is_proof(r.status))
                self.assertNotIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING})


class TestConcolicPointer(unittest.TestCase):
    def test_null_branch_skipped_not_crash(self):
        f, _ = fn("null_branch")
        rs = run_concolic([f])
        self.assertEqual(len(rs), 1)
        self.assertIn(rs[0].status, {laws.NEEDS_HARNESS})
        self.assertNotEqual(rs[0].status, laws.CRASH)
        self.assertEqual(rs[0].stage, "concolic")

    def test_unchecked_alloc_needs_harness(self):
        f, _ = fn("unchecked_alloc")
        r = run_concolic([f])[0]
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CRASH)

    def test_return_local_array_needs_harness_not_clean(self):
        f, _ = fn("arr_esc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_vla_needs_harness_not_error(self):
        f, _ = fn("vla_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_float_needs_harness_not_error(self):
        f, _ = fn("fdiv_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_recursive_needs_harness_not_clean(self):
        f, _ = fn("rec_id")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_goto_stays_error_never_proof(self):
        f, _ = fn("with_goto")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_taut_bound_is_not_parse_error(self):
        f, _ = fn("taut_bound_bad")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_strcpy_overflow_crashes(self):
        f, _ = fn("copy_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.CRASH, r.message)
        self.assertEqual(r.cls, "MEM-OOB-WRITE")

    def test_trunc_ok_char_lit_is_clean_not_error(self):
        f, _ = fn("trunc_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertEqual(r.status, laws.CLEAN, r.message)

    def test_throw_needs_harness_not_error(self):
        path = TD / "throw_dtor.cpp"
        hits = [f for f in extract_functions(path, str(path))
                if f.name == "throws_not_dtor"]
        self.assertTrue(hits)
        r = concolic_function(hits[0], budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN, laws.CRASH},
        )

    def test_alloca_bad_needs_harness_not_error(self):
        f, _ = fn("alloca_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN, laws.CRASH},
        )

    def test_alloca_ok_needs_harness_not_error(self):
        f, _ = fn("alloca_ok")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN, laws.CRASH},
        )

    def test_longjmp_needs_harness_not_error(self):
        f, _ = fn("jmp_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN, laws.CRASH},
        )

    def test_jmp_ok_without_setjmp_is_clean_not_harness(self):
        f, _ = fn("jmp_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertEqual(r.status, laws.CLEAN, r.message)

    def test_goto_stays_error_not_longjmp_harness(self):
        f, _ = fn("with_goto")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_return_buf_plus_zero_needs_harness_not_clean(self):
        f, _ = fn("arr_esc_plus0")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_return_zero_plus_buf_needs_harness_not_clean(self):
        f, _ = fn("arr_esc_plus_rhs")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_atomic_qual_needs_harness_not_error(self):
        f, _ = fn("atom_qual_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_mutex_object_needs_harness_not_error(self):
        f, _ = fn("lock_init_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_vol_ok_without_qualifier_is_not_harness(self):
        f, _ = fn("vol_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_asm_needs_harness_not_error(self):
        f, _ = fn("asm_nop")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_try_catch_needs_harness_not_error(self):
        path = TD / "try_catch.cpp"
        hits = [f for f in extract_functions(path, str(path))
                if f.name == "try_ok"]
        self.assertTrue(hits)
        r = concolic_function(hits[0], budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN, laws.CRASH},
        )

    def test_generic_needs_harness_not_error(self):
        f, _ = fn("generic_sel")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_const_local_needs_harness_not_error(self):
        f, _ = fn("const_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_const_param_ok_is_not_harness(self):
        f, _ = fn("const_param_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_empty_inf_ok_postfix_is_not_parse_error(self):
        f, _ = fn("empty_inf_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.CRASH, laws.CLEAN})

    def test_struct_local_needs_harness_not_error(self):
        f, _ = fn("struct_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_typedef_local_needs_harness_not_error(self):
        f, _ = fn("typedef_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_return_paren_buf_needs_harness_not_clean(self):
        f, _ = fn("arr_esc_paren")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_return_paren_addr_needs_harness_not_error(self):
        f, _ = fn("esc_paren_addr")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_register_local_needs_harness_not_error(self):
        f, _ = fn("register_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_auto_type_needs_harness_not_error(self):
        f, _ = fn("auto_type_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_auto_type_gnu_needs_harness_not_error(self):
        f, _ = fn("auto_type_gnu_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_static_local_needs_harness_not_error(self):
        f, _ = fn("static_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_extern_local_needs_harness_not_error(self):
        f, _ = fn("extern_local_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_anon_enum_local_needs_harness_not_error(self):
        f, _ = fn("anon_enum_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_enum_const_ok_still_clean_not_proof(self):
        f, _ = fn("enum_const_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertEqual(r.status, laws.CLEAN)

    def test_alignas_needs_harness_not_error(self):
        f, _ = fn("alignas_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_compound_lit_needs_harness_not_error(self):
        f, _ = fn("compound_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_const_for_needs_harness_not_error(self):
        f, _ = fn("const_for_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_anon_struct_needs_harness_not_error(self):
        f, _ = fn("anon_struct_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_memcpy_cstr_needs_harness_not_clean(self):
        f, _ = fn("missing_nul_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_mkstemp_ok_needs_harness_not_error(self):
        f, _ = fn("mkstemp_ok")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_mkstemp_bad_needs_harness_not_clean(self):
        f, _ = fn("mkstemp_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_percent_n_address_of_needs_harness_not_error(self):
        f, _ = fn("percent_n_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_popen_needs_harness_not_error(self):
        for name in ("popen_bad", "popen_ok"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_tls_local_needs_harness_not_error(self):
        for name in ("tls_local_bad", "thread_local_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_tls_local_ok_is_not_harness(self):
        f, _ = fn("tls_local_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_complex_local_needs_harness_not_error(self):
        for name in ("complex_bad", "imaginary_bad", "complex_float_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_complex_ok_is_not_harness(self):
        f, _ = fn("complex_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_typeof_local_needs_harness_not_error(self):
        for name in ("typeof_bad", "typeof_gnu_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_sizeof_ok_is_not_typeof_harness(self):
        f, _ = fn("sizeof_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_nested_fn_needs_harness_not_error(self):
        f, _ = fn("nested_fn_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_nested_fn_ok_is_not_harness(self):
        f, _ = fn("nested_fn_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_computed_goto_needs_harness_not_error(self):
        for name in ("computed_goto_bad", "computed_goto_ptr_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_designated_init_needs_harness_not_error(self):
        for name in ("desig_init_bad", "desig_field_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_designated_init_ok_is_not_harness(self):
        f, _ = fn("desig_init_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_static_assert_ok_is_not_harness(self):
        f, _ = fn("static_assert_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_alignof_needs_harness_not_error(self):
        for name in ("alignof_bad", "alignof_gnu_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_alignof_ok_is_not_alignof_harness(self):
        f, _ = fn("alignof_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_va_arg_needs_harness_not_error(self):
        f, _ = fn("va_arg_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_va_arg_ok_is_not_harness(self):
        f, _ = fn("va_arg_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_range_for_needs_harness_not_error(self):
        f, _ = fn("range_for_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_range_for_ok_is_not_harness(self):
        f, _ = fn("range_for_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_lambda_needs_harness_not_error(self):
        f, _ = fn("lambda_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_lambda_ok_is_not_harness(self):
        f, _ = fn("lambda_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_with_goto_stays_error(self):
        f, _ = fn("with_goto")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_cxx_casts_needs_harness_not_error(self):
        for name in ("dyn_cast_bad", "typeid_bad", "reinterp_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_static_cast_ok_is_not_harness(self):
        f, _ = fn("static_cast_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_packed_needs_harness_not_error(self):
        f, _ = fn("packed_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_packed_ok_is_not_harness(self):
        f, _ = fn("packed_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_coroutine_needs_harness_not_error(self):
        f, _ = fn("coro_await_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_coroutine_ok_is_not_harness(self):
        f, _ = fn("coro_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_label_addr_needs_harness_not_error(self):
        f, _ = fn("label_addr_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_wide_string_needs_harness_not_error(self):
        f, _ = fn("wide_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_wide_ok_is_not_harness(self):
        f, _ = fn("wide_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_bitfield_needs_harness_not_error(self):
        f, _ = fn("bitfield_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_bitfield_ok_is_not_harness(self):
        f, _ = fn("bitfield_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_proc_spawn_needs_harness_not_error(self):
        for name in ("spawn_fork_bad", "spawn_exec_bad"):
            f, _ = fn(name)
            r = concolic_function(f, budget=8)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_mmap_unenc_needs_harness_not_error(self):
        f, _ = fn("mmap_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_wcs_unenc_needs_harness_not_error(self):
        f, _ = fn("wcs_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_int128_unenc_needs_harness_not_error(self):
        f, _ = fn("int128_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_case_range_unenc_needs_harness_not_error(self):
        f, _ = fn("case_range_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_bitcast_unenc_needs_harness_not_error(self):
        f, _ = fn("bitcast_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_ifcx_unenc_needs_harness_not_error(self):
        f, _ = fn("ifcx_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)

    def test_dlopen_unenc_needs_harness_not_error(self):
        f, _ = fn("dlopen_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_clz_unenc_needs_harness_not_error(self):
        f, _ = fn("clz_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_nullptr_unenc_needs_harness_not_error(self):
        f, _ = fn("nullptr_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_launder_unenc_needs_harness_not_error(self):
        f, _ = fn("launder_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_fold_unenc_needs_harness_not_error(self):
        f, _ = fn("fold_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_decimal_unenc_needs_harness_not_error(self):
        f, _ = fn("decimal_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_restrict_unenc_needs_harness_not_error(self):
        f, _ = fn("restrict_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_float16_unenc_needs_harness_not_error(self):
        f, _ = fn("float16_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_lifetime_unenc_needs_harness_not_error(self):
        f, _ = fn("lifetime_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_requires_unenc_needs_harness_not_error(self):
        f, _ = fn("requires_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_choose_unenc_needs_harness_not_error(self):
        f, _ = fn("choose_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_typeof_unqual_unenc_needs_harness_not_error(self):
        f, _ = fn("typeof_unqual_unenc_bad")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
        )

    def test_listen_connect_pipe_unenc_needs_harness_not_error(self):
        for name in (
            "listen_unenc_bad",
            "connect_unenc_bad",
            "pipe_unenc_bad",
            "dup_unenc_bad",
            "fcntl_unenc_bad",
            "waitpid_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_expected_format_spaceship_cleanup_constexpr_unenc_needs_harness(self):
        for name in (
            "expected_unenc_bad",
            "format_unenc_bad",
            "spaceship_unenc_bad",
            "cleanup_unenc_bad",
            "constexpr_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_select_send_kill_addrinfo_unenc_needs_harness_not_error(self):
        for name in (
            "select_unenc_bad",
            "send_unenc_bad",
            "kill_unenc_bad",
            "addrinfo_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_jthread_async_function_mdspan_mutex_vecsize_unenc_needs_harness(self):
        for name in (
            "jthread_unenc_bad",
            "async_unenc_bad",
            "function_unenc_bad",
            "mdspan_unenc_bad",
            "mutex_unenc_bad",
            "vecsize_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_pthread_join_sem_openat_flock_aligned_unenc_needs_harness(self):
        for name in (
            "join_unenc_bad",
            "sem_unenc_bad",
            "openat_unenc_bad",
            "flock_unenc_bad",
            "aligned_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_condvar_atomic_ref_generator_assume_builtin_unenc_needs_harness(self):
        for name in (
            "condvar_unenc_bad",
            "atomic_ref_unenc_bad",
            "generator_unenc_bad",
            "assume_unenc_bad",
            "atomic_builtin_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_opendir_setrlimit_getsockopt_stat_unenc_needs_harness_not_error(self):
        for name in (
            "opendir_unenc_bad",
            "setrlimit_unenc_bad",
            "getsockopt_unenc_bad",
            "stat_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_any_fs_regex_latch_from_chars_visit_unenc_needs_harness(self):
        for name in (
            "any_unenc_bad",
            "fs_unenc_bad",
            "regex_unenc_bad",
            "latch_unenc_bad",
            "from_chars_unenc_bad",
            "visit_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_clock_shm_spawn_glob_fseek_sleep_unenc_needs_harness_not_error(self):
        for name in (
            "clock_unenc_bad",
            "shm_unenc_bad",
            "spawn_unenc_bad",
            "glob_unenc_bad",
            "fseek_unenc_bad",
            "sleep_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_source_stacktrace_stop_flat_map_unenc_needs_harness(self):
        for name in (
            "source_loc_unenc_bad",
            "stacktrace_unenc_bad",
            "stop_token_unenc_bad",
            "flat_map_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_getopt_uname_sendfile_memfd_prctl_tcgetattr_unenc_needs_harness_not_error(self):
        for name in (
            "getopt_unenc_bad",
            "uname_unenc_bad",
            "sendfile_unenc_bad",
            "memfd_unenc_bad",
            "prctl_unenc_bad",
            "tcgetattr_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_chrono_fn_ref_flat_set_ranges_unenc_needs_harness(self):
        for name in (
            "chrono_unenc_bad",
            "fn_ref_unenc_bad",
            "flat_set_unenc_bad",
            "ranges_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_sysconf_getrusage_nftw_wordexp_getlogin_inet_unenc_needs_harness_not_error(self):
        for name in (
            "sysconf_unenc_bad",
            "getrusage_unenc_bad",
            "nftw_unenc_bad",
            "wordexp_unenc_bad",
            "getlogin_unenc_bad",
            "inet_pton_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_hive_execution_indirect_bitset_copyable_unenc_needs_harness(self):
        for name in (
            "hive_unenc_bad",
            "execution_unenc_bad",
            "indirect_unenc_bad",
            "bitset_unenc_bad",
            "copyable_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_mlock_splice_inotify_fsync_getrandom_getline_unenc_needs_harness_not_error(self):
        for name in (
            "mlock_unenc_bad",
            "splice_unenc_bad",
            "inotify_unenc_bad",
            "fsync_unenc_bad",
            "getrandom_unenc_bad",
            "getline_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_to_chars_hazard_text_enc_simd_unenc_needs_harness_not_error(self):
        for name in (
            "to_chars_unenc_bad",
            "hazard_unenc_bad",
            "text_enc_unenc_bad",
            "simd_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_strlcpy_isatty_ptsname_mount_fmemopen_bzero_unenc_needs_harness_not_error(self):
        for name in (
            "strlcpy_unenc_bad",
            "isatty_unenc_bad",
            "ptsname_unenc_bad",
            "mount_unenc_bad",
            "fmemopen_unenc_bad",
            "bzero_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_rcu_linalg_embed_import_unenc_needs_harness_not_error(self):
        for name in (
            "rcu_unenc_bad",
            "linalg_unenc_bad",
            "embed_unenc_bad",
            "import_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_setxattr_sched_aio_iouring_capset_statx_pidfd_unenc_needs_harness_not_error(self):
        for name in (
            "setxattr_unenc_bad",
            "sched_unenc_bad",
            "aio_unenc_bad",
            "iouring_unenc_bad",
            "capset_unenc_bad",
            "statx_unenc_bad",
            "pidfd_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_out_ptr_flat_mmap_spanstream_barrier_task_unenc_needs_harness_not_error(self):
        for name in (
            "out_ptr_unenc_bad",
            "flat_mmap_unenc_bad",
            "spanstream_unenc_bad",
            "barrier_unenc_bad",
            "task_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_fanotify_seccomp_getgrnam_fallocate_close_range_landlock_unenc_needs_harness_not_error(self):
        for name in (
            "fanotify_unenc_bad",
            "seccomp_unenc_bad",
            "getgrnam_unenc_bad",
            "fallocate_unenc_bad",
            "close_range_unenc_bad",
            "landlock_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_osync_packaged_flat_mset_syncbuf_unenc_needs_harness_not_error(self):
        for name in (
            "osync_unenc_bad",
            "packaged_unenc_bad",
            "flat_mset_unenc_bad",
            "syncbuf_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_bpf_userfaultfd_getpass_initgroups_clone_unenc_needs_harness_not_error(self):
        for name in (
            "bpf_unenc_bad",
            "userfaultfd_unenc_bad",
            "getpass_unenc_bad",
            "initgroups_unenc_bad",
            "clone_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_openat2_sendmmsg_name_to_handle_process_madvise_unenc_needs_harness_not_error(self):
        for name in (
            "openat2_unenc_bad",
            "sendmmsg_unenc_bad",
            "name_to_handle_unenc_bad",
            "process_madvise_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_personality_quotactl_getpriority_signalfd_unenc_needs_harness_not_error(self):
        for name in (
            "personality_unenc_bad",
            "quotactl_unenc_bad",
            "getpriority_unenc_bad",
            "signalfd_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_weak_exc_ptr_coro_h_valarray_to_under_unexpect_unenc_needs_harness_not_error(self):
        for name in (
            "weak_ptr_unenc_bad",
            "exc_ptr_unenc_bad",
            "coro_h_unenc_bad",
            "valarray_unenc_bad",
            "to_under_unenc_bad",
            "unexpect_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_pivot_root_membarrier_pkey_statfs_syncfs_prlimit_process_vm_perf_unenc_needs_harness_not_error(self):
        for name in (
            "pivot_root_unenc_bad",
            "membarrier_unenc_bad",
            "pkey_unenc_bad",
            "statfs_unenc_bad",
            "syncfs_unenc_bad",
            "prlimit_unenc_bad",
            "process_vm_unenc_bad",
            "perf_event_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_tuple_deque_fwd_list_list_map_umap_unenc_needs_harness_not_error(self):
        for name in (
            "tuple_unenc_bad",
            "deque_unenc_bad",
            "fwd_list_unenc_bad",
            "list_unenc_bad",
            "map_unenc_bad",
            "umap_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_clone3_kcmp_keyctl_fsopen_process_mrelease_memfd_secret_unenc_needs_harness_not_error(self):
        for name in (
            "clone3_unenc_bad",
            "kcmp_unenc_bad",
            "keyctl_unenc_bad",
            "fsopen_unenc_bad",
            "process_mrelease_unenc_bad",
            "memfd_secret_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_ioprio_mq_open_shmget_futex_adjtimex_sethostname_unenc_needs_harness_not_error(self):
        for name in (
            "ioprio_unenc_bad",
            "mq_open_unenc_bad",
            "shmget_unenc_bad",
            "futex_unenc_bad",
            "adjtimex_unenc_bad",
            "sethostname_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_reboot_swapon_acct_ioperm_mincore_rseq_unenc_needs_harness_not_error(self):
        for name in (
            "reboot_unenc_bad",
            "swapon_unenc_bad",
            "acct_unenc_bad",
            "ioperm_unenc_bad",
            "mincore_unenc_bad",
            "rseq_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_timer_create_semget_msgget_klogctl_mount_setattr_getcpu_unenc_needs_harness_not_error(self):
        for name in (
            "timer_create_unenc_bad",
            "semget_unenc_bad",
            "msgget_unenc_bad",
            "klogctl_unenc_bad",
            "mount_setattr_unenc_bad",
            "getcpu_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_wstring_multimap_multiset_binsem_errc_byteswap_unenc_needs_harness_not_error(self):
        for name in (
            "wstring_unenc_bad",
            "multimap_unenc_bad",
            "mset_unenc_bad",
            "binsem_unenc_bad",
            "errc_unenc_bad",
            "byteswap_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_set_queue_stack_pqueue_array_uset_unenc_needs_harness_not_error(self):
        for name in (
            "set_unenc_bad",
            "queue_unenc_bad",
            "stack_unenc_bad",
            "pqueue_unenc_bad",
            "array_unenc_bad",
            "uset_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_init_module_kexec_quotactl_fd_pkey_free_tgkill_add_key_unenc_needs_harness_not_error(self):
        for name in (
            "init_module_unenc_bad",
            "kexec_unenc_bad",
            "quotactl_fd_unenc_bad",
            "pkey_free_unenc_bad",
            "tgkill_unenc_bad",
            "add_key_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_semctl_msgctl_shmctl_timer_settime_setdomainname_io_submit_unenc_needs_harness_not_error(self):
        for name in (
            "semctl_unenc_bad",
            "msgctl_unenc_bad",
            "shmctl_unenc_bad",
            "timer_settime_unenc_bad",
            "setdomainname_unenc_bad",
            "io_submit_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_cxx_pmr_u8string_ummap_umset_shared_lock_atomic_flag_unenc_needs_harness_not_error(self):
        for name in (
            "pmr_unenc_bad",
            "u8_unenc_bad",
            "ummap_unenc_bad",
            "umset_unenc_bad",
            "slock_unenc_bad",
            "aflag_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_io_setup_request_key_tkill_timer_delete_mq_unlink_shmat_unenc_needs_harness_not_error(self):
        for name in (
            "io_setup_unenc_bad",
            "request_key_unenc_bad",
            "tkill_unenc_bad",
            "timer_delete_unenc_bad",
            "mq_unlink_unenc_bad",
            "shmat_unenc_bad",
            "semop_unenc_bad",
            "msgsnd_unenc_bad",
            "sync_file_range_unenc_bad",
            "msync_unenc_bad",
            "socketpair_unenc_bad",
            "sysinfo_unenc_bad",
            "cvany_unenc_bad",
            "rmutex_unenc_bad",
            "tmutex_unenc_bad",
            "fstream_unenc_bad",
            "tthread_unenc_bad",
            "call_once_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_clock_set_gettid_setsched_setitimer_nice_arch_prctl_unenc_needs_harness_not_error(self):
        for name in (
            "clock_set_unenc_bad",
            "settimeofday_unenc_bad",
            "gettid_unenc_bad",
            "setsched_unenc_bad",
            "setitimer_unenc_bad",
            "nice_unenc_bad",
            "arch_prctl_unenc_bad",
            "getdents_unenc_bad",
            "utimensat_unenc_bad",
            "linkat_unenc_bad",
            "mbind_unenc_bad",
            "futex_waitv_unenc_bad",
            "stmutex_unenc_bad",
            "rtmutex_unenc_bad",
            "syserr_unenc_bad",
            "tzdb_unenc_bad",
            "vzip_unenc_bad",
            "fmtto_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_syslog_setpgid_epoll_yield_move_pages_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "syslog_unenc_bad",
            "setpgid_unenc_bad",
            "setreuid_unenc_bad",
            "getgroups_unenc_bad",
            "epoll_create_unenc_bad",
            "timerfd_settime_unenc_bad",
            "remap_file_pages_unenc_bad",
            "move_pages_unenc_bad",
            "cachestat_unenc_bad",
            "map_shadow_stack_unenc_bad",
            "sched_yield_unenc_bad",
            "setfsuid_unenc_bad",
            "errcat_unenc_bad",
            "nested_unenc_bad",
            "fence_unenc_bad",
            "nexit_unenc_bad",
            "wconv_unenc_bad",
            "invoke_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_wait4_preadv_sendmsg_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "wait4_unenc_bad",
            "preadv_unenc_bad",
            "sendmsg_unenc_bad",
            "getsockname_unenc_bad",
            "epoll_pwait_unenc_bad",
            "inotify_rm_watch_unenc_bad",
            "eventfd_rw_unenc_bad",
            "sched_setattr_unenc_bad",
            "renameat2_unenc_bad",
            "execveat_unenc_bad",
            "mlock2_unenc_bad",
            "faccessat2_unenc_bad",
            "apply_unenc_bad",
            "refwrap_unenc_bad",
            "endian_unenc_bad",
            "bitceil_unenc_bad",
            "uncaught_unenc_bad",
            "vjoin_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_ustat_vhangup_mseal_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "ustat_unenc_bad",
            "vhangup_unenc_bad",
            "mseal_unenc_bad",
            "futex2_unenc_bad",
            "listmount_unenc_bad",
            "lsm_attr_unenc_bad",
            "mempolicy_home_unenc_bad",
            "file_getattr_unenc_bad",
            "setxattrat_unenc_bad",
            "fchmodat2_unenc_bad",
            "sigqueueinfo_unenc_bad",
            "open_tree_attr_unenc_bad",
            "qexit_unenc_bad",
            "toarr_unenc_bad",
            "zoned_unenc_bad",
            "killdep_unenc_bad",
            "rotl_unenc_bad",
            "curexc_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_fadvise_sigaction_sem_open_renameat_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "fadvise_unenc_bad",
            "readahead_unenc_bad",
            "sigaction_unenc_bad",
            "sigprocmask_unenc_bad",
            "sem_open_unenc_bad",
            "rwlock_unenc_bad",
            "pthread_cond_unenc_bad",
            "sigaltstack_unenc_bad",
            "renameat_unenc_bad",
            "faccessat_unenc_bad",
            "fchmodat_unenc_bad",
            "pthread_barrier_unenc_bad",
            "bitw_unenc_bad",
            "lerp_unenc_bad",
            "midpt_unenc_bad",
            "cmpl_unenc_bad",
            "countl_unenc_bad",
            "unreach_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_symlinkat_pthread_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "symlinkat_unenc_bad",
            "unlinkat_unenc_bad",
            "mkdirat_unenc_bad",
            "mknodat_unenc_bad",
            "readlinkat_unenc_bad",
            "fstatat_unenc_bad",
            "spin_unenc_bad",
            "pthread_key_unenc_bad",
            "pthread_cancel_unenc_bad",
            "pthread_kill_unenc_bad",
            "pthread_sigmask_unenc_bad",
            "pthread_atfork_unenc_bad",
            "gcd_unenc_bad",
            "lcm_unenc_bad",
            "clamp_unenc_bad",
            "exch_unenc_bad",
            "toaddr_unenc_bad",
            "ice_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_pledge_ucontext_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "pledge_unenc_bad",
            "unveil_unenc_bad",
            "sysctl_unenc_bad",
            "kqueue_unenc_bad",
            "kevent_unenc_bad",
            "pause_unenc_bad",
            "ppoll_unenc_bad",
            "sigwait_unenc_bad",
            "sigqueue_unenc_bad",
            "ucontext_unenc_bad",
            "sem_timedwait_unenc_bad",
            "pthread_attr_unenc_bad",
            "addrof_unenc_bad",
            "asmalign_unenc_bad",
            "asconst_unenc_bad",
            "exscan_unenc_bad",
            "mkeptr_unenc_bad",
            "setterm_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "cap_enter_unenc_bad",
            "cap_rights_unenc_bad",
            "pdfork_unenc_bad",
            "procctl_unenc_bad",
            "closefrom_unenc_bad",
            "issetugid_unenc_bad",
            "arc4random_unenc_bad",
            "chflags_unenc_bad",
            "getfsstat_unenc_bad",
            "pthread_yield_unenc_bad",
            "sem_trywait_unenc_bad",
            "adjtime_unenc_bad",
            "inscan_unenc_bad",
            "tred_unenc_bad",
            "reduce_unenc_bad",
            "uicopy_unenc_bad",
            "construct_unenc_bad",
            "fwdlike_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover2_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "revoke_unenc_bad",
            "ktrace_unenc_bad",
            "rfork_unenc_bad",
            "jail_unenc_bad",
            "setlogin_unenc_bad",
            "getresuid_unenc_bad",
            "getpeereid_unenc_bad",
            "strtonum_unenc_bad",
            "reallocarray_unenc_bad",
            "timingsafe_unenc_bad",
            "getprogname_unenc_bad",
            "daemon_unenc_bad",
            "uifill_unenc_bad",
            "destroyn_unenc_bad",
            "addsat_unenc_bad",
            "tscan_unenc_bad",
            "typeid_unenc_bad",
            "nontype_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover3_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "cap_fcntls_unenc_bad",
            "pdgetpid_unenc_bad",
            "kldload_unenc_bad",
            "extattr_unenc_bad",
            "mac_unenc_bad",
            "audit_unenc_bad",
            "kvm_unenc_bad",
            "reallocf_unenc_bad",
            "uuidgen_unenc_bad",
            "setfib_unenc_bad",
            "ntp_gettime_unenc_bad",
            "crypt_newhash_unenc_bad",
            "layoutc_unenc_bad",
            "pinter_unenc_bad",
            "uvalue_unenc_bad",
            "bciter_unenc_bad",
            "corrm_unenc_bad",
            "rto_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover4_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "wait6_unenc_bad",
            "cpuset_unenc_bad",
            "rtprio_unenc_bad",
            "kenv_unenc_bad",
            "getfh_unenc_bad",
            "getmntinfo_unenc_bad",
            "nmount_unenc_bad",
            "strmode_unenc_bad",
            "getosreldate_unenc_bad",
            "cap_sandboxed_unenc_bad",
            "getgrouplist_unenc_bad",
            "eaccess_unenc_bad",
            "enumv_unenc_bad",
            "cart_unenc_bad",
            "chunk_unenc_bad",
            "slide_unenc_bad",
            "adjv_unenc_bad",
            "jwith_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover5_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "login_class_unenc_bad",
            "fflags_unenc_bad",
            "getdirentries_unenc_bad",
            "kinfo_unenc_bad",
            "umtx_unenc_bad",
            "thr_unenc_bad",
            "modfind_unenc_bad",
            "lpathconf_unenc_bad",
            "loginclass_unenc_bad",
            "getfsent_unenc_bad",
            "minherit_unenc_bad",
            "cap_getmode_unenc_bad",
            "ztrans_unenc_bad",
            "asrval_unenc_bad",
            "frange_unenc_bad",
            "scenum_unenc_bad",
            "stride_unenc_bad",
            "repeat_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover6_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "nfssvc_unenc_bad",
            "sysarch_unenc_bad",
            "getpagesizes_unenc_bad",
            "sbrk_unenc_bad",
            "ksem_unenc_bad",
            "cap_getrights_unenc_bad",
            "devname_unenc_bad",
            "getbootfile_unenc_bad",
            "kldfirstmod_unenc_bad",
            "fhlink_unenc_bad",
            "valloc_unenc_bad",
            "getdomainname_unenc_bad",
            "takev_unenc_bad",
            "dropv_unenc_bad",
            "filterv_unenc_bad",
            "tview_unenc_bad",
            "elems_unenc_bad",
            "iota_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_leftover7_libc_cxx_unenc_needs_harness_not_error(self):
        for name in (
            "fts_unenc_bad",
            "getvfsbyname_unenc_bad",
            "unmount_unenc_bad",
            "getpagesize_unenc_bad",
            "lio_unenc_bad",
            "cpuclock_unenc_bad",
            "pthcpuclock_unenc_bad",
            "schedprio_unenc_bad",
            "spawnattr_unenc_bad",
            "kld_isloaded_unenc_bad",
            "dlfunc_unenc_bad",
            "typedmem_unenc_bad",
            "twhile_unenc_bad",
            "dwhile_unenc_bad",
            "keys_unenc_bad",
            "vals_unenc_bad",
            "rview_unenc_bad",
            "countv_unenc_bad",
        ):
            with self.subTest(name=name):
                f, _ = fn(name)
                r = concolic_function(f, budget=8)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR)
                self.assertNotEqual(r.status, laws.CLEAN)
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CRASH},
                )

    def test_spaceship_ok_is_not_harness(self):
        f, _ = fn("spaceship_ok")
        r = concolic_function(f, budget=8)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_saturate_clean_not_proof(self):
        f, _ = fn("saturate")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING})
        self.assertIn("not a proof", r.message)

    def test_abs_ok_clean_not_proof(self):
        f, _ = fn("abs_ok")
        r = concolic_function(f, budget=8)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("not a proof", r.message)

    def test_unsigned_add_is_not_signed_ovf_crash(self):
        f, _ = fn("add_u")
        r = concolic_function(f, budget=16)
        self.assertNotEqual(r.status, laws.CRASH, r.message)
        self.assertEqual(r.status, laws.CLEAN, r.message)

    def test_unsigned_guarded_index_is_not_oob_crash(self):
        f, _ = fn("idx_u_ok")
        r = concolic_function(f, budget=16)
        self.assertNotEqual(r.status, laws.CRASH, r.message)
        self.assertEqual(r.status, laws.CLEAN, r.message)

    def test_unsigned_unguarded_index_crashes(self):
        f, _ = fn("idx_u_bad")
        r = concolic_function(f, budget=16)
        self.assertEqual(r.status, laws.CRASH, r.message)
        self.assertIn(r.cls, {"MEM-OOB-READ", "MEM-OOB-WRITE"})

    def test_long_long_extremes_are_not_32bit_overflow(self):
        f, _ = fn("add_ll")
        r = concolic_function(f, budget=16)
        self.assertNotEqual(r.status, laws.CRASH, r.message)


class TestConcolicKleeFork(unittest.TestCase):
    """Mined from KLEE Executor::fork: SMT model of the negated branch."""

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_z3_negated_branch_model_is_new_seed(self):
        f, _ = fn("klee_fork_neg")
        cond = _branch_conditions(f)[0]
        got = _z3_solve_flip(f, {"x": 0}, cond, want=True)
        self.assertIsInstance(got, dict, got)
        self.assertEqual(got["x"], 10)

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_z3_negated_branch_finds_crash_not_proof(self):
        f, _ = fn("klee_fork_neg")
        r = concolic_function(f, budget=32)
        self.assertEqual(r.status, laws.CRASH, r.message)
        self.assertEqual(r.cls, "INT-DIV-ZERO")
        self.assertTrue(r.counterexample)
        self.assertEqual(r.stage, "concolic")
        self.assertEqual(r.strength, laws.STRENGTH_FINDS)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING, laws.FAILED},
        )
        self.assertEqual(r.extra.get("oracle"), "z3")
        self.assertGreater(r.extra.get("z3_seeds", 0), 0)

    def test_z3_missing_keeps_concrete_clean_not_proof(self):
        f, _ = fn("klee_fork_neg")
        with patch("prism.concolic.HAS_Z3", False):
            r = concolic_function(f, budget=32)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertIn("not a proof", r.message)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING, laws.CRASH},
        )
        self.assertEqual(r.extra.get("oracle"), "concrete")

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_unsat_branch_is_skipped_clean_not_proof(self):
        f, _ = fn("klee_fork_unsat")
        cond = _branch_conditions(f)[0]
        got = _z3_solve_flip(f, {"x": 0}, cond, want=True)
        self.assertIs(got, _UNSAT)
        nxt, via_z3, was_unsat = _neighbor_for_cond(f, {"x": 0}, cond)
        self.assertIsNone(nxt)
        self.assertTrue(was_unsat)
        self.assertFalse(via_z3)
        r = concolic_function(f, budget=32)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertIn("not a proof", r.message)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING, laws.CRASH, laws.FAILED},
        )
        self.assertGreater(r.extra.get("skipped_unsat", 0), 0)

    def test_unsat_without_z3_is_clean_not_a_skipped_proof(self):
        """Missing Z3 cannot classify the then-branch as Solver::False."""
        f, _ = fn("klee_fork_unsat")
        with patch("prism.concolic.HAS_Z3", False):
            r = concolic_function(f, budget=32)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertIn("not a proof", r.message)
        self.assertEqual(r.extra.get("skipped_unsat", 0), 0)
        self.assertEqual(r.extra.get("oracle"), "concrete")
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING, laws.CRASH, laws.FAILED},
        )


if __name__ == "__main__":
    unittest.main()
