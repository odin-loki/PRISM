"""BMC frontend: switch/enum/loops plus existing scalar oracles."""

from __future__ import annotations

import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, bmc_function, extract_enums
from prism.cparse import extract_functions
from prism.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def load(name: str):
    for p in list(TD.glob("*.c")) + list(TD.glob("*.cpp")):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


def bmc(name: str, unwind: int = 8):
    f, p = load(name)
    enums = extract_enums(p.read_text(encoding="utf-8"))
    return bmc_function(f, unwind, enums=enums), f, p


class TestExtractEnums(unittest.TestCase):
    def test_explicit_values(self):
        d = extract_enums("enum { A = 1, B = 2 };")
        self.assertEqual(d["A"], 1)
        self.assertEqual(d["B"], 2)

    def test_fsm_file(self):
        d = extract_enums((TD / "fsm.c").read_text(encoding="utf-8"))
        self.assertEqual(d["ST_IDLE"], 0)
        self.assertEqual(d["ST_WORK"], 1)
        self.assertEqual(d["ST_BAD"], 2)

    def test_masked_file(self):
        d = extract_enums((TD / "masked_switch.c").read_text(encoding="utf-8"))
        self.assertEqual(d["A"], 1)
        self.assertEqual(d["B"], 2)


class TestMissingZ3IsNotrun(unittest.TestCase):
    def test_bmc_function_missing_z3_is_notrun_never_raises(self):
        from unittest.mock import patch
        from prism.models import FunctionInfo

        fn = FunctionInfo(
            file="abs_ok.c",
            name="abs_ok",
            kind="SCALAR",
            line=1,
            signature="int abs_ok(int x)",
            params=[("int", "x")],
            body="return x < 0 ? -x : x;",
        )
        with patch("prism.bmc.HAS_Z3", False):
            rec = bmc_function(fn, unwind=8)
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertIn("z3", rec.message.lower())
        self.assertEqual((rec.extra or {}).get("install"), "pip install z3-solver")


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestBMC(unittest.TestCase):
    def test_overflow_failed(self):
        r, _, _ = bmc("add_overflow")
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")
        self.assertTrue(r.counterexample)

    def test_div0_failed(self):
        r, _, _ = bmc("div_param")
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-DIV-ZERO")

    def test_oob_failed(self):
        r, _, _ = bmc("oob_write")
        self.assertEqual(r.status, laws.FAILED)
        self.assertIn(r.cls, {"MEM-OOB-WRITE", "MEM-OOB-READ"})

    def test_abs_proved(self):
        r, _, _ = bmc("abs_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_switch_fsm_not_error(self):
        r, _, _ = bmc("fsm_step")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(
            r.status,
            {laws.FAILED, laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_switch_masked_not_error(self):
        r, _, _ = bmc("masked_switch")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(
            r.status,
            {laws.FAILED, laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_switch_int_cases_proved(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="sw",
            kind="SCALAR",
            line=1,
            signature="int sw(int x)",
            params=[("int", "x")],
            body="""
            int y;
            y = 0;
            switch (x) {
            case 1: y = 1; break;
            case 2: y = 2; break;
            default: y = 3; break;
            }
            return y;
            """,
        )
        r = bmc_function(fn, 8, enums={})
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_enum_arg_used_in_case(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="en",
            kind="SCALAR",
            line=1,
            signature="int en(int x)",
            params=[("int", "x")],
            body="""
            int y;
            y = 0;
            switch (x) {
            case A: y = 1; break;
            case B: y = 2; break;
            default: y = 0; break;
            }
            return y;
            """,
        )
        r = bmc_function(fn, 8, enums={"A": 1, "B": 2})
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_loop_prove(self):
        r, _, _ = bmc("loop_prove")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_loop_overflow(self):
        r, _, _ = bmc("loop_overflow")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_bare_return_is_not_frontend_crash(self):
        r, _, _ = bmc("taut_bound_ok")
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertIn(r.status, {
            laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED,
        })

    def test_do_once_proved(self):
        r, _, _ = bmc("do_once")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_do_overflow_failed(self):
        r, _, _ = bmc("do_overflow")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_sizeof_int_not_error(self):
        r, _, _ = bmc("sz_int")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_sizeof_array_not_error(self):
        r, _, _ = bmc("sz_arr")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_ternary_pick_proved(self):
        r, _, _ = bmc("pick")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_ternary_abs_overflow(self):
        r, _, _ = bmc("abs_ter")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_char_literal_and_cast_not_frontend_crash(self):
        r, _, _ = bmc("trunc_ok")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})
        r2, _, _ = bmc("trunc_bad")
        self.assertNotEqual(r2.status, laws.ERROR, r2.message)

    def test_continue_skips_overflowing_tail(self):
        r, _, _ = bmc("cont_skip")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.FAILED, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_incremental_fails_at_k1_for_loop_free(self):
        r, _, _ = bmc("add_overflow")
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.extra.get("incremental_k"), 1)
        self.assertEqual(r.extra.get("param_premise"), "named")

    def test_comma_sum_proved(self):
        r, _, _ = bmc("comma_sum")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_comma_ovf_failed(self):
        r, _, _ = bmc("comma_ovf")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_goto_unencoded_error(self):
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertIn("goto", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_unsigned_add_wrap_is_not_signed_ovf(self):
        r, _, _ = bmc("add_u")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.FAILED, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_unsigned_index_guard_proved(self):
        r, _, _ = bmc("idx_u_ok")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_unsigned_index_oob_failed(self):
        r, _, _ = bmc("idx_u_bad")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertIn(r.cls, {"MEM-OOB-READ", "MEM-OOB-WRITE"})

    def test_unsigned_lt_zero_is_dead_not_ub(self):
        r, _, _ = bmc("dead_u")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.FAILED, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_vla_needs_harness_not_proof(self):
        r, _, _ = bmc("vla_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_vla_ok_constant_array_not_harness(self):
        r, _, _ = bmc("vla_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED})

    def test_nested_ovf_failed(self):
        r, _, _ = bmc("nested_ovf")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_nested_ok_not_error(self):
        r, _, _ = bmc("nested_ok")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_long_long_add_overflows_at_64(self):
        r, _, _ = bmc("add_ll")
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_long_long_guarded_add_proved(self):
        r, _, _ = bmc("add_ll_ok")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_other_needs_harness_not_error(self):
        r, _, _ = bmc("move_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestKInduction(unittest.TestCase):
    def test_closed_step_is_proved_unbounded(self):
        from prism.bmc import k_induction
        f, _ = load("kinduct_closed")
        rec = k_induction(f, 8)
        self.assertEqual(rec.status, laws.PROVED_UNBOUNDED, rec.message)
        self.assertEqual(rec.extra.get("k_induction"), "closed")
        self.assertEqual(rec.extra.get("k_induction_k"), 1)
        raw, _, _ = bmc("kinduct_closed")
        self.assertEqual(raw.status, laws.BOUNDED, raw.message)

    def test_open_step_stays_bounded_never_failed(self):
        from prism.bmc import k_induction
        f, _ = load("kinduct_step_open")
        rec = k_induction(f, 8)
        self.assertEqual(rec.status, laws.BOUNDED, rec.message)
        self.assertNotEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.extra.get("k_induction"), "step-open")
        self.assertEqual(rec.extra.get("k_induction_tried"), [1, 2])

    def test_nested_ok_never_failed(self):
        from prism.bmc import k_induction
        f, _ = load("nested_ok")
        raw, _, _ = bmc("nested_ok")
        rec = k_induction(f, 8)
        self.assertNotEqual(rec.status, laws.FAILED, rec.message)
        if raw.status == laws.PROVED_UNBOUNDED:
            self.assertEqual(rec.status, laws.PROVED_UNBOUNDED, rec.message)
            self.assertEqual(rec.extra.get("k_induction"), "not-needed")
        elif raw.status == laws.BOUNDED:
            self.assertIn(rec.status, {laws.BOUNDED, laws.PROVED_UNBOUNDED}, rec.message)
            if rec.status == laws.BOUNDED:
                self.assertEqual(rec.extra.get("k_induction"), "unencoded")

    def test_nested_ovf_not_proved_unbounded(self):
        from prism.bmc import k_induction
        f, _ = load("nested_ovf")
        raw, _, _ = bmc("nested_ovf")
        self.assertEqual(raw.status, laws.FAILED, raw.message)
        rec = k_induction(f, 8)
        self.assertEqual(rec.status, laws.FAILED, rec.message)
        self.assertEqual(rec.extra.get("k_induction"), "not-needed")


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestLocalPointerHarness(unittest.TestCase):
    def test_unchecked_alloc_needs_harness_not_error(self):
        r, _, _ = bmc("unchecked_alloc")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_oob_write_still_scalar_array(self):
        r, f, _ = bmc("oob_write")
        self.assertEqual(f.kind, "SCALAR")
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "MEM-OOB-WRITE")

    def test_taint_run_needs_harness(self):
        r, _, _ = bmc("run")
        self.assertEqual(r.status, laws.NEEDS_HARNESS)

    def test_return_address_of_local_needs_harness_not_error(self):
        r, _, _ = bmc("esc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_return_local_array_needs_harness_not_proof(self):
        r, _, _ = bmc("arr_esc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_return_array_element_is_not_decay_harness(self):
        r, _, _ = bmc("write_slot")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertIn(r.cls, {"MEM-OOB-WRITE", "MEM-OOB-READ", "UNINIT-READ"})

    def test_system_literal_is_not_a_vacuous_proof(self):
        r, _, _ = bmc("ok")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_exit_is_not_a_vacuous_proof(self):
        r, _, _ = bmc("fatal")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_pthread_create_is_not_a_vacuous_proof(self):
        r, _, _ = bmc("start")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_iso_thrd_create_is_not_a_vacuous_proof(self):
        r, _, _ = bmc("iso_thrd_start")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thrd", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )
        r, _, _ = bmc("thrd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thrd", r.message.lower())
        self.assertNotIn("pthread", r.message.lower())

    def test_recursive_identity_needs_harness_not_proof(self):
        r, _, _ = bmc("rec_id")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("recursive", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_recursive_add_needs_harness_not_fake_overflow(self):
        r, _, _ = bmc("rec_add")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_nonrecursive_base_still_proved(self):
        r, _, _ = bmc("rec_ok_base")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_float_unencoded_needs_harness_not_error(self):
        r, _, _ = bmc("fdiv_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_cxx_string_view_needs_harness_not_error(self):
        r, _, _ = bmc("view_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_strcpy_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("copy_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_snprintf_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("copy_ok")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_throw_unencoded_needs_harness_not_error(self):
        r, _, _ = bmc("throws_not_dtor")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_taut_bound_bad_is_not_frontend_error(self):
        r, _, _ = bmc("taut_bound_bad")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {
            laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED,
        })

    def test_new_delete_still_needs_harness_not_proof(self):
        r, _, _ = bmc("new_mismatch_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.ERROR},
        )

    def test_alloca_bad_needs_harness_not_proof(self):
        r, _, _ = bmc("alloca_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_alloca_ok_needs_harness_not_proof(self):
        r, _, _ = bmc("alloca_ok")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_builtin_alloca_without_ptr_decl_needs_harness(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="stack_frame",
            kind="VOID",
            line=1,
            signature="void stack_frame(void)",
            params=[],
            body="__builtin_alloca(16);\n",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_longjmp_needs_harness_not_error(self):
        r, _, _ = bmc("jmp_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_jmp_ok_without_setjmp_is_not_harness(self):
        r, _, _ = bmc("jmp_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_va_start_needs_harness_not_error(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="va_bad",
            kind="SCALAR",
            line=1,
            signature="int va_bad(int n)",
            params=[("int", "n")],
            body="va_list ap; va_start(ap, n); return n;\n",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_goto_is_error_not_longjmp_harness(self):
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertIn("goto", r.message.lower())
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_return_buf_plus_zero_needs_harness_not_proof(self):
        r, _, _ = bmc("arr_esc_plus0")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_return_buf_plus_i_needs_harness_not_fake_ovf(self):
        r, _, _ = bmc("arr_esc_plusi")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.ERROR},
        )

    def test_asm_needs_harness_not_proof(self):
        r, _, _ = bmc("asm_nop")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_asm_volatile_needs_harness_not_error(self):
        r, _, _ = bmc("asm_vol")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_generic_needs_harness_not_proof(self):
        r, _, _ = bmc("generic_sel")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_stmt_expr_needs_harness_not_error(self):
        r, _, _ = bmc("stmt_expr_id")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_try_catch_needs_harness_not_error(self):
        r, _, _ = bmc("try_ok")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_offsetof_needs_harness_not_vacuous_proof(self):
        r, _, _ = bmc("off_field")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_printf_void_is_not_a_vacuous_proof(self):
        r, _, _ = bmc("fmt_print")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_printf_mixed_with_overflow_still_encodes_scalar(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="print_then_add",
            kind="SCALAR",
            line=1,
            signature="int print_then_add(int x)",
            params=[("int", "x")],
            body='printf("%d", x);\nreturn x + 1;\n',
        )
        r = bmc_function(fn, 8, enums={})
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_atomic_qual_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("atom_qual_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )
        self.assertIn("memory-model", r.message)

    def test_vol_ok_without_qualifier_is_not_harness(self):
        r, _, _ = bmc("vol_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_volatile_decl_needs_harness_not_error(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="vol_qual_bad",
            kind="VOID",
            line=1,
            signature="void vol_qual_bad(void)",
            params=[],
            body="volatile int y;\ny = 1;\n",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_mutex_object_needs_harness_not_error(self):
        r, _, _ = bmc("lock_init_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        r2, _, _ = bmc("lock_init_static_ok")
        self.assertEqual(r2.status, laws.NEEDS_HARNESS, r2.message)
        self.assertNotEqual(r2.status, laws.ERROR)
        r3, _, _ = bmc("mtx_init_bad")
        self.assertEqual(r3.status, laws.NEEDS_HARNESS, r3.message)
        self.assertNotEqual(r3.status, laws.ERROR)
        r, _, _ = bmc("arr_esc_plus_rhs")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED, laws.ERROR},
        )

    def test_new_without_ptr_decl_needs_harness_not_error(self):
        r, _, _ = bmc("new_no_ptr")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_builtin_unreachable_vacuous_is_not_a_proof(self):
        r, _, _ = bmc("unreach_only")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_builtin_trap_vacuous_is_not_a_proof(self):
        r, _, _ = bmc("trap_only")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_builtin_trap_mixed_with_overflow_still_encodes_scalar(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="trap_then_add",
            kind="SCALAR",
            line=1,
            signature="int trap_then_add(int x)",
            params=[("int", "x")],
            body="__builtin_trap();\nreturn x + 1;\n",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_const_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("const_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("const", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_const_int_parameter_still_proves(self):
        r, _, _ = bmc("const_param_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_abs_ok_const_int_param_still_proves(self):
        f, _ = load("abs_ok")
        fn = FunctionInfo(
            file=f.file, name=f.name, kind="SCALAR", line=f.line,
            signature="int abs_ok(const int x)",
            params=[("const int", "x")],
            body=f.body,
        )
        r = bmc_function(fn, 8, enums={})
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_struct_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("struct_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("layout", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_typedef_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("typedef_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("layout", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_return_paren_buf_needs_harness_not_proof(self):
        r, _, _ = bmc("arr_esc_paren")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED, laws.ERROR},
        )

    def test_return_paren_addr_needs_harness_not_error(self):
        r, _, _ = bmc("esc_paren_addr")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_register_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("register_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("register", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_auto_storage_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("auto_storage_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_auto_type_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("auto_type_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_auto_type_gnu_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("auto_type_gnu_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("__auto_type", r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_static_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("static_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("storage-duration", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_extern_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("extern_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("storage-duration", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_anon_enum_local_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("anon_enum_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("layout", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_enum_const_ok_still_proves(self):
        r, _, _ = bmc("enum_const_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_alignas_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("alignas_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("alignas", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_alignas_ok_still_proves(self):
        r, _, _ = bmc("alignas_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_compound_lit_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("compound_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("compound", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_compound_ok_still_proves(self):
        r, _, _ = bmc("compound_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_const_for_header_needs_harness_not_error(self):
        r, _, _ = bmc("const_for_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("const", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_const_cast_still_proves(self):
        r, _, _ = bmc("const_cast_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_anon_struct_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("anon_struct_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("layout", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_mkstemp_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("mkstemp_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_memcpy_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("missing_nul_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_mkstemp_ok_array_init_is_not_a_proof(self):
        r, _, _ = bmc("mkstemp_ok")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_tmpnam_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("tmpnam_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_percent_n_address_of_needs_harness_not_error(self):
        r, _, _ = bmc("percent_n_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_chroot_unencoded_is_not_a_proof(self):
        r, _, _ = bmc("chroot_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
        )

    def test_popen_is_needs_harness_not_error(self):
        for name in ("popen_bad", "popen_ok"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
            )

    def test_umask_srand_unencoded_is_not_a_proof(self):
        for name in ("umask_bad", "umask_ok", "srand_bad", "srand_ok"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
            )

    def test_signal_unencoded_is_not_a_proof(self):
        for name in ("signal_bad", "signal_ok", "signal_dfl_ok"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
            )

    def test_const_cast_unencoded_is_not_a_proof(self):
        for name in ("cxx_cv_write_bad", "cxx_cv_write_ok"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.CLEAN},
            )

    def test_for_int_header_still_encodes(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="fori",
            kind="SCALAR",
            line=1,
            signature="int fori(void)",
            params=[],
            body="""
            int s;
            s = 0;
            for (int i = 0; i < 2; i++)
                s = s + 1;
            return s;
            """,
        )
        r = bmc_function(fn, 8, enums={})
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_tls_local_needs_harness_not_error_or_proof(self):
        for name in ("tls_local_bad", "thread_local_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("tls", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_tls_local_ok_still_proves(self):
        r, _, _ = bmc("tls_local_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_complex_local_needs_harness_not_error_or_proof(self):
        for name in ("complex_bad", "imaginary_bad", "complex_float_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("complex", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_complex_ok_still_proves(self):
        r, _, _ = bmc("complex_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_typeof_local_needs_harness_not_error_or_proof(self):
        for name in ("typeof_bad", "typeof_gnu_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("typeof", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_sizeof_ok_still_proves_not_typeof_harness(self):
        r, _, _ = bmc("sizeof_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_nested_fn_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("nested_fn_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("nested", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_nested_fn_ok_still_proves(self):
        r, _, _ = bmc("nested_fn_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_computed_goto_needs_harness_not_error_or_proof(self):
        for name in ("computed_goto_bad", "computed_goto_ptr_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("computed goto", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_designated_init_needs_harness_not_error_or_proof(self):
        for name in ("desig_init_bad", "desig_field_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("designated init", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_designated_init_ok_still_proves(self):
        r, _, _ = bmc("desig_init_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_static_assert_ok_still_proves(self):
        r, _, _ = bmc("static_assert_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_alignof_needs_harness_not_error_or_proof(self):
        for name in ("alignof_bad", "alignof_gnu_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("alignof", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_alignof_ok_still_proves_not_alignof_harness(self):
        r, _, _ = bmc("alignof_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_va_arg_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("va_arg_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("va_arg", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_va_arg_ok_still_proves(self):
        r, _, _ = bmc("va_arg_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_range_for_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("range_for_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("range-for", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_range_for_ok_still_proves(self):
        r, _, _ = bmc("range_for_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_lambda_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("lambda_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("lambda", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_lambda_ok_still_proves(self):
        r, _, _ = bmc("lambda_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_with_goto_stays_error(self):
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertIn("goto", r.message.lower())
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)

    def test_cxx_casts_needs_harness_not_error_or_proof(self):
        for name in ("dyn_cast_bad", "typeid_bad", "reinterp_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_static_cast_ok_still_proves(self):
        r, _, _ = bmc("static_cast_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_packed_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("packed_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("packed", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_packed_ok_still_proves(self):
        r, _, _ = bmc("packed_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_coroutine_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("coro_await_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("coroutine", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_coroutine_ok_still_proves(self):
        r, _, _ = bmc("coro_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_label_addr_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("label_addr_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("label-address", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_wide_string_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("wide_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("wide", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_wide_ok_still_proves(self):
        r, _, _ = bmc("wide_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_bitfield_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("bitfield_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("layout", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_bitfield_ok_still_proves(self):
        r, _, _ = bmc("bitfield_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_proc_spawn_needs_harness_not_error_or_proof(self):
        for name in ("spawn_fork_bad", "spawn_exec_bad"):
            r, _, _ = bmc(name)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR, r.message)
            self.assertIn("spawn", r.message.lower())
            self.assertNotIn(
                r.status,
                {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
            )

    def test_spawn_ok_still_proves(self):
        r, _, _ = bmc("spawn_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_mmap_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("mmap_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("mmap", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_mmap_unenc_ok_still_proves(self):
        r, _, _ = bmc("mmap_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_wcs_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("wcs_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("wcscpy", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_int128_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("int128_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("128", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_int128_ok_still_proves(self):
        r, _, _ = bmc("int128_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_case_range_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("case_range_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("case-range", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_case_range_ok_still_proves(self):
        r, _, _ = bmc("case_range_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_bitcast_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("bitcast_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("bit_cast", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_bitcast_ok_still_proves(self):
        r, _, _ = bmc("bitcast_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_ifcx_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("ifcx_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("if constexpr", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_ifcx_ok_still_proves(self):
        r, _, _ = bmc("ifcx_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_dlopen_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("dlopen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("dlopen", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_dlopen_unenc_ok_still_proves(self):
        r, _, _ = bmc("dlopen_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_clz_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("clz_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("clz", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_clz_ok_still_proves(self):
        r, _, _ = bmc("clz_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_nullptr_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("nullptr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("nullptr", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_nullptr_ok_still_proves(self):
        r, _, _ = bmc("nullptr_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_launder_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("launder_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("launder", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_launder_ok_still_proves(self):
        r, _, _ = bmc("launder_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_fold_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("fold_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("fold", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_fold_ok_still_proves(self):
        r, _, _ = bmc("fold_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_decimal_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("decimal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("decimal", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_decimal_ok_still_proves(self):
        r, _, _ = bmc("decimal_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_restrict_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("restrict_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("restrict", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_restrict_ok_still_proves(self):
        r, _, _ = bmc("restrict_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_float16_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("float16_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("ieee", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_float16_ok_still_proves(self):
        r, _, _ = bmc("float16_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_lifetime_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("lifetime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("lifetime", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_lifetime_ok_still_proves(self):
        r, _, _ = bmc("lifetime_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_requires_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("requires_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("concept", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_requires_ok_still_proves(self):
        r, _, _ = bmc("requires_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_choose_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("choose_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("choose", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_choose_ok_still_proves(self):
        r, _, _ = bmc("choose_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_typeof_unqual_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("typeof_unqual_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("typeof_unqual", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_typeof_unqual_ok_still_proves(self):
        r, _, _ = bmc("typeof_unqual_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_listen_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("listen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("listen", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_listen_unenc_ok_still_proves(self):
        r, _, _ = bmc("listen_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_connect_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("connect_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("connect", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_connect_unenc_ok_still_proves(self):
        r, _, _ = bmc("connect_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_pipe_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("pipe_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("pipe", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_pipe_unenc_ok_still_proves(self):
        r, _, _ = bmc("pipe_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_dup_fcntl_waitpid_unenc_needs_harness_not_error_or_proof(self):
        for name, needle in (
            ("dup_unenc_bad", "dup"),
            ("fcntl_unenc_bad", "fcntl"),
            ("waitpid_unenc_bad", "wait"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_dup_fcntl_waitpid_ok_still_proves(self):
        for name in ("dup_unenc_ok", "fcntl_unenc_ok", "waitpid_unenc_ok"):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_expected_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("expected_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("expected", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_expected_ok_still_proves(self):
        r, _, _ = bmc("expected_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_format_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("format_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("format", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_format_unenc_ok_still_proves(self):
        r, _, _ = bmc("format_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_spaceship_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("spaceship_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("spaceship", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_spaceship_ok_still_proves(self):
        r, _, _ = bmc("spaceship_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cleanup_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("cleanup_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("cleanup", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_cleanup_ok_still_proves(self):
        r, _, _ = bmc("cleanup_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_constexpr_unenc_needs_harness_not_error_or_proof(self):
        r, _, _ = bmc("constexpr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("constexpr", r.message.lower())
        self.assertNotIn("if constexpr", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_constexpr_ok_still_proves(self):
        r, _, _ = bmc("constexpr_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_select_send_kill_addrinfo_unenc_needs_harness_not_error_or_proof(self):
        for name, needle in (
            ("select_unenc_bad", "select"),
            ("send_unenc_bad", "send"),
            ("kill_unenc_bad", "kill"),
            ("addrinfo_unenc_bad", "addrinfo"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_select_send_kill_addrinfo_ok_still_proves(self):
        for name in (
            "select_unenc_ok",
            "send_unenc_ok",
            "kill_unenc_ok",
            "addrinfo_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_jthread_async_function_mdspan_mutex_vecsize_unenc_needs_harness(self):
        for name, needle in (
            ("jthread_unenc_bad", "jthread"),
            ("async_unenc_bad", "async"),
            ("function_unenc_bad", "function"),
            ("mdspan_unenc_bad", "mdspan"),
            ("mutex_unenc_bad", "mutex"),
            ("vecsize_unenc_bad", "vector_size"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_jthread_async_function_mdspan_mutex_vecsize_ok_still_proves(self):
        for name in (
            "jthread_unenc_ok",
            "async_unenc_ok",
            "function_unenc_ok",
            "mdspan_unenc_ok",
            "mutex_unenc_ok",
            "vecsize_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_pthread_join_sem_openat_flock_aligned_unenc_needs_harness(self):
        for name, needle in (
            ("join_unenc_bad", "join"),
            ("sem_unenc_bad", "sem"),
            ("openat_unenc_bad", "openat"),
            ("flock_unenc_bad", "flock"),
            ("aligned_unenc_bad", "aligned"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_pthread_join_sem_openat_flock_aligned_ok_still_proves(self):
        for name in (
            "join_unenc_ok",
            "sem_unenc_ok",
            "openat_unenc_ok",
            "flock_unenc_ok",
            "aligned_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_condvar_atomic_ref_generator_assume_builtin_unenc_needs_harness(self):
        for name, needle in (
            ("condvar_unenc_bad", "condition_variable"),
            ("atomic_ref_unenc_bad", "atomic_ref"),
            ("generator_unenc_bad", "generator"),
            ("assume_unenc_bad", "assume"),
            ("atomic_builtin_unenc_bad", "atomic"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_condvar_atomic_ref_generator_assume_builtin_ok_still_proves(self):
        for name in (
            "condvar_unenc_ok",
            "atomic_ref_unenc_ok",
            "generator_unenc_ok",
            "assume_unenc_ok",
            "atomic_builtin_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_opendir_setrlimit_getsockopt_stat_unenc_needs_harness(self):
        for name, needle in (
            ("opendir_unenc_bad", "opendir"),
            ("setrlimit_unenc_bad", "setrlimit"),
            ("getsockopt_unenc_bad", "getsockopt"),
            ("stat_unenc_bad", "stat"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_opendir_setrlimit_getsockopt_stat_unenc_ok_still_proves(self):
        for name in (
            "opendir_unenc_ok",
            "setrlimit_unenc_ok",
            "getsockopt_unenc_ok",
            "stat_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_any_fs_regex_latch_from_chars_visit_unenc_needs_harness(self):
        for name, needle in (
            ("any_unenc_bad", "any"),
            ("fs_unenc_bad", "filesystem"),
            ("regex_unenc_bad", "regex"),
            ("latch_unenc_bad", "latch"),
            ("from_chars_unenc_bad", "from_chars"),
            ("visit_unenc_bad", "visit"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_any_fs_regex_latch_from_chars_visit_unenc_ok_still_proves(self):
        for name in (
            "any_unenc_ok",
            "fs_unenc_ok",
            "regex_unenc_ok",
            "latch_unenc_ok",
            "from_chars_unenc_ok",
            "visit_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_clock_shm_spawn_glob_fseek_sleep_unenc_needs_harness(self):
        for name, needle in (
            ("clock_unenc_bad", "clock_gettime"),
            ("shm_unenc_bad", "shm"),
            ("spawn_unenc_bad", "posix_spawn"),
            ("glob_unenc_bad", "glob"),
            ("fseek_unenc_bad", "fseek"),
            ("sleep_unenc_bad", "sleep"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_clock_shm_spawn_glob_fseek_sleep_unenc_ok_still_proves(self):
        for name in (
            "clock_unenc_ok",
            "shm_unenc_ok",
            "spawn_unenc_ok",
            "glob_unenc_ok",
            "fseek_unenc_ok",
            "sleep_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_source_stacktrace_stop_flat_map_unenc_needs_harness(self):
        for name, needle in (
            ("source_loc_unenc_bad", "source_location"),
            ("stacktrace_unenc_bad", "stacktrace"),
            ("stop_token_unenc_bad", "stop_token"),
            ("flat_map_unenc_bad", "flat_map"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_source_stacktrace_stop_flat_map_unenc_ok_still_proves(self):
        for name in (
            "source_loc_unenc_ok",
            "stacktrace_unenc_ok",
            "stop_token_unenc_ok",
            "flat_map_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_getopt_uname_sendfile_memfd_prctl_tcgetattr_unenc_needs_harness(self):
        for name, needle in (
            ("getopt_unenc_bad", "getopt"),
            ("uname_unenc_bad", "uname"),
            ("sendfile_unenc_bad", "sendfile"),
            ("memfd_unenc_bad", "memfd"),
            ("prctl_unenc_bad", "prctl"),
            ("tcgetattr_unenc_bad", "tcgetattr"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_getopt_uname_sendfile_memfd_prctl_tcgetattr_unenc_ok_still_proves(self):
        for name in (
            "getopt_unenc_ok",
            "uname_unenc_ok",
            "sendfile_unenc_ok",
            "memfd_unenc_ok",
            "prctl_unenc_ok",
            "tcgetattr_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_chrono_fn_ref_flat_set_ranges_unenc_needs_harness(self):
        for name, needle in (
            ("chrono_unenc_bad", "chrono"),
            ("fn_ref_unenc_bad", "function_ref"),
            ("flat_set_unenc_bad", "flat_set"),
            ("ranges_unenc_bad", "views"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_chrono_fn_ref_flat_set_ranges_unenc_ok_still_proves(self):
        for name in (
            "chrono_unenc_ok",
            "fn_ref_unenc_ok",
            "flat_set_unenc_ok",
            "ranges_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_sysconf_getrusage_nftw_wordexp_getlogin_inet_unenc_needs_harness(self):
        for name, needle in (
            ("sysconf_unenc_bad", "sysconf"),
            ("getrusage_unenc_bad", "getrusage"),
            ("nftw_unenc_bad", "nftw"),
            ("wordexp_unenc_bad", "wordexp"),
            ("getlogin_unenc_bad", "getlogin"),
            ("inet_pton_unenc_bad", "inet_pton"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_sysconf_getrusage_nftw_wordexp_getlogin_inet_unenc_ok_still_proves(self):
        for name in (
            "sysconf_unenc_ok",
            "getrusage_unenc_ok",
            "nftw_unenc_ok",
            "wordexp_unenc_ok",
            "getlogin_unenc_ok",
            "inet_pton_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_hive_execution_indirect_bitset_copyable_unenc_needs_harness(self):
        for name, needle in (
            ("hive_unenc_bad", "hive"),
            ("execution_unenc_bad", "execution"),
            ("indirect_unenc_bad", "indirect"),
            ("bitset_unenc_bad", "bitset"),
            ("copyable_unenc_bad", "move_only_function"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_hive_execution_indirect_bitset_copyable_unenc_ok_still_proves(self):
        for name in (
            "hive_unenc_ok",
            "execution_unenc_ok",
            "indirect_unenc_ok",
            "bitset_unenc_ok",
            "copyable_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_mlock_splice_inotify_fsync_getrandom_getline_unenc_needs_harness(self):
        for name, needle in (
            ("mlock_unenc_bad", "mlock"),
            ("splice_unenc_bad", "splice"),
            ("inotify_unenc_bad", "inotify"),
            ("fsync_unenc_bad", "fsync"),
            ("getrandom_unenc_bad", "getrandom"),
            ("getline_unenc_bad", "getline"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_mlock_splice_inotify_fsync_getrandom_getline_unenc_ok_still_proves(self):
        for name in (
            "mlock_unenc_ok",
            "splice_unenc_ok",
            "inotify_unenc_ok",
            "fsync_unenc_ok",
            "getrandom_unenc_ok",
            "getline_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_to_chars_hazard_text_enc_simd_unenc_needs_harness(self):
        for name, needle in (
            ("to_chars_unenc_bad", "to_chars"),
            ("hazard_unenc_bad", "hazard"),
            ("text_enc_unenc_bad", "text_encoding"),
            ("simd_unenc_bad", "simd"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_to_chars_hazard_text_enc_simd_unenc_ok_still_proves(self):
        for name in (
            "to_chars_unenc_ok",
            "hazard_unenc_ok",
            "text_enc_unenc_ok",
            "simd_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_strlcpy_isatty_ptsname_mount_fmemopen_bzero_unenc_needs_harness(self):
        for name, needle in (
            ("strlcpy_unenc_bad", "strlcpy"),
            ("isatty_unenc_bad", "isatty"),
            ("ptsname_unenc_bad", "ptsname"),
            ("mount_unenc_bad", "mount"),
            ("fmemopen_unenc_bad", "fmemopen"),
            ("bzero_unenc_bad", "explicit_bzero"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_strlcpy_isatty_ptsname_mount_fmemopen_bzero_unenc_ok_still_proves(self):
        for name in (
            "strlcpy_unenc_ok",
            "isatty_unenc_ok",
            "ptsname_unenc_ok",
            "mount_unenc_ok",
            "fmemopen_unenc_ok",
            "bzero_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_rcu_linalg_embed_import_unenc_needs_harness(self):
        for name, needle in (
            ("rcu_unenc_bad", "rcu"),
            ("linalg_unenc_bad", "linalg"),
            ("embed_unenc_bad", "embed"),
            ("import_unenc_bad", "import"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_rcu_linalg_embed_import_unenc_ok_still_proves(self):
        for name in (
            "rcu_unenc_ok",
            "linalg_unenc_ok",
            "embed_unenc_ok",
            "import_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_setxattr_sched_aio_iouring_capset_statx_pidfd_unenc_needs_harness(self):
        for name, needle in (
            ("setxattr_unenc_bad", "setxattr"),
            ("sched_unenc_bad", "sched"),
            ("aio_unenc_bad", "aio"),
            ("iouring_unenc_bad", "io_uring"),
            ("capset_unenc_bad", "capset"),
            ("statx_unenc_bad", "statx"),
            ("pidfd_unenc_bad", "pidfd"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_setxattr_sched_aio_iouring_capset_statx_pidfd_unenc_ok_still_proves(self):
        for name in (
            "setxattr_unenc_ok",
            "sched_unenc_ok",
            "aio_unenc_ok",
            "iouring_unenc_ok",
            "capset_unenc_ok",
            "statx_unenc_ok",
            "pidfd_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_out_ptr_flat_mmap_spanstream_barrier_task_unenc_needs_harness(self):
        for name, needle in (
            ("out_ptr_unenc_bad", "out_ptr"),
            ("flat_mmap_unenc_bad", "flat_multimap"),
            ("spanstream_unenc_bad", "spanstream"),
            ("barrier_unenc_bad", "barrier"),
            ("task_unenc_bad", "task"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_out_ptr_flat_mmap_spanstream_barrier_task_unenc_ok_still_proves(self):
        for name in (
            "out_ptr_unenc_ok",
            "flat_mmap_unenc_ok",
            "spanstream_unenc_ok",
            "barrier_unenc_ok",
            "task_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_fanotify_seccomp_getgrnam_fallocate_close_range_landlock_unenc_needs_harness(self):
        for name, needle in (
            ("fanotify_unenc_bad", "fanotify"),
            ("seccomp_unenc_bad", "seccomp"),
            ("getgrnam_unenc_bad", "getgrnam"),
            ("fallocate_unenc_bad", "fallocate"),
            ("close_range_unenc_bad", "close_range"),
            ("landlock_unenc_bad", "landlock"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_fanotify_seccomp_getgrnam_fallocate_close_range_landlock_unenc_ok_still_proves(self):
        for name in (
            "fanotify_unenc_ok",
            "seccomp_unenc_ok",
            "getgrnam_unenc_ok",
            "fallocate_unenc_ok",
            "close_range_unenc_ok",
            "landlock_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_osync_packaged_flat_mset_syncbuf_unenc_needs_harness(self):
        for name, needle in (
            ("osync_unenc_bad", "osyncstream"),
            ("packaged_unenc_bad", "packaged_task"),
            ("flat_mset_unenc_bad", "flat_multiset"),
            ("syncbuf_unenc_bad", "syncbuf"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_osync_packaged_flat_mset_syncbuf_unenc_ok_still_proves(self):
        for name in (
            "osync_unenc_ok",
            "packaged_unenc_ok",
            "flat_mset_unenc_ok",
            "syncbuf_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_bpf_userfaultfd_getpass_initgroups_clone_unenc_needs_harness(self):
        for name, needle in (
            ("bpf_unenc_bad", "bpf"),
            ("userfaultfd_unenc_bad", "userfaultfd"),
            ("getpass_unenc_bad", "getpass"),
            ("initgroups_unenc_bad", "initgroups"),
            ("clone_unenc_bad", "unshare"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_bpf_userfaultfd_getpass_initgroups_clone_unenc_ok_still_proves(self):
        for name in (
            "bpf_unenc_ok",
            "userfaultfd_unenc_ok",
            "getpass_unenc_ok",
            "initgroups_unenc_ok",
            "clone_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_openat2_sendmmsg_name_to_handle_process_madvise_unenc_needs_harness(self):
        for name, needle in (
            ("openat2_unenc_bad", "openat2"),
            ("sendmmsg_unenc_bad", "sendmmsg"),
            ("name_to_handle_unenc_bad", "name_to_handle"),
            ("process_madvise_unenc_bad", "process_madvise"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_openat2_sendmmsg_name_to_handle_process_madvise_unenc_ok_still_proves(self):
        for name in (
            "openat2_unenc_ok",
            "sendmmsg_unenc_ok",
            "name_to_handle_unenc_ok",
            "process_madvise_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_personality_quotactl_getpriority_signalfd_unenc_needs_harness(self):
        for name, needle in (
            ("personality_unenc_bad", "personality"),
            ("quotactl_unenc_bad", "quotactl"),
            ("getpriority_unenc_bad", "getpriority"),
            ("signalfd_unenc_bad", "signalfd"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_personality_quotactl_getpriority_signalfd_unenc_ok_still_proves(self):
        for name in (
            "personality_unenc_ok",
            "quotactl_unenc_ok",
            "getpriority_unenc_ok",
            "signalfd_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_weak_exc_ptr_coro_h_valarray_to_under_unexpect_unenc_needs_harness(self):
        for name, needle in (
            ("weak_ptr_unenc_bad", "weak_ptr"),
            ("exc_ptr_unenc_bad", "exception_ptr"),
            ("coro_h_unenc_bad", "coroutine_handle"),
            ("valarray_unenc_bad", "valarray"),
            ("to_under_unenc_bad", "to_underlying"),
            ("unexpect_unenc_bad", "unexpected"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_weak_exc_ptr_coro_h_valarray_to_under_unexpect_unenc_ok_still_proves(self):
        for name in (
            "weak_ptr_unenc_ok",
            "exc_ptr_unenc_ok",
            "coro_h_unenc_ok",
            "valarray_unenc_ok",
            "to_under_unenc_ok",
            "unexpect_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_pivot_root_membarrier_pkey_statfs_syncfs_prlimit_process_vm_perf_unenc_needs_harness(self):
        for name, needle in (
            ("pivot_root_unenc_bad", "pivot_root"),
            ("membarrier_unenc_bad", "membarrier"),
            ("pkey_unenc_bad", "pkey_alloc"),
            ("statfs_unenc_bad", "statfs"),
            ("syncfs_unenc_bad", "syncfs"),
            ("prlimit_unenc_bad", "prlimit"),
            ("process_vm_unenc_bad", "process_vm_readv"),
            ("perf_event_unenc_bad", "perf_event_open"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_pivot_root_membarrier_pkey_statfs_syncfs_prlimit_process_vm_perf_unenc_ok_still_proves(self):
        for name in (
            "pivot_root_unenc_ok",
            "membarrier_unenc_ok",
            "pkey_unenc_ok",
            "statfs_unenc_ok",
            "syncfs_unenc_ok",
            "prlimit_unenc_ok",
            "process_vm_unenc_ok",
            "perf_event_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_tuple_deque_fwd_list_list_map_umap_unenc_needs_harness(self):
        for name, needle in (
            ("tuple_unenc_bad", "tuple"),
            ("deque_unenc_bad", "deque"),
            ("fwd_list_unenc_bad", "forward_list"),
            ("list_unenc_bad", "std::list"),
            ("map_unenc_bad", "std::map"),
            ("umap_unenc_bad", "unordered_map"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_tuple_deque_fwd_list_list_map_umap_unenc_ok_still_proves(self):
        for name in (
            "tuple_unenc_ok",
            "deque_unenc_ok",
            "fwd_list_unenc_ok",
            "list_unenc_ok",
            "map_unenc_ok",
            "umap_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_clone3_kcmp_keyctl_fsopen_process_mrelease_memfd_secret_unenc_needs_harness(self):
        for name, needle in (
            ("clone3_unenc_bad", "clone3"),
            ("kcmp_unenc_bad", "kcmp"),
            ("keyctl_unenc_bad", "keyctl"),
            ("fsopen_unenc_bad", "fsopen"),
            ("process_mrelease_unenc_bad", "process_mrelease"),
            ("memfd_secret_unenc_bad", "memfd_secret"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_clone3_kcmp_keyctl_fsopen_process_mrelease_memfd_secret_unenc_ok_still_proves(self):
        for name in (
            "clone3_unenc_ok",
            "kcmp_unenc_ok",
            "keyctl_unenc_ok",
            "fsopen_unenc_ok",
            "process_mrelease_unenc_ok",
            "memfd_secret_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_ioprio_mq_open_shmget_futex_adjtimex_sethostname_unenc_needs_harness(self):
        for name, needle in (
            ("ioprio_unenc_bad", "ioprio"),
            ("mq_open_unenc_bad", "mq_open"),
            ("shmget_unenc_bad", "shmget"),
            ("futex_unenc_bad", "futex"),
            ("adjtimex_unenc_bad", "adjtimex"),
            ("sethostname_unenc_bad", "sethostname"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_ioprio_mq_open_shmget_futex_adjtimex_sethostname_unenc_ok_still_proves(self):
        for name in (
            "ioprio_unenc_ok",
            "mq_open_unenc_ok",
            "shmget_unenc_ok",
            "futex_unenc_ok",
            "adjtimex_unenc_ok",
            "sethostname_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_reboot_swapon_acct_ioperm_mincore_rseq_unenc_needs_harness(self):
        for name, needle in (
            ("reboot_unenc_bad", "reboot"),
            ("swapon_unenc_bad", "swapon"),
            ("acct_unenc_bad", "acct"),
            ("ioperm_unenc_bad", "ioperm"),
            ("mincore_unenc_bad", "mincore"),
            ("rseq_unenc_bad", "rseq"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_reboot_swapon_acct_ioperm_mincore_rseq_unenc_ok_still_proves(self):
        for name in (
            "reboot_unenc_ok",
            "swapon_unenc_ok",
            "acct_unenc_ok",
            "ioperm_unenc_ok",
            "mincore_unenc_ok",
            "rseq_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_timer_create_semget_msgget_klogctl_mount_setattr_getcpu_unenc_needs_harness(self):
        for name, needle in (
            ("timer_create_unenc_bad", "timer_create"),
            ("semget_unenc_bad", "semget"),
            ("msgget_unenc_bad", "msgget"),
            ("klogctl_unenc_bad", "klogctl"),
            ("mount_setattr_unenc_bad", "mount_setattr"),
            ("getcpu_unenc_bad", "getcpu"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_timer_create_semget_msgget_klogctl_mount_setattr_getcpu_unenc_ok_still_proves(self):
        for name in (
            "timer_create_unenc_ok",
            "semget_unenc_ok",
            "msgget_unenc_ok",
            "klogctl_unenc_ok",
            "mount_setattr_unenc_ok",
            "getcpu_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_wstring_multimap_multiset_binsem_errc_byteswap_unenc_needs_harness(self):
        for name, needle in (
            ("wstring_unenc_bad", "wstring"),
            ("multimap_unenc_bad", "multimap"),
            ("mset_unenc_bad", "multiset"),
            ("binsem_unenc_bad", "binary_semaphore"),
            ("errc_unenc_bad", "error_code"),
            ("byteswap_unenc_bad", "byteswap"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_wstring_multimap_multiset_binsem_errc_byteswap_unenc_ok_still_proves(self):
        for name in (
            "wstring_unenc_ok",
            "multimap_unenc_ok",
            "mset_unenc_ok",
            "binsem_unenc_ok",
            "errc_unenc_ok",
            "byteswap_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_set_queue_stack_pqueue_array_uset_unenc_needs_harness(self):
        for name, needle in (
            ("set_unenc_bad", "set"),
            ("queue_unenc_bad", "queue"),
            ("stack_unenc_bad", "stack"),
            ("pqueue_unenc_bad", "priority_queue"),
            ("array_unenc_bad", "array"),
            ("uset_unenc_bad", "unordered_set"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_set_queue_stack_pqueue_array_uset_unenc_ok_still_proves(self):
        for name in (
            "set_unenc_ok",
            "queue_unenc_ok",
            "stack_unenc_ok",
            "pqueue_unenc_ok",
            "array_unenc_ok",
            "uset_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_init_module_kexec_quotactl_fd_pkey_free_tgkill_add_key_unenc_needs_harness(self):
        for name, needle in (
            ("init_module_unenc_bad", "init_module"),
            ("kexec_unenc_bad", "kexec"),
            ("quotactl_fd_unenc_bad", "quotactl_fd"),
            ("pkey_free_unenc_bad", "pkey_free"),
            ("tgkill_unenc_bad", "tgkill"),
            ("add_key_unenc_bad", "add_key"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_init_module_kexec_quotactl_fd_pkey_free_tgkill_add_key_unenc_ok_still_proves(self):
        for name in (
            "init_module_unenc_ok",
            "kexec_unenc_ok",
            "quotactl_fd_unenc_ok",
            "pkey_free_unenc_ok",
            "tgkill_unenc_ok",
            "add_key_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_semctl_msgctl_shmctl_timer_settime_setdomainname_io_submit_unenc_needs_harness(self):
        for name, needle in (
            ("semctl_unenc_bad", "semctl"),
            ("msgctl_unenc_bad", "msgctl"),
            ("shmctl_unenc_bad", "shmctl"),
            ("timer_settime_unenc_bad", "timer_settime"),
            ("setdomainname_unenc_bad", "setdomainname"),
            ("io_submit_unenc_bad", "io_submit"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_semctl_msgctl_shmctl_timer_settime_setdomainname_io_submit_unenc_ok_still_proves(self):
        for name in (
            "semctl_unenc_ok",
            "msgctl_unenc_ok",
            "shmctl_unenc_ok",
            "timer_settime_unenc_ok",
            "setdomainname_unenc_ok",
            "io_submit_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_pmr_u8string_ummap_umset_shared_lock_atomic_flag_unenc_needs_harness(self):
        for name, needle in (
            ("pmr_unenc_bad", "pmr"),
            ("u8_unenc_bad", "u8string"),
            ("ummap_unenc_bad", "unordered_multimap"),
            ("umset_unenc_bad", "unordered_multiset"),
            ("slock_unenc_bad", "shared_lock"),
            ("aflag_unenc_bad", "atomic_flag"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_pmr_u8string_ummap_umset_shared_lock_atomic_flag_unenc_ok_still_proves(self):
        for name in (
            "pmr_unenc_ok",
            "u8_unenc_ok",
            "ummap_unenc_ok",
            "umset_unenc_ok",
            "slock_unenc_ok",
            "aflag_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_io_setup_request_key_tkill_timer_delete_mq_unlink_shmat_unenc_needs_harness(self):
        for name, needle in (
            ("io_setup_unenc_bad", "io_setup"),
            ("request_key_unenc_bad", "request_key"),
            ("tkill_unenc_bad", "tkill"),
            ("timer_delete_unenc_bad", "timer_delete"),
            ("mq_unlink_unenc_bad", "mq_unlink"),
            ("shmat_unenc_bad", "shmat"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_io_setup_request_key_tkill_timer_delete_mq_unlink_shmat_unenc_ok_still_proves(self):
        for name in (
            "io_setup_unenc_ok",
            "request_key_unenc_ok",
            "tkill_unenc_ok",
            "timer_delete_unenc_ok",
            "mq_unlink_unenc_ok",
            "shmat_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_semop_msgsnd_sync_file_range_msync_socketpair_sysinfo_unenc_needs_harness(self):
        for name, needle in (
            ("semop_unenc_bad", "semop"),
            ("msgsnd_unenc_bad", "msgsnd"),
            ("sync_file_range_unenc_bad", "sync_file_range"),
            ("msync_unenc_bad", "msync"),
            ("socketpair_unenc_bad", "socketpair"),
            ("sysinfo_unenc_bad", "sysinfo"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_semop_msgsnd_sync_file_range_msync_socketpair_sysinfo_unenc_ok_still_proves(self):
        for name in (
            "semop_unenc_ok",
            "msgsnd_unenc_ok",
            "sync_file_range_unenc_ok",
            "msync_unenc_ok",
            "socketpair_unenc_ok",
            "sysinfo_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_cvany_rmutex_tmutex_fstream_tthread_call_once_unenc_needs_harness(self):
        for name, needle in (
            ("cvany_unenc_bad", "condition_variable_any"),
            ("rmutex_unenc_bad", "recursive_mutex"),
            ("tmutex_unenc_bad", "timed_mutex"),
            ("fstream_unenc_bad", "fstream"),
            ("tthread_unenc_bad", "this_thread"),
            ("call_once_unenc_bad", "call_once"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_cvany_rmutex_tmutex_fstream_tthread_call_once_unenc_ok_still_proves(self):
        for name in (
            "cvany_unenc_ok",
            "rmutex_unenc_ok",
            "tmutex_unenc_ok",
            "fstream_unenc_ok",
            "tthread_unenc_ok",
            "call_once_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_io_submit_tgkill_condvar_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("io_submit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_submit", r.message.lower())
        r, _, _ = bmc("tgkill_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("tgkill", r.message.lower())
        r, _, _ = bmc("condvar_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("condition_variable", r.message.lower())
        self.assertNotIn("condition_variable_any", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_clock_set_settimeofday_gettid_setsched_setitimer_nice_unenc_needs_harness(self):
        for name, needle in (
            ("clock_set_unenc_bad", "clock_settime"),
            ("settimeofday_unenc_bad", "settimeofday"),
            ("gettid_unenc_bad", "gettid"),
            ("setsched_unenc_bad", "sched_setscheduler"),
            ("setitimer_unenc_bad", "setitimer"),
            ("nice_unenc_bad", "nice"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_clock_set_settimeofday_gettid_setsched_setitimer_nice_unenc_ok_still_proves(self):
        for name in (
            "clock_set_unenc_ok",
            "settimeofday_unenc_ok",
            "gettid_unenc_ok",
            "setsched_unenc_ok",
            "setitimer_unenc_ok",
            "nice_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_arch_prctl_getdents_utimensat_linkat_mbind_futex_waitv_unenc_needs_harness(self):
        for name, needle in (
            ("arch_prctl_unenc_bad", "arch_prctl"),
            ("getdents_unenc_bad", "getdents"),
            ("utimensat_unenc_bad", "utimensat"),
            ("linkat_unenc_bad", "linkat"),
            ("mbind_unenc_bad", "mbind"),
            ("futex_waitv_unenc_bad", "futex_waitv"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_arch_prctl_getdents_utimensat_linkat_mbind_futex_waitv_unenc_ok_still_proves(self):
        for name in (
            "arch_prctl_unenc_ok",
            "getdents_unenc_ok",
            "utimensat_unenc_ok",
            "linkat_unenc_ok",
            "mbind_unenc_ok",
            "futex_waitv_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_stmutex_rtmutex_syserr_tzdb_vzip_fmtto_unenc_needs_harness(self):
        for name, needle in (
            ("stmutex_unenc_bad", "shared_timed_mutex"),
            ("rtmutex_unenc_bad", "recursive_timed_mutex"),
            ("syserr_unenc_bad", "system_error"),
            ("tzdb_unenc_bad", "tzdb"),
            ("vzip_unenc_bad", "zip"),
            ("fmtto_unenc_bad", "format_to"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_stmutex_rtmutex_syserr_tzdb_vzip_fmtto_unenc_ok_still_proves(self):
        for name in (
            "stmutex_unenc_ok",
            "rtmutex_unenc_ok",
            "syserr_unenc_ok",
            "tzdb_unenc_ok",
            "vzip_unenc_ok",
            "fmtto_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_futex_clock_rmutex_tmutex_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("futex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("futex", r.message.lower())
        r, _, _ = bmc("clock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("clock_gettime", r.message.lower())
        r, _, _ = bmc("rmutex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("recursive_mutex", r.message.lower())
        self.assertNotIn("recursive_timed_mutex", r.message.lower())
        r, _, _ = bmc("tmutex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timed_mutex", r.message.lower())
        self.assertNotIn("shared_timed_mutex", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_syslog_setpgid_setreuid_getgroups_epoll_create_timerfd_unenc_needs_harness(self):
        for name, needle in (
            ("syslog_unenc_bad", "syslog"),
            ("setpgid_unenc_bad", "setpgid"),
            ("setreuid_unenc_bad", "setreuid"),
            ("getgroups_unenc_bad", "getgroups"),
            ("epoll_create_unenc_bad", "epoll_create"),
            ("timerfd_settime_unenc_bad", "timerfd_settime"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_syslog_setpgid_setreuid_getgroups_epoll_create_timerfd_unenc_ok_still_proves(self):
        for name in (
            "syslog_unenc_ok",
            "setpgid_unenc_ok",
            "setreuid_unenc_ok",
            "getgroups_unenc_ok",
            "epoll_create_unenc_ok",
            "timerfd_settime_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_remap_move_pages_cachestat_shadow_yield_setfsuid_unenc_needs_harness(self):
        for name, needle in (
            ("remap_file_pages_unenc_bad", "remap_file_pages"),
            ("move_pages_unenc_bad", "move_pages"),
            ("cachestat_unenc_bad", "cachestat"),
            ("map_shadow_stack_unenc_bad", "map_shadow_stack"),
            ("sched_yield_unenc_bad", "sched_yield"),
            ("setfsuid_unenc_bad", "setfsuid"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_remap_move_pages_cachestat_shadow_yield_setfsuid_unenc_ok_still_proves(self):
        for name in (
            "remap_file_pages_unenc_ok",
            "move_pages_unenc_ok",
            "cachestat_unenc_ok",
            "map_shadow_stack_unenc_ok",
            "sched_yield_unenc_ok",
            "setfsuid_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_errcat_nested_fence_nexit_wconv_invoke_unenc_needs_harness(self):
        for name, needle in (
            ("errcat_unenc_bad", "error_category"),
            ("nested_unenc_bad", "nested_exception"),
            ("fence_unenc_bad", "atomic_thread_fence"),
            ("nexit_unenc_bad", "notify_all_at_thread_exit"),
            ("wconv_unenc_bad", "wstring_convert"),
            ("invoke_unenc_bad", "std::invoke"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_errcat_nested_fence_nexit_wconv_invoke_unenc_ok_still_proves(self):
        for name in (
            "errcat_unenc_ok",
            "nested_unenc_ok",
            "fence_unenc_ok",
            "nexit_unenc_ok",
            "wconv_unenc_ok",
            "invoke_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_klogctl_setsched_mbind_timer_settime_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("klogctl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("klogctl", r.message.lower())
        self.assertNotIn("syslog", r.message.lower())
        r, _, _ = bmc("setsched_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sched_setscheduler", r.message.lower())
        self.assertNotIn("sched_yield", r.message.lower())
        r, _, _ = bmc("mbind_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mbind", r.message.lower())
        self.assertNotIn("move_pages", r.message.lower())
        r, _, _ = bmc("timer_settime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timer_settime", r.message.lower())
        self.assertNotIn("timerfd_settime", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_wait4_preadv_sendmsg_getsockname_epoll_pwait_inotify_rm_unenc_needs_harness(self):
        for name, needle in (
            ("wait4_unenc_bad", "wait4"),
            ("preadv_unenc_bad", "preadv"),
            ("sendmsg_unenc_bad", "sendmsg"),
            ("getsockname_unenc_bad", "getsockname"),
            ("epoll_pwait_unenc_bad", "epoll_pwait"),
            ("inotify_rm_watch_unenc_bad", "inotify_rm_watch"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_wait4_preadv_sendmsg_getsockname_epoll_pwait_inotify_rm_unenc_ok_still_proves(self):
        for name in (
            "wait4_unenc_ok",
            "preadv_unenc_ok",
            "sendmsg_unenc_ok",
            "getsockname_unenc_ok",
            "epoll_pwait_unenc_ok",
            "inotify_rm_watch_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_eventfd_rw_sched_setattr_renameat2_execveat_mlock2_faccessat2_unenc_needs_harness(self):
        for name, needle in (
            ("eventfd_rw_unenc_bad", "eventfd_read"),
            ("sched_setattr_unenc_bad", "sched_setattr"),
            ("renameat2_unenc_bad", "renameat2"),
            ("execveat_unenc_bad", "execveat"),
            ("mlock2_unenc_bad", "mlock2"),
            ("faccessat2_unenc_bad", "faccessat2"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_eventfd_rw_sched_setattr_renameat2_execveat_mlock2_faccessat2_unenc_ok_still_proves(self):
        for name in (
            "eventfd_rw_unenc_ok",
            "sched_setattr_unenc_ok",
            "renameat2_unenc_ok",
            "execveat_unenc_ok",
            "mlock2_unenc_ok",
            "faccessat2_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_apply_refwrap_endian_bitceil_uncaught_vjoin_unenc_needs_harness(self):
        for name, needle in (
            ("apply_unenc_bad", "std::apply"),
            ("refwrap_unenc_bad", "reference_wrapper"),
            ("endian_unenc_bad", "std::endian"),
            ("bitceil_unenc_bad", "bit_ceil"),
            ("uncaught_unenc_bad", "uncaught_exceptions"),
            ("vjoin_unenc_bad", "join"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_apply_refwrap_endian_bitceil_uncaught_vjoin_unenc_ok_still_proves(self):
        for name in (
            "apply_unenc_ok",
            "refwrap_unenc_ok",
            "endian_unenc_ok",
            "bitceil_unenc_ok",
            "uncaught_unenc_ok",
            "vjoin_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_wait4_sendmsg_epoll_pwait_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("waitpid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait", r.message.lower())
        self.assertNotIn("wait4", r.message.lower())
        r, _, _ = bmc("sendmmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmmsg", r.message.lower())
        self.assertNotIn("sendmsg", r.message.lower().replace("sendmmsg", ""))
        r, _, _ = bmc("epoll_create_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("epoll_create", r.message.lower())
        self.assertNotIn("epoll_pwait", r.message.lower())
        r, _, _ = bmc("invoke_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("invoke", r.message.lower())
        self.assertNotIn("apply", r.message.lower())
        r, _, _ = bmc("vzip_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("zip", r.message.lower())
        self.assertNotIn("join", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_ustat_vhangup_mseal_futex2_listmount_lsm_unenc_needs_harness(self):
        for name, needle in (
            ("ustat_unenc_bad", "ustat"),
            ("vhangup_unenc_bad", "vhangup"),
            ("mseal_unenc_bad", "mseal"),
            ("futex2_unenc_bad", "futex_wait"),
            ("listmount_unenc_bad", "statmount"),
            ("lsm_attr_unenc_bad", "lsm_get_self_attr"),
            ("mempolicy_home_unenc_bad", "set_mempolicy_home_node"),
            ("file_getattr_unenc_bad", "file_getattr"),
            ("setxattrat_unenc_bad", "setxattrat"),
            ("fchmodat2_unenc_bad", "fchmodat2"),
            ("sigqueueinfo_unenc_bad", "rt_sigqueueinfo"),
            ("open_tree_attr_unenc_bad", "open_tree_attr"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_ustat_vhangup_mseal_futex2_listmount_lsm_unenc_ok_still_proves(self):
        for name in (
            "ustat_unenc_ok",
            "vhangup_unenc_ok",
            "mseal_unenc_ok",
            "futex2_unenc_ok",
            "listmount_unenc_ok",
            "lsm_attr_unenc_ok",
            "mempolicy_home_unenc_ok",
            "file_getattr_unenc_ok",
            "setxattrat_unenc_ok",
            "fchmodat2_unenc_ok",
            "sigqueueinfo_unenc_ok",
            "open_tree_attr_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_qexit_toarr_zoned_killdep_rotl_curexc_unenc_needs_harness(self):
        for name, needle in (
            ("qexit_unenc_bad", "quick_exit"),
            ("toarr_unenc_bad", "to_array"),
            ("zoned_unenc_bad", "zoned_time"),
            ("killdep_unenc_bad", "kill_dependency"),
            ("rotl_unenc_bad", "rotl"),
            ("curexc_unenc_bad", "current_exception"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_qexit_toarr_zoned_killdep_rotl_curexc_unenc_ok_still_proves(self):
        for name in (
            "qexit_unenc_ok",
            "toarr_unenc_ok",
            "zoned_unenc_ok",
            "killdep_unenc_ok",
            "rotl_unenc_ok",
            "curexc_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_futex2_setxattrat_mempolicy_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("futex_waitv_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("futex_waitv", r.message.lower())
        self.assertNotIn("futex_wait", r.message.lower().replace("futex_waitv", ""))
        r, _, _ = bmc("setxattr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setxattr", r.message.lower())
        self.assertNotIn("setxattrat", r.message.lower())
        r, _, _ = bmc("mbind_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mbind", r.message.lower())
        self.assertNotIn("set_mempolicy_home_node", r.message.lower())
        r, _, _ = bmc("tzdb_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("tzdb", r.message.lower())
        self.assertNotIn("zoned_time", r.message.lower())
        r, _, _ = bmc("array_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("array", r.message.lower())
        self.assertNotIn("to_array", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_fadvise_sigaction_sem_open_renameat_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("fadvise_unenc_bad", "posix_fadvise"),
            ("readahead_unenc_bad", "readahead"),
            ("sigaction_unenc_bad", "sigaction"),
            ("sigprocmask_unenc_bad", "sigprocmask"),
            ("sem_open_unenc_bad", "sem_open"),
            ("rwlock_unenc_bad", "pthread_rwlock"),
            ("pthread_cond_unenc_bad", "pthread_cond"),
            ("sigaltstack_unenc_bad", "sigaltstack"),
            ("renameat_unenc_bad", "renameat"),
            ("faccessat_unenc_bad", "faccessat"),
            ("fchmodat_unenc_bad", "fchmodat"),
            ("pthread_barrier_unenc_bad", "pthread_barrier"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_fadvise_sigaction_sem_open_renameat_cxx_unenc_ok_still_proves(self):
        for name in (
            "fadvise_unenc_ok",
            "readahead_unenc_ok",
            "sigaction_unenc_ok",
            "sigprocmask_unenc_ok",
            "sem_open_unenc_ok",
            "rwlock_unenc_ok",
            "pthread_cond_unenc_ok",
            "sigaltstack_unenc_ok",
            "renameat_unenc_ok",
            "faccessat_unenc_ok",
            "fchmodat_unenc_ok",
            "pthread_barrier_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_cxx_bit_width_lerp_midpoint_cmp_less_countl_unreach_needs_harness(self):
        for name, needle in (
            ("bitw_unenc_bad", "bit_width"),
            ("lerp_unenc_bad", "lerp"),
            ("midpt_unenc_bad", "midpoint"),
            ("cmpl_unenc_bad", "cmp_less"),
            ("countl_unenc_bad", "countl_zero"),
            ("unreach_unenc_bad", "unreachable"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_cxx_bit_width_lerp_midpoint_cmp_less_countl_unreach_ok_still_proves(self):
        for name in (
            "bitw_unenc_ok",
            "lerp_unenc_ok",
            "midpt_unenc_ok",
            "cmpl_unenc_ok",
            "countl_unenc_ok",
            "unreach_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_renameat2_sem_wait_bitceil_rotl_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("renameat2_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("renameat2", r.message.lower())
        r, _, _ = bmc("faccessat2_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("faccessat2", r.message.lower())
        r, _, _ = bmc("fchmodat2_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fchmodat2", r.message.lower())
        r, _, _ = bmc("renameat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("renameat", r.message.lower())
        self.assertNotIn("renameat2", r.message.lower())
        r, _, _ = bmc("faccessat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("faccessat", r.message.lower())
        self.assertNotIn("faccessat2", r.message.lower())
        r, _, _ = bmc("fchmodat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fchmodat", r.message.lower())
        self.assertNotIn("fchmodat2", r.message.lower())
        r, _, _ = bmc("sem_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_wait", r.message.lower())
        self.assertNotIn("sem_open", r.message.lower())
        r, _, _ = bmc("bitceil_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("bit_ceil", r.message.lower())
        self.assertNotIn("bit_width", r.message.lower())
        r, _, _ = bmc("rotl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rotl", r.message.lower())
        r, _, _ = bmc("process_madvise_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("process_madvise", r.message.lower())
        self.assertNotIn("posix_fadvise", r.message.lower())
        r, _, _ = bmc("signalfd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("signalfd", r.message.lower())
        self.assertNotIn("sigaction", r.message.lower())
        r, _, _ = bmc("condvar_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("condition_variable", r.message.lower())
        self.assertNotIn("pthread_cond", r.message.lower())
        r, _, _ = bmc("barrier_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("barrier", r.message.lower())
        self.assertNotIn("pthread_barrier", r.message.lower())
        r, _, _ = bmc("unreach_only")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn("std::unreachable", r.message.lower())
        r, _, _ = bmc("trap_only")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn("std::unreachable", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_symlinkat_pthread_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("symlinkat_unenc_bad", "symlinkat"),
            ("unlinkat_unenc_bad", "unlinkat"),
            ("mkdirat_unenc_bad", "mkdirat"),
            ("mknodat_unenc_bad", "mknodat"),
            ("readlinkat_unenc_bad", "readlinkat"),
            ("fstatat_unenc_bad", "fstatat"),
            ("spin_unenc_bad", "pthread_spin"),
            ("pthread_key_unenc_bad", "pthread_key"),
            ("pthread_cancel_unenc_bad", "pthread_cancel"),
            ("pthread_kill_unenc_bad", "pthread_kill"),
            ("pthread_sigmask_unenc_bad", "pthread_sigmask"),
            ("pthread_atfork_unenc_bad", "pthread_atfork"),
            ("gcd_unenc_bad", "gcd"),
            ("lcm_unenc_bad", "lcm"),
            ("clamp_unenc_bad", "clamp"),
            ("exch_unenc_bad", "exchange"),
            ("toaddr_unenc_bad", "to_address"),
            ("ice_unenc_bad", "is_constant_evaluated"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_symlinkat_pthread_cxx_unenc_ok_still_proves(self):
        for name in (
            "symlinkat_unenc_ok",
            "unlinkat_unenc_ok",
            "mkdirat_unenc_ok",
            "mknodat_unenc_ok",
            "readlinkat_unenc_ok",
            "fstatat_unenc_ok",
            "spin_unenc_ok",
            "pthread_key_unenc_ok",
            "pthread_cancel_unenc_ok",
            "pthread_kill_unenc_ok",
            "pthread_sigmask_unenc_ok",
            "pthread_atfork_unenc_ok",
            "gcd_unenc_ok",
            "lcm_unenc_ok",
            "clamp_unenc_ok",
            "exch_unenc_ok",
            "toaddr_unenc_ok",
            "ice_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_symlinkat_fstatat_spin_kill_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("stat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stat", r.message.lower())
        self.assertNotIn("fstatat", r.message.lower())
        r, _, _ = bmc("symlink_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("symlink", r.message.lower())
        self.assertNotIn("symlinkat", r.message.lower())
        r, _, _ = bmc("unlink_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unlink", r.message.lower())
        self.assertNotIn("unlinkat", r.message.lower())
        r, _, _ = bmc("mkdir_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mkdir", r.message.lower())
        self.assertNotIn("mkdirat", r.message.lower())
        r, _, _ = bmc("rwlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_rwlock", r.message.lower())
        self.assertNotIn("pthread_spin", r.message.lower())
        r, _, _ = bmc("sigprocmask_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sigprocmask", r.message.lower())
        self.assertNotIn("pthread_sigmask", r.message.lower())
        r, _, _ = bmc("midpt_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("midpoint", r.message.lower())
        self.assertNotIn("clamp", r.message.lower())
        r, _, _ = bmc("lerp_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("lerp", r.message.lower())
        self.assertNotIn("clamp", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_pledge_ucontext_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("pledge_unenc_bad", "pledge"),
            ("unveil_unenc_bad", "unveil"),
            ("sysctl_unenc_bad", "sysctl"),
            ("kqueue_unenc_bad", "kqueue"),
            ("kevent_unenc_bad", "kevent"),
            ("pause_unenc_bad", "pause"),
            ("ppoll_unenc_bad", "ppoll"),
            ("sigwait_unenc_bad", "sigwait"),
            ("sigqueue_unenc_bad", "sigqueue"),
            ("ucontext_unenc_bad", "getcontext"),
            ("sem_timedwait_unenc_bad", "sem_timedwait"),
            ("pthread_attr_unenc_bad", "pthread_attr"),
            ("addrof_unenc_bad", "addressof"),
            ("asmalign_unenc_bad", "assume_aligned"),
            ("asconst_unenc_bad", "as_const"),
            ("exscan_unenc_bad", "exclusive_scan"),
            ("mkeptr_unenc_bad", "make_exception_ptr"),
            ("setterm_unenc_bad", "set_terminate"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_pledge_ucontext_cxx_unenc_ok_still_proves(self):
        for name in (
            "pledge_unenc_ok",
            "unveil_unenc_ok",
            "sysctl_unenc_ok",
            "kqueue_unenc_ok",
            "kevent_unenc_ok",
            "pause_unenc_ok",
            "ppoll_unenc_ok",
            "sigwait_unenc_ok",
            "sigqueue_unenc_ok",
            "ucontext_unenc_ok",
            "sem_timedwait_unenc_ok",
            "pthread_attr_unenc_ok",
            "addrof_unenc_ok",
            "asmalign_unenc_ok",
            "asconst_unenc_ok",
            "exscan_unenc_ok",
            "mkeptr_unenc_ok",
            "setterm_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_pledge_ppoll_sigqueue_sem_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("sigqueueinfo_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rt_sigqueueinfo", r.message.lower())
        self.assertNotIn("sigqueue unencoded", r.message.lower())
        r, _, _ = bmc("sem_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_wait", r.message.lower())
        self.assertNotIn("sem_timedwait", r.message.lower())
        r, _, _ = bmc("toaddr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("to_address", r.message.lower())
        self.assertNotIn("addressof", r.message.lower())
        r, _, _ = bmc("select_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("select", r.message.lower())
        self.assertNotIn("ppoll", r.message.lower())
        r, _, _ = bmc("join_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_join", r.message.lower())
        self.assertNotIn("pthread_attr", r.message.lower())
        r, _, _ = bmc("ice_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("is_constant_evaluated", r.message.lower())
        self.assertNotIn("assume_aligned", r.message.lower())
        r, _, _ = bmc("unreach_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unreachable", r.message.lower())
        self.assertNotIn("set_terminate", r.message.lower())
        r, _, _ = bmc("curexc_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("current_exception", r.message.lower())
        self.assertNotIn("make_exception_ptr", r.message.lower())
        r, _, _ = bmc("exc_ptr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("exception_ptr", r.message.lower())
        self.assertNotIn("make_exception_ptr", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("cap_enter_unenc_bad", "cap_enter"),
            ("cap_rights_unenc_bad", "cap_rights"),
            ("pdfork_unenc_bad", "pdfork"),
            ("procctl_unenc_bad", "procctl"),
            ("closefrom_unenc_bad", "closefrom"),
            ("issetugid_unenc_bad", "issetugid"),
            ("arc4random_unenc_bad", "arc4random"),
            ("chflags_unenc_bad", "chflags"),
            ("getfsstat_unenc_bad", "getfsstat"),
            ("pthread_yield_unenc_bad", "pthread_yield"),
            ("sem_trywait_unenc_bad", "sem_trywait"),
            ("adjtime_unenc_bad", "adjtime"),
            ("inscan_unenc_bad", "inclusive_scan"),
            ("tred_unenc_bad", "transform_reduce"),
            ("reduce_unenc_bad", "reduce"),
            ("uicopy_unenc_bad", "uninitialized_copy"),
            ("construct_unenc_bad", "construct_at"),
            ("fwdlike_unenc_bad", "forward_like"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "cap_enter_unenc_ok",
            "cap_rights_unenc_ok",
            "pdfork_unenc_ok",
            "procctl_unenc_ok",
            "closefrom_unenc_ok",
            "issetugid_unenc_ok",
            "arc4random_unenc_ok",
            "chflags_unenc_ok",
            "getfsstat_unenc_ok",
            "pthread_yield_unenc_ok",
            "sem_trywait_unenc_ok",
            "adjtime_unenc_ok",
            "inscan_unenc_ok",
            "tred_unenc_ok",
            "reduce_unenc_ok",
            "uicopy_unenc_ok",
            "construct_unenc_ok",
            "fwdlike_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("pledge_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pledge", r.message.lower())
        self.assertNotIn("cap_enter", r.message.lower())
        r, _, _ = bmc("getrandom_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getrandom", r.message.lower())
        self.assertNotIn("arc4random", r.message.lower())
        r, _, _ = bmc("adjtimex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("adjtimex", r.message.lower())
        self.assertNotIn("adjtime unencoded", r.message.lower())
        r, _, _ = bmc("sem_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_wait", r.message.lower())
        self.assertNotIn("sem_trywait", r.message.lower())
        r, _, _ = bmc("sem_timedwait_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_timedwait", r.message.lower())
        self.assertNotIn("sem_trywait", r.message.lower())
        r, _, _ = bmc("exscan_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("exclusive_scan", r.message.lower())
        self.assertNotIn("inclusive_scan", r.message.lower())
        r, _, _ = bmc("toaddr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("to_address", r.message.lower())
        self.assertNotIn("construct_at", r.message.lower())
        r, _, _ = bmc("prctl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("prctl", r.message.lower())
        self.assertNotIn("procctl", r.message.lower())
        r, _, _ = bmc("close_range_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("close_range", r.message.lower())
        self.assertNotIn("closefrom", r.message.lower())
        r, _, _ = bmc("sched_yield_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sched_yield", r.message.lower())
        self.assertNotIn("pthread_yield", r.message.lower())
        r, _, _ = bmc("statfs_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("statfs", r.message.lower())
        self.assertNotIn("getfsstat", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover2_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("revoke_unenc_bad", "revoke"),
            ("ktrace_unenc_bad", "ktrace"),
            ("rfork_unenc_bad", "rfork"),
            ("jail_unenc_bad", "jail"),
            ("setlogin_unenc_bad", "setlogin"),
            ("getresuid_unenc_bad", "getresuid"),
            ("getpeereid_unenc_bad", "getpeereid"),
            ("strtonum_unenc_bad", "strtonum"),
            ("reallocarray_unenc_bad", "reallocarray"),
            ("timingsafe_unenc_bad", "timingsafe"),
            ("getprogname_unenc_bad", "getprogname"),
            ("daemon_unenc_bad", "daemon"),
            ("uifill_unenc_bad", "uninitialized_fill"),
            ("destroyn_unenc_bad", "destroy_n"),
            ("addsat_unenc_bad", "add_sat"),
            ("tscan_unenc_bad", "transform_inclusive_scan"),
            ("typeid_unenc_bad", "type_identity"),
            ("nontype_unenc_bad", "nontype"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover2_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "revoke_unenc_ok",
            "ktrace_unenc_ok",
            "rfork_unenc_ok",
            "jail_unenc_ok",
            "setlogin_unenc_ok",
            "getresuid_unenc_ok",
            "getpeereid_unenc_ok",
            "strtonum_unenc_ok",
            "reallocarray_unenc_ok",
            "timingsafe_unenc_ok",
            "getprogname_unenc_ok",
            "daemon_unenc_ok",
            "uifill_unenc_ok",
            "destroyn_unenc_ok",
            "addsat_unenc_ok",
            "tscan_unenc_ok",
            "typeid_unenc_ok",
            "nontype_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover2_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("pdfork_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pdfork", r.message.lower())
        self.assertNotIn("rfork", r.message.lower())
        r, _, _ = bmc("getlogin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getlogin", r.message.lower())
        self.assertNotIn("setlogin", r.message.lower())
        r, _, _ = bmc("setreuid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setreuid", r.message.lower())
        self.assertNotIn("getresuid", r.message.lower())
        r, _, _ = bmc("inscan_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("inclusive_scan", r.message.lower())
        self.assertNotIn("transform_inclusive_scan", r.message.lower())
        r, _, _ = bmc("construct_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("construct_at", r.message.lower())
        self.assertNotIn("destroy_n", r.message.lower())
        r, _, _ = bmc("uicopy_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uninitialized_copy", r.message.lower())
        self.assertNotIn("uninitialized_fill", r.message.lower())
        r, _, _ = bmc("prctl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("prctl", r.message.lower())
        self.assertNotIn("ktrace", r.message.lower())
        r, _, _ = bmc("typeid_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("typeid", r.message.lower())
        self.assertNotIn("type_identity", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover3_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("cap_fcntls_unenc_bad", "cap_fcntls"),
            ("pdgetpid_unenc_bad", "pdgetpid"),
            ("kldload_unenc_bad", "kldload"),
            ("extattr_unenc_bad", "extattr"),
            ("mac_unenc_bad", "mac_set"),
            ("audit_unenc_bad", "auditon"),
            ("kvm_unenc_bad", "kvm_open"),
            ("reallocf_unenc_bad", "reallocf"),
            ("uuidgen_unenc_bad", "uuidgen"),
            ("setfib_unenc_bad", "setfib"),
            ("ntp_gettime_unenc_bad", "ntp_gettime"),
            ("crypt_newhash_unenc_bad", "crypt_newhash"),
            ("layoutc_unenc_bad", "is_layout_compatible"),
            ("pinter_unenc_bad", "is_pointer_interconvertible"),
            ("uvalue_unenc_bad", "uninitialized_value_construct"),
            ("bciter_unenc_bad", "basic_const_iterator"),
            ("corrm_unenc_bad", "is_corresponding_member"),
            ("rto_unenc_bad", "ranges::to"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover3_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "cap_fcntls_unenc_ok",
            "pdgetpid_unenc_ok",
            "kldload_unenc_ok",
            "extattr_unenc_ok",
            "mac_unenc_ok",
            "audit_unenc_ok",
            "kvm_unenc_ok",
            "reallocf_unenc_ok",
            "uuidgen_unenc_ok",
            "setfib_unenc_ok",
            "ntp_gettime_unenc_ok",
            "crypt_newhash_unenc_ok",
            "layoutc_unenc_ok",
            "pinter_unenc_ok",
            "uvalue_unenc_ok",
            "bciter_unenc_ok",
            "corrm_unenc_ok",
            "rto_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover3_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("pdfork_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pdfork", r.message.lower())
        self.assertNotIn("pdgetpid", r.message.lower())
        r, _, _ = bmc("cap_enter_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_enter", r.message.lower())
        self.assertNotIn("cap_fcntls", r.message.lower())
        r, _, _ = bmc("reallocarray_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("reallocarray", r.message.lower())
        self.assertNotIn("reallocf", r.message.lower())
        r, _, _ = bmc("adjtime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("adjtime", r.message.lower())
        self.assertNotIn("ntp_gettime", r.message.lower())
        r, _, _ = bmc("uifill_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uninitialized_fill", r.message.lower())
        self.assertNotIn("uninitialized_value_construct", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover4_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("wait6_unenc_bad", "wait6"),
            ("cpuset_unenc_bad", "cpuset"),
            ("rtprio_unenc_bad", "rtprio"),
            ("kenv_unenc_bad", "kenv"),
            ("getfh_unenc_bad", "getfh"),
            ("getmntinfo_unenc_bad", "getmntinfo"),
            ("nmount_unenc_bad", "nmount"),
            ("strmode_unenc_bad", "strmode"),
            ("getosreldate_unenc_bad", "getosreldate"),
            ("cap_sandboxed_unenc_bad", "cap_sandboxed"),
            ("getgrouplist_unenc_bad", "getgrouplist"),
            ("eaccess_unenc_bad", "eaccess"),
            ("enumv_unenc_bad", "enumerate"),
            ("cart_unenc_bad", "cartesian_product"),
            ("chunk_unenc_bad", "chunk"),
            ("slide_unenc_bad", "slide"),
            ("adjv_unenc_bad", "adjacent"),
            ("jwith_unenc_bad", "join_with"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover4_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "wait6_unenc_ok",
            "cpuset_unenc_ok",
            "rtprio_unenc_ok",
            "kenv_unenc_ok",
            "getfh_unenc_ok",
            "getmntinfo_unenc_ok",
            "nmount_unenc_ok",
            "strmode_unenc_ok",
            "getosreldate_unenc_ok",
            "cap_sandboxed_unenc_ok",
            "getgrouplist_unenc_ok",
            "eaccess_unenc_ok",
            "enumv_unenc_ok",
            "cart_unenc_ok",
            "chunk_unenc_ok",
            "slide_unenc_ok",
            "adjv_unenc_ok",
            "jwith_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover4_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("wait4_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait4", r.message.lower())
        self.assertNotIn("wait6", r.message.lower())
        r, _, _ = bmc("sched_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sched_setaffinity", r.message.lower())
        self.assertNotIn("cpuset", r.message.lower())
        r, _, _ = bmc("cap_enter_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_enter", r.message.lower())
        self.assertNotIn("cap_sandboxed", r.message.lower())
        r, _, _ = bmc("getgroups_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getgroups", r.message.lower())
        self.assertNotIn("getgrouplist", r.message.lower())
        r, _, _ = bmc("vjoin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("join", r.message.lower())
        self.assertNotIn("join_with", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover5_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("login_class_unenc_bad", "login_getclass"),
            ("fflags_unenc_bad", "fflagstostr"),
            ("getdirentries_unenc_bad", "getdirentries"),
            ("kinfo_unenc_bad", "kinfo_getproc"),
            ("umtx_unenc_bad", "_umtx_op"),
            ("thr_unenc_bad", "thr_kill"),
            ("modfind_unenc_bad", "modfind"),
            ("lpathconf_unenc_bad", "lpathconf"),
            ("loginclass_unenc_bad", "loginclass"),
            ("getfsent_unenc_bad", "getfsent"),
            ("minherit_unenc_bad", "minherit"),
            ("cap_getmode_unenc_bad", "cap_getmode"),
            ("ztrans_unenc_bad", "zip_transform"),
            ("asrval_unenc_bad", "as_rvalue"),
            ("frange_unenc_bad", "from_range"),
            ("scenum_unenc_bad", "is_scoped_enum"),
            ("stride_unenc_bad", "stride"),
            ("repeat_unenc_bad", "repeat"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover5_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "login_class_unenc_ok",
            "fflags_unenc_ok",
            "getdirentries_unenc_ok",
            "kinfo_unenc_ok",
            "umtx_unenc_ok",
            "thr_unenc_ok",
            "modfind_unenc_ok",
            "lpathconf_unenc_ok",
            "loginclass_unenc_ok",
            "getfsent_unenc_ok",
            "minherit_unenc_ok",
            "cap_getmode_unenc_ok",
            "ztrans_unenc_ok",
            "asrval_unenc_ok",
            "frange_unenc_ok",
            "scenum_unenc_ok",
            "stride_unenc_ok",
            "repeat_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover5_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("getlogin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getlogin", r.message.lower())
        self.assertNotIn("login_getclass", r.message.lower())
        self.assertNotIn("loginclass", r.message.lower())
        r, _, _ = bmc("getdents_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getdents", r.message.lower())
        self.assertNotIn("getdirentries", r.message.lower())
        r, _, _ = bmc("kldload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        self.assertNotIn("modfind", r.message.lower())
        r, _, _ = bmc("cap_sandboxed_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_sandboxed", r.message.lower())
        self.assertNotIn("cap_getmode", r.message.lower())
        r, _, _ = bmc("vzip_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("zip", r.message.lower())
        self.assertNotIn("zip_transform", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover6_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("nfssvc_unenc_bad", "nfssvc"),
            ("sysarch_unenc_bad", "sysarch"),
            ("getpagesizes_unenc_bad", "getpagesizes"),
            ("sbrk_unenc_bad", "sbrk"),
            ("ksem_unenc_bad", "ksem"),
            ("cap_getrights_unenc_bad", "cap_getrights"),
            ("devname_unenc_bad", "devname"),
            ("getbootfile_unenc_bad", "getbootfile"),
            ("kldfirstmod_unenc_bad", "kldfirstmod"),
            ("fhlink_unenc_bad", "fhlink"),
            ("valloc_unenc_bad", "valloc"),
            ("getdomainname_unenc_bad", "getdomainname"),
            ("takev_unenc_bad", "take"),
            ("dropv_unenc_bad", "drop"),
            ("filterv_unenc_bad", "filter"),
            ("tview_unenc_bad", "views::transform"),
            ("elems_unenc_bad", "elements"),
            ("iota_unenc_bad", "iota"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover6_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "nfssvc_unenc_ok",
            "sysarch_unenc_ok",
            "getpagesizes_unenc_ok",
            "sbrk_unenc_ok",
            "ksem_unenc_ok",
            "cap_getrights_unenc_ok",
            "devname_unenc_ok",
            "getbootfile_unenc_ok",
            "kldfirstmod_unenc_ok",
            "fhlink_unenc_ok",
            "valloc_unenc_ok",
            "getdomainname_unenc_ok",
            "takev_unenc_ok",
            "dropv_unenc_ok",
            "filterv_unenc_ok",
            "tview_unenc_ok",
            "elems_unenc_ok",
            "iota_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover6_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("sem_open_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_open", r.message.lower())
        self.assertNotIn("ksem", r.message.lower())
        r, _, _ = bmc("kldload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        self.assertNotIn("kldfirstmod", r.message.lower())
        r, _, _ = bmc("cap_rights_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_rights", r.message.lower())
        self.assertNotIn("cap_getrights", r.message.lower())
        r, _, _ = bmc("setdomainname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setdomainname", r.message.lower())
        self.assertNotIn("getdomainname", r.message.lower())
        r, _, _ = bmc("tred_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("transform_reduce", r.message.lower())
        self.assertNotIn("transform_view", r.message.lower())
        self.assertNotIn("views::transform", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover7_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("fts_unenc_bad", "fts_open"),
            ("getvfsbyname_unenc_bad", "getvfsbyname"),
            ("unmount_unenc_bad", "unmount"),
            ("getpagesize_unenc_bad", "getpagesize"),
            ("lio_unenc_bad", "lio_listio"),
            ("cpuclock_unenc_bad", "clock_getcpuclockid"),
            ("pthcpuclock_unenc_bad", "pthread_getcpuclockid"),
            ("schedprio_unenc_bad", "sched_get_priority"),
            ("spawnattr_unenc_bad", "posix_spawn_file_actions"),
            ("kld_isloaded_unenc_bad", "kld_load"),
            ("dlfunc_unenc_bad", "dlfunc"),
            ("typedmem_unenc_bad", "posix_typed_mem"),
            ("twhile_unenc_bad", "take_while"),
            ("dwhile_unenc_bad", "drop_while"),
            ("keys_unenc_bad", "keys"),
            ("vals_unenc_bad", "values"),
            ("rview_unenc_bad", "reverse"),
            ("countv_unenc_bad", "counted"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover7_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "fts_unenc_ok",
            "getvfsbyname_unenc_ok",
            "unmount_unenc_ok",
            "getpagesize_unenc_ok",
            "lio_unenc_ok",
            "cpuclock_unenc_ok",
            "pthcpuclock_unenc_ok",
            "schedprio_unenc_ok",
            "spawnattr_unenc_ok",
            "kld_isloaded_unenc_ok",
            "dlfunc_unenc_ok",
            "typedmem_unenc_ok",
            "twhile_unenc_ok",
            "dwhile_unenc_ok",
            "keys_unenc_ok",
            "vals_unenc_ok",
            "rview_unenc_ok",
            "countv_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover7_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("getpagesizes_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getpagesizes", r.message.lower())
        self.assertNotRegex(r.message.lower(), r"\bgetpagesize(?!s)")
        r, _, _ = bmc("takev_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("take", r.message.lower())
        self.assertNotIn("take_while", r.message.lower())
        r, _, _ = bmc("dropv_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("drop", r.message.lower())
        self.assertNotIn("drop_while", r.message.lower())
        r, _, _ = bmc("kldload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        self.assertNotIn("kld_load", r.message.lower())
        r, _, _ = bmc("spawn_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("posix_spawn", r.message.lower())
        self.assertNotIn("posix_spawn_file_actions", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover8_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("brk_unenc_bad", "brk"),
            ("ioctl_unenc_bad", "ioctl"),
            ("socket_unenc_bad", "socket"),
            ("access_unenc_bad", "access"),
            ("scandir_unenc_bad", "scandir"),
            ("accept_unenc_bad", "accept"),
            ("asprintf_unenc_bad", "asprintf"),
            ("bind_unenc_bad", "bind"),
            ("chown_unenc_bad", "chown"),
            ("mkfifo_unenc_bad", "mkfifo"),
            ("setuid_unenc_bad", "setuid"),
            ("getpwuid_unenc_bad", "getpwuid"),
            ("swait_unenc_bad", "sync_wait"),
            ("cassert_unenc_bad", "contracts"),
            ("daggr_unenc_bad", "reflection"),
            ("twnested_unenc_bad", "throw_with_nested"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover8_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "brk_unenc_ok",
            "ioctl_unenc_ok",
            "socket_unenc_ok",
            "access_unenc_ok",
            "scandir_unenc_ok",
            "accept_unenc_ok",
            "asprintf_unenc_ok",
            "bind_unenc_ok",
            "chown_unenc_ok",
            "mkfifo_unenc_ok",
            "setuid_unenc_ok",
            "getpwuid_unenc_ok",
            "swait_unenc_ok",
            "cassert_unenc_ok",
            "daggr_unenc_ok",
            "twnested_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover8_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("sbrk_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sbrk", r.message.lower())
        self.assertNotRegex(r.message.lower(), r"\bbrk unencoded")
        r, _, _ = bmc("eaccess_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("eaccess", r.message.lower())
        self.assertNotRegex(r.message.lower(), r"\baccess unencoded")
        r, _, _ = bmc("socketpair_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("socketpair", r.message.lower())
        self.assertNotRegex(r.message.lower(), r"\bsocket unencoded")
        r, _, _ = bmc("mmap_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mmap", r.message.lower())
        self.assertNotIn("ioctl", r.message.lower())
        r, _, _ = bmc("nested_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("nested_exception", r.message.lower())
        self.assertNotIn("throw_with_nested", r.message.lower())
        r, _, _ = bmc("execution_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("execution", r.message.lower())
        self.assertNotIn("sync_wait", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover9_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("spawnattr_init_unenc_bad", "posix_spawn_file_actions"),
            ("dlvsym_unenc_bad", "dlfunc"),
            ("kldnextmod_unenc_bad", "kldfirstmod"),
            ("fts_read_unenc_bad", "fts_open"),
            ("fts_children_unenc_bad", "fts_open"),
            ("fts_close_unenc_bad", "fts_open"),
            ("fts_set_unenc_bad", "fts_open"),
            ("typedmem_info_unenc_bad", "posix_typed_mem"),
            ("schedprio_min_unenc_bad", "sched_get_priority"),
            ("shutdown_unenc_bad", "send"),
            ("recv_unenc_bad", "send"),
            ("sendto_unenc_bad", "send"),
            ("recvfrom_unenc_bad", "send"),
            ("accept4_unenc_bad", "accept"),
            ("seteuid_unenc_bad", "setuid"),
            ("setgid_unenc_bad", "setuid"),
            ("vasprintf_unenc_bad", "asprintf"),
            ("fchown_unenc_bad", "chown"),
            ("lchown_unenc_bad", "chown"),
            ("mknod_unenc_bad", "mkfifo"),
            ("getpwnam_unenc_bad", "getpwuid"),
            ("crypt_unenc_bad", "getpwuid"),
            ("dclass_unenc_bad", "reflection"),
            ("caret_unenc_bad", "reflection"),
            ("stdmeta_unenc_bad", "reflection"),
            ("cpre_unenc_bad", "contracts"),
            ("cpost_unenc_bad", "contracts"),
            ("rinested_unenc_bad", "throw_with_nested"),
            ("sigfence_unenc_bad", "atomic_thread_fence"),
            ("atqexit_unenc_bad", "quick_exit"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover9_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "spawnattr_init_unenc_ok",
            "dlvsym_unenc_ok",
            "kldnextmod_unenc_ok",
            "fts_read_unenc_ok",
            "fts_children_unenc_ok",
            "fts_close_unenc_ok",
            "fts_set_unenc_ok",
            "typedmem_info_unenc_ok",
            "schedprio_min_unenc_ok",
            "shutdown_unenc_ok",
            "recv_unenc_ok",
            "sendto_unenc_ok",
            "recvfrom_unenc_ok",
            "accept4_unenc_ok",
            "seteuid_unenc_ok",
            "setgid_unenc_ok",
            "vasprintf_unenc_ok",
            "fchown_unenc_ok",
            "lchown_unenc_ok",
            "mknod_unenc_ok",
            "getpwnam_unenc_ok",
            "crypt_unenc_ok",
            "dclass_unenc_ok",
            "caret_unenc_ok",
            "stdmeta_unenc_ok",
            "cpre_unenc_ok",
            "cpost_unenc_ok",
            "rinested_unenc_ok",
            "sigfence_unenc_ok",
            "atqexit_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover9_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("sendmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmsg", r.message.lower())
        self.assertNotIn("send unencoded", r.message.lower())
        r, _, _ = bmc("mknodat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mknodat", r.message.lower())
        self.assertNotIn("mkfifo", r.message.lower())
        r, _, _ = bmc("crypt_newhash_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("crypt_newhash", r.message.lower())
        self.assertNotIn("getpwuid", r.message.lower())
        r, _, _ = bmc("dlopen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("dlopen", r.message.lower())
        self.assertNotIn("dlfunc", r.message.lower())
        r, _, _ = bmc("spawn_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("posix_spawn", r.message.lower())
        self.assertNotIn("posix_spawn_file_actions", r.message.lower())
        r, _, _ = bmc("nested_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("nested_exception", r.message.lower())
        self.assertNotIn("throw_with_nested", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover10_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("kld_load_unenc_bad", "kld_load"),
            ("ksem_close_unenc_bad", "ksem"),
            ("ksem_wait_unenc_bad", "ksem"),
            ("ksem_post_unenc_bad", "ksem"),
            ("ksem_unlink_unenc_bad", "ksem"),
            ("devname_r_unenc_bad", "devname"),
            ("fhlinkat_unenc_bad", "fhlink"),
            ("fhreadlink_unenc_bad", "fhlink"),
            ("print_unenc_bad", "format"),
            ("println_unenc_bad", "format"),
            ("future_unenc_bad", "async"),
            ("ulock_unenc_bad", "mutex"),
            ("destroy_at_unenc_bad", "construct_at"),
            ("subsat_unenc_bad", "add_sat"),
            ("getterm_unenc_bad", "set_terminate"),
            ("fmtn_unenc_bad", "format_to"),
            ("bitfloor_unenc_bad", "bit_ceil"),
            ("stopsrc_unenc_bad", "stop_token"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover10_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "kld_load_unenc_ok",
            "ksem_close_unenc_ok",
            "ksem_wait_unenc_ok",
            "ksem_post_unenc_ok",
            "ksem_unlink_unenc_ok",
            "devname_r_unenc_ok",
            "fhlinkat_unenc_ok",
            "fhreadlink_unenc_ok",
            "print_unenc_ok",
            "println_unenc_ok",
            "future_unenc_ok",
            "ulock_unenc_ok",
            "destroy_at_unenc_ok",
            "subsat_unenc_ok",
            "getterm_unenc_ok",
            "fmtn_unenc_ok",
            "bitfloor_unenc_ok",
            "stopsrc_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover10_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("kldload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        self.assertNotIn("kld_load", r.message.lower())
        r, _, _ = bmc("kld_isloaded_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kld_load", r.message.lower())
        self.assertNotIn("kldload unencoded", r.message.lower())
        r, _, _ = bmc("sem_open_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_open", r.message.lower())
        self.assertNotIn("ksem", r.message.lower())
        r, _, _ = bmc("format_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("format", r.message.lower())
        self.assertNotIn("format_to", r.message.lower())
        r, _, _ = bmc("fmtto_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("format_to", r.message.lower())
        r, _, _ = bmc("async_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("async", r.message.lower())
        self.assertNotIn("mutex", r.message.lower())
        r, _, _ = bmc("mutex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mutex", r.message.lower())
        self.assertNotIn("condition_variable", r.message.lower())
        r, _, _ = bmc("construct_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("construct_at", r.message.lower())
        self.assertNotIn("destroy_n", r.message.lower())
        r, _, _ = bmc("addsat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("add_sat", r.message.lower())
        self.assertNotIn("sub_sat", r.message.lower())
        r, _, _ = bmc("stop_token_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stop_token", r.message.lower())
        self.assertNotIn("latch", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_trailing_junk_stays_error_not_harness(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="junk",
            kind="SCALAR",
            line=1,
            signature="int junk(int x)",
            params=[("int", "x")],
            body="return x + ;\n",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS)
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED},
        )

    def test_leftover11_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("mulsat_unenc_bad", "add_sat"),
            ("divsat_unenc_bad", "add_sat"),
            ("satcast_unenc_bad", "add_sat"),
            ("lguard_unenc_bad", "mutex"),
            ("scoped_unenc_bad", "mutex"),
            ("hasbit_unenc_bad", "bit_ceil"),
            ("popcnt_unenc_bad", "bit_ceil"),
            ("stopcb_unenc_bad", "stop_token"),
            ("promise_unenc_bad", "async"),
            ("smutex_unenc_bad", "condition_variable"),
            ("czone_unenc_bad", "tzdb"),
            ("chunkby_unenc_bad", "chunk"),
            ("adjt_unenc_bad", "adjacent"),
            ("texscan_unenc_bad", "transform_inclusive_scan"),
            ("rotr_unenc_bad", "rotl"),
            ("cmpg_unenc_bad", "cmp_less"),
            ("cntrz_unenc_bad", "countl_zero"),
            ("uimove_unenc_bad", "uninitialized_copy"),
            ("uifilln_unenc_bad", "uninitialized_fill"),
            ("pinterb_unenc_bad", "is_pointer_interconvertible"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover11_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "mulsat_unenc_ok",
            "divsat_unenc_ok",
            "satcast_unenc_ok",
            "lguard_unenc_ok",
            "scoped_unenc_ok",
            "hasbit_unenc_ok",
            "popcnt_unenc_ok",
            "stopcb_unenc_ok",
            "promise_unenc_ok",
            "smutex_unenc_ok",
            "czone_unenc_ok",
            "chunkby_unenc_ok",
            "adjt_unenc_ok",
            "texscan_unenc_ok",
            "rotr_unenc_ok",
            "cmpg_unenc_ok",
            "cntrz_unenc_ok",
            "uimove_unenc_ok",
            "uifilln_unenc_ok",
            "pinterb_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover11_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("ulock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mutex", r.message.lower())
        self.assertNotIn("shared_lock", r.message.lower())
        r, _, _ = bmc("slock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("shared_lock", r.message.lower())
        self.assertNotIn("scoped_lock", r.message.lower())
        r, _, _ = bmc("mutex_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mutex", r.message.lower())
        self.assertNotIn("condition_variable", r.message.lower())
        r, _, _ = bmc("addsat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("add_sat", r.message.lower())
        self.assertNotIn("mul_sat", r.message.lower())
        r, _, _ = bmc("bitceil_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("bit_ceil", r.message.lower())
        self.assertNotIn("has_single_bit", r.message.lower())
        r, _, _ = bmc("async_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("async", r.message.lower())
        self.assertNotIn("promise", r.message.lower())
        r, _, _ = bmc("condvar_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("condition_variable", r.message.lower())
        self.assertNotIn("shared_mutex", r.message.lower())
        r, _, _ = bmc("chunk_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chunk", r.message.lower())
        self.assertNotIn("chunk_by", r.message.lower())
        r, _, _ = bmc("rotl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rotl", r.message.lower())
        self.assertNotIn("rotr", r.message.lower())
        r, _, _ = bmc("stop_token_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stop_token", r.message.lower())
        self.assertNotIn("latch", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover12_libc_unenc_needs_harness(self):
        for name, needle in (
            ("madvise_unenc_bad", "madvise"),
            ("posix_madvise_unenc_bad", "madvise"),
            ("setusercontext_unenc_bad", "login_getclass"),
            ("strtofflags_unenc_bad", "fflagstostr"),
            ("kinfo_getfile_unenc_bad", "kinfo_getproc"),
            ("kinfo_getvmmap_unenc_bad", "kinfo_getproc"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover12_libc_unenc_ok_still_proves(self):
        for name in (
            "madvise_unenc_ok",
            "posix_madvise_unenc_ok",
            "setusercontext_unenc_ok",
            "strtofflags_unenc_ok",
            "kinfo_getfile_unenc_ok",
            "kinfo_getvmmap_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover12_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("madvise_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("madvise", r.message.lower())
        self.assertNotIn("process_madvise", r.message.lower())
        r, _, _ = bmc("process_madvise_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("process_madvise", r.message.lower())
        r, _, _ = bmc("login_class_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("login_getclass", r.message.lower())
        self.assertNotIn("setusercontext", r.message.lower())
        r, _, _ = bmc("setusercontext_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("login_getclass", r.message.lower())
        r, _, _ = bmc("fflags_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fflagstostr", r.message.lower())
        self.assertNotIn("strtofflags", r.message.lower())
        r, _, _ = bmc("strtofflags_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fflagstostr", r.message.lower())
        r, _, _ = bmc("kinfo_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kinfo_getproc", r.message.lower())
        self.assertNotIn("kinfo_getfile", r.message.lower())
        r, _, _ = bmc("kinfo_getfile_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kinfo_getproc", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover12_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("chunkv_unenc_bad", "chunk"),
            ("adjvw_unenc_bad", "adjacent"),
            ("uidc_unenc_bad", "uninitialized_fill"),
            ("uidcn_unenc_bad", "uninitialized_fill"),
            ("uicn_unenc_bad", "uninitialized_copy"),
            ("uimn_unenc_bad", "uninitialized_copy"),
            ("cmple_unenc_bad", "cmp_less"),
            ("cmpge_unenc_bad", "cmp_less"),
            ("cmpeq_unenc_bad", "cmp_less"),
            ("cmpne_unenc_bad", "cmp_less"),
            ("inrng_unenc_bad", "cmp_less"),
            ("cntlo_unenc_bad", "countl_zero"),
            ("cntro_unenc_bad", "countl_zero"),
            ("ifs_unenc_bad", "fstream"),
            ("ofs_unenc_bad", "fstream"),
            ("u8view_unenc_bad", "u8string"),
            ("mofn_unenc_bad", "move_only_function"),
            ("uvaln_unenc_bad", "uninitialized_value_construct"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover12_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "chunkv_unenc_ok",
            "adjvw_unenc_ok",
            "uidc_unenc_ok",
            "uidcn_unenc_ok",
            "uicn_unenc_ok",
            "uimn_unenc_ok",
            "cmple_unenc_ok",
            "cmpge_unenc_ok",
            "cmpeq_unenc_ok",
            "cmpne_unenc_ok",
            "inrng_unenc_ok",
            "cntlo_unenc_ok",
            "cntro_unenc_ok",
            "ifs_unenc_ok",
            "ofs_unenc_ok",
            "u8view_unenc_ok",
            "mofn_unenc_ok",
            "uvaln_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover12_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("chunk_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chunk", r.message.lower())
        self.assertNotIn("chunk_view", r.message.lower())
        r, _, _ = bmc("chunkby_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chunk", r.message.lower())
        self.assertNotIn("chunk_view", r.message.lower())
        r, _, _ = bmc("adjv_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("adjacent", r.message.lower())
        self.assertNotIn("adjacent_view", r.message.lower())
        r, _, _ = bmc("uifill_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uninitialized_fill", r.message.lower())
        self.assertNotIn("default_construct", r.message.lower())
        r, _, _ = bmc("uicopy_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uninitialized_copy", r.message.lower())
        self.assertNotIn("uninitialized_copy_n", r.message.lower())
        r, _, _ = bmc("cmpl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cmp_less", r.message.lower())
        self.assertNotIn("cmp_less_equal", r.message.lower())
        r, _, _ = bmc("countl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("countl_zero", r.message.lower())
        self.assertNotIn("countl_one", r.message.lower())
        r, _, _ = bmc("fstream_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fstream", r.message.lower())
        self.assertNotIn("ifstream", r.message.lower())
        r, _, _ = bmc("u8_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("u8string", r.message.lower())
        self.assertNotIn("u8string_view", r.message.lower())
        r, _, _ = bmc("uvalue_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uninitialized_value_construct", r.message.lower())
        self.assertNotIn("uninitialized_value_construct_n", r.message.lower())
        r, _, _ = bmc("copyable_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("move_only_function", r.message.lower())
        self.assertNotIn("copyable_function", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover13_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("setloginclass_unenc_bad", "loginclass"),
            ("setfsent_unenc_bad", "getfsent"),
            ("endfsent_unenc_bad", "getfsent"),
            ("modstat_unenc_bad", "modfind"),
            ("modnext_unenc_bad", "modfind"),
            ("modfnext_unenc_bad", "modfind"),
            ("slidev_unenc_bad", "slide"),
            ("jwithv_unenc_bad", "join_with"),
            ("joinv_unenc_bad", "join"),
            ("ztransv_unenc_bad", "zip_transform"),
            ("zipv_unenc_bad", "zip"),
            ("asrvalv_unenc_bad", "as_rvalue"),
            ("enumvw_unenc_bad", "enumerate"),
            ("cartv_unenc_bad", "cartesian_product"),
            ("stridev_unenc_bad", "stride"),
            ("repeatv_unenc_bad", "repeat"),
            ("twhilev_unenc_bad", "take_while"),
            ("takevw_unenc_bad", "take"),
            ("dwhilev_unenc_bad", "drop_while"),
            ("dropvw_unenc_bad", "drop"),
            ("keysv_unenc_bad", "keys"),
            ("valsv_unenc_bad", "values"),
            ("revv_unenc_bad", "reverse"),
            ("countvw_unenc_bad", "counted"),
            ("filtervw_unenc_bad", "filter"),
            ("tvw_unenc_bad", "views::transform"),
            ("elemsv_unenc_bad", "elements"),
            ("iotav_unenc_bad", "iota"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover13_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "setloginclass_unenc_ok",
            "setfsent_unenc_ok",
            "endfsent_unenc_ok",
            "modstat_unenc_ok",
            "modnext_unenc_ok",
            "modfnext_unenc_ok",
            "slidev_unenc_ok",
            "jwithv_unenc_ok",
            "joinv_unenc_ok",
            "ztransv_unenc_ok",
            "zipv_unenc_ok",
            "asrvalv_unenc_ok",
            "enumvw_unenc_ok",
            "cartv_unenc_ok",
            "stridev_unenc_ok",
            "repeatv_unenc_ok",
            "twhilev_unenc_ok",
            "takevw_unenc_ok",
            "dwhilev_unenc_ok",
            "dropvw_unenc_ok",
            "keysv_unenc_ok",
            "valsv_unenc_ok",
            "revv_unenc_ok",
            "countvw_unenc_ok",
            "filtervw_unenc_ok",
            "tvw_unenc_ok",
            "elemsv_unenc_ok",
            "iotav_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover13_libc_cxx_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("loginclass_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("loginclass", r.message.lower())
        self.assertNotIn("setloginclass", r.message.lower())
        r, _, _ = bmc("setloginclass_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("loginclass", r.message.lower())
        r, _, _ = bmc("getfsent_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getfsent", r.message.lower())
        self.assertNotIn("setfsent", r.message.lower())
        r, _, _ = bmc("setfsent_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getfsent", r.message.lower())
        r, _, _ = bmc("modfind_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("modfind", r.message.lower())
        self.assertNotIn("modstat", r.message.lower())
        r, _, _ = bmc("modstat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("modfind", r.message.lower())
        r, _, _ = bmc("slide_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("slide", r.message.lower())
        self.assertNotIn("slide_view", r.message.lower())
        r, _, _ = bmc("slidev_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("slide", r.message.lower())
        r, _, _ = bmc("jwith_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("join_with", r.message.lower())
        self.assertNotIn("join_with_view", r.message.lower())
        r, _, _ = bmc("vjoin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("join", r.message.lower())
        self.assertNotIn("join_view", r.message.lower())
        r, _, _ = bmc("takev_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("take", r.message.lower())
        self.assertNotIn("take_view", r.message.lower())
        r, _, _ = bmc("tvw_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("views::transform", r.message.lower())
        r, _, _ = bmc("filterv_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("filter", r.message.lower())
        self.assertNotIn("filter_view", r.message.lower())
        r, _, _ = bmc("filtervw_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("filter", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover14_libc_unenc_needs_harness(self):
        for name, needle in (
            ("thr_new_unenc_bad", "thr_kill"),
            ("thr_kill2_unenc_bad", "thr_kill"),
            ("thr_self_unenc_bad", "thr_kill"),
            ("thr_exit_unenc_bad", "thr_kill"),
            ("thr_suspend_unenc_bad", "thr_kill"),
            ("thr_wake_unenc_bad", "thr_kill"),
            ("kldunload_unenc_bad", "kldload"),
            ("kldfind_unenc_bad", "kldload"),
            ("kldsym_unenc_bad", "kldload"),
            ("kldstat_unenc_bad", "kldload"),
            ("extattr_get_file_unenc_bad", "extattr"),
            ("extattr_delete_file_unenc_bad", "extattr"),
            ("extattr_list_file_unenc_bad", "extattr"),
            ("extattr_set_fd_unenc_bad", "extattr"),
            ("extattr_get_fd_unenc_bad", "extattr"),
            ("extattr_delete_fd_unenc_bad", "extattr"),
            ("extattr_list_fd_unenc_bad", "extattr"),
            ("extattr_set_link_unenc_bad", "extattr"),
            ("extattr_get_link_unenc_bad", "extattr"),
            ("extattr_delete_link_unenc_bad", "extattr"),
            ("extattr_list_link_unenc_bad", "extattr"),
            ("mac_get_proc_unenc_bad", "mac_set"),
            ("mac_set_fd_unenc_bad", "mac_set"),
            ("mac_get_fd_unenc_bad", "mac_set"),
            ("mac_set_file_unenc_bad", "mac_set"),
            ("mac_get_file_unenc_bad", "mac_set"),
            ("getaudit_unenc_bad", "auditon"),
            ("setaudit_unenc_bad", "auditon"),
            ("auditctl_unenc_bad", "auditon"),
            ("kvm_openfiles_unenc_bad", "kvm_open"),
            ("kvm_getprocs_unenc_bad", "kvm_open"),
            ("kvm_close_unenc_bad", "kvm_open"),
            ("kvm_nlist_unenc_bad", "kvm_open"),
            ("cap_ioctls_limit_unenc_bad", "cap_fcntls"),
            ("pdwait4_unenc_bad", "pdgetpid"),
            ("crypt_checkpass_unenc_bad", "crypt_newhash"),
            ("jail_attach_unenc_bad", "jail"),
            ("jail_get_unenc_bad", "jail"),
            ("jail_set_unenc_bad", "jail"),
            ("jail_remove_unenc_bad", "jail"),
            ("getresgid_unenc_bad", "getresuid"),
            ("timingsafe_memcmp_unenc_bad", "timingsafe"),
            ("setprogname_unenc_bad", "getprogname"),
            ("setproctitle_unenc_bad", "daemon"),
            ("arc4random_buf_unenc_bad", "arc4random"),
            ("arc4random_uniform_unenc_bad", "arc4random"),
            ("fchflags_unenc_bad", "chflags"),
            ("lchflags_unenc_bad", "chflags"),
            ("ntp_adjtime_unenc_bad", "adjtime"),
            ("sem_getvalue_unenc_bad", "sem_trywait"),
            ("rtprio_thread_unenc_bad", "rtprio"),
            ("cpuset_getaffinity_unenc_bad", "cpuset"),
            ("fhopen_unenc_bad", "getfh"),
            ("fhstat_unenc_bad", "getfh"),
            ("fhstatfs_unenc_bad", "getfh"),
            ("getfhat_unenc_bad", "getfh"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover14_libc_unenc_ok_still_proves(self):
        for name in (
            "thr_new_unenc_ok",
            "thr_kill2_unenc_ok",
            "thr_self_unenc_ok",
            "thr_exit_unenc_ok",
            "thr_suspend_unenc_ok",
            "thr_wake_unenc_ok",
            "kldunload_unenc_ok",
            "kldfind_unenc_ok",
            "kldsym_unenc_ok",
            "kldstat_unenc_ok",
            "extattr_get_file_unenc_ok",
            "extattr_delete_file_unenc_ok",
            "extattr_list_file_unenc_ok",
            "extattr_set_fd_unenc_ok",
            "extattr_get_fd_unenc_ok",
            "extattr_delete_fd_unenc_ok",
            "extattr_list_fd_unenc_ok",
            "extattr_set_link_unenc_ok",
            "extattr_get_link_unenc_ok",
            "extattr_delete_link_unenc_ok",
            "extattr_list_link_unenc_ok",
            "mac_get_proc_unenc_ok",
            "mac_set_fd_unenc_ok",
            "mac_get_fd_unenc_ok",
            "mac_set_file_unenc_ok",
            "mac_get_file_unenc_ok",
            "getaudit_unenc_ok",
            "setaudit_unenc_ok",
            "auditctl_unenc_ok",
            "kvm_openfiles_unenc_ok",
            "kvm_getprocs_unenc_ok",
            "kvm_close_unenc_ok",
            "kvm_nlist_unenc_ok",
            "cap_ioctls_limit_unenc_ok",
            "pdwait4_unenc_ok",
            "crypt_checkpass_unenc_ok",
            "jail_attach_unenc_ok",
            "jail_get_unenc_ok",
            "jail_set_unenc_ok",
            "jail_remove_unenc_ok",
            "getresgid_unenc_ok",
            "timingsafe_memcmp_unenc_ok",
            "setprogname_unenc_ok",
            "setproctitle_unenc_ok",
            "arc4random_buf_unenc_ok",
            "arc4random_uniform_unenc_ok",
            "fchflags_unenc_ok",
            "lchflags_unenc_ok",
            "ntp_adjtime_unenc_ok",
            "sem_getvalue_unenc_ok",
            "rtprio_thread_unenc_ok",
            "cpuset_getaffinity_unenc_ok",
            "fhopen_unenc_ok",
            "fhstat_unenc_ok",
            "fhstatfs_unenc_ok",
            "getfhat_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover14_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("thr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thr_kill", r.message.lower())
        self.assertNotIn("thr_new", r.message.lower())
        r, _, _ = bmc("thr_new_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thr_kill", r.message.lower())
        r, _, _ = bmc("kldload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        self.assertNotIn("kldunload", r.message.lower())
        r, _, _ = bmc("kldunload_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kldload", r.message.lower())
        r, _, _ = bmc("extattr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("extattr", r.message.lower())
        self.assertNotIn("extattr_get_file", r.message.lower())
        r, _, _ = bmc("extattr_get_file_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("extattr", r.message.lower())
        r, _, _ = bmc("mac_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mac_set", r.message.lower())
        self.assertNotIn("mac_get_proc", r.message.lower())
        r, _, _ = bmc("mac_get_proc_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mac_set", r.message.lower())
        r, _, _ = bmc("audit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("auditon", r.message.lower())
        self.assertNotIn("getaudit", r.message.lower())
        r, _, _ = bmc("getaudit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("auditon", r.message.lower())
        r, _, _ = bmc("kvm_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kvm_open", r.message.lower())
        self.assertNotIn("kvm_openfiles", r.message.lower())
        r, _, _ = bmc("kvm_openfiles_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kvm_open", r.message.lower())
        r, _, _ = bmc("cap_fcntls_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_fcntls", r.message.lower())
        self.assertNotIn("cap_ioctls", r.message.lower())
        r, _, _ = bmc("cap_ioctls_limit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_fcntls", r.message.lower())
        r, _, _ = bmc("pdgetpid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pdgetpid", r.message.lower())
        self.assertNotIn("pdwait4", r.message.lower())
        r, _, _ = bmc("pdwait4_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pdgetpid", r.message.lower())
        r, _, _ = bmc("crypt_newhash_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("crypt_newhash", r.message.lower())
        self.assertNotIn("crypt_checkpass", r.message.lower())
        r, _, _ = bmc("crypt_checkpass_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("crypt_newhash", r.message.lower())
        r, _, _ = bmc("jail_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("jail", r.message.lower())
        self.assertNotIn("jail_attach", r.message.lower())
        r, _, _ = bmc("jail_attach_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("jail", r.message.lower())
        r, _, _ = bmc("getresuid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getresuid", r.message.lower())
        self.assertNotIn("getresgid", r.message.lower())
        r, _, _ = bmc("getresgid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getresuid", r.message.lower())
        r, _, _ = bmc("timingsafe_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timingsafe", r.message.lower())
        self.assertNotIn("timingsafe_memcmp", r.message.lower())
        r, _, _ = bmc("timingsafe_memcmp_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timingsafe", r.message.lower())
        r, _, _ = bmc("getprogname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getprogname", r.message.lower())
        self.assertNotIn("setprogname", r.message.lower())
        r, _, _ = bmc("setprogname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getprogname", r.message.lower())
        r, _, _ = bmc("daemon_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("daemon", r.message.lower())
        self.assertNotIn("setproctitle", r.message.lower())
        r, _, _ = bmc("setproctitle_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("daemon", r.message.lower())
        r, _, _ = bmc("arc4random_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("arc4random", r.message.lower())
        self.assertNotIn("arc4random_buf", r.message.lower())
        r, _, _ = bmc("arc4random_buf_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("arc4random", r.message.lower())
        r, _, _ = bmc("chflags_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chflags", r.message.lower())
        self.assertNotIn("fchflags", r.message.lower())
        r, _, _ = bmc("fchflags_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chflags", r.message.lower())
        r, _, _ = bmc("adjtime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("adjtime", r.message.lower())
        self.assertNotIn("ntp_adjtime", r.message.lower())
        r, _, _ = bmc("ntp_adjtime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("adjtime", r.message.lower())
        r, _, _ = bmc("sem_trywait_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_trywait", r.message.lower())
        self.assertNotIn("sem_getvalue", r.message.lower())
        r, _, _ = bmc("sem_getvalue_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_trywait", r.message.lower())
        r, _, _ = bmc("rtprio_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rtprio", r.message.lower())
        self.assertNotIn("rtprio_thread", r.message.lower())
        r, _, _ = bmc("rtprio_thread_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rtprio", r.message.lower())
        r, _, _ = bmc("cpuset_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cpuset", r.message.lower())
        self.assertNotIn("cpuset_getaffinity", r.message.lower())
        r, _, _ = bmc("cpuset_getaffinity_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cpuset", r.message.lower())
        r, _, _ = bmc("getfh_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getfh", r.message.lower())
        self.assertNotIn("fhopen", r.message.lower())
        r, _, _ = bmc("fhopen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getfh", r.message.lower())
        r, _, _ = bmc("thr_new_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("jail_attach_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover15_libc_unenc_needs_harness(self):
        for name, needle in (
            ("cap_rights_get_unenc_bad", "cap_rights"),
            ("pthread_attr_destroy_unenc_bad", "pthread_attr"),
            ("pthread_attr_setstacksize_unenc_bad", "pthread_attr"),
            ("pthread_attr_setstack_unenc_bad", "pthread_attr"),
            ("pthread_attr_setdetachstate_unenc_bad", "pthread_attr"),
            ("pthread_attr_getstacksize_unenc_bad", "pthread_attr"),
            ("pthread_attr_getstack_unenc_bad", "pthread_attr"),
            ("pthread_attr_getdetachstate_unenc_bad", "pthread_attr"),
            ("setcontext_unenc_bad", "getcontext"),
            ("swapcontext_unenc_bad", "getcontext"),
            ("makecontext_unenc_bad", "getcontext"),
            ("wait3_unenc_bad", "wait4"),
            ("setregid_unenc_bad", "setreuid"),
            ("setresuid_unenc_bad", "setreuid"),
            ("setresgid_unenc_bad", "setreuid"),
            ("raise_unenc_bad", "kill"),
            ("alarm_unenc_bad", "kill"),
            ("pthread_detach_unenc_bad", "pthread_join"),
            ("sem_post_unenc_bad", "sem_wait"),
            ("sem_init_unenc_bad", "sem"),
            ("sem_destroy_unenc_bad", "sem"),
            ("fdopendir_unenc_bad", "opendir"),
            ("readdir_unenc_bad", "opendir"),
            ("closedir_unenc_bad", "opendir"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover15_libc_unenc_ok_still_proves(self):
        for name in (
            "cap_rights_get_unenc_ok",
            "pthread_attr_destroy_unenc_ok",
            "pthread_attr_setstacksize_unenc_ok",
            "pthread_attr_setstack_unenc_ok",
            "pthread_attr_setdetachstate_unenc_ok",
            "pthread_attr_getstacksize_unenc_ok",
            "pthread_attr_getstack_unenc_ok",
            "pthread_attr_getdetachstate_unenc_ok",
            "setcontext_unenc_ok",
            "swapcontext_unenc_ok",
            "makecontext_unenc_ok",
            "wait3_unenc_ok",
            "setregid_unenc_ok",
            "setresuid_unenc_ok",
            "setresgid_unenc_ok",
            "raise_unenc_ok",
            "alarm_unenc_ok",
            "pthread_detach_unenc_ok",
            "sem_post_unenc_ok",
            "sem_init_unenc_ok",
            "sem_destroy_unenc_ok",
            "fdopendir_unenc_ok",
            "readdir_unenc_ok",
            "closedir_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover15_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("cap_rights_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_rights", r.message.lower())
        self.assertNotIn("cap_rights_get", r.message.lower())
        r, _, _ = bmc("cap_rights_get_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("cap_rights", r.message.lower())
        r, _, _ = bmc("pthread_attr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_attr", r.message.lower())
        self.assertNotIn("pthread_attr_destroy", r.message.lower())
        r, _, _ = bmc("pthread_attr_destroy_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_attr", r.message.lower())
        r, _, _ = bmc("ucontext_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getcontext", r.message.lower())
        self.assertNotIn("setcontext", r.message.lower())
        r, _, _ = bmc("setcontext_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getcontext", r.message.lower())
        r, _, _ = bmc("wait4_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait4", r.message.lower())
        self.assertNotIn("wait3", r.message.lower())
        r, _, _ = bmc("wait3_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait4", r.message.lower())
        r, _, _ = bmc("setreuid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setreuid", r.message.lower())
        self.assertNotIn("setregid", r.message.lower())
        r, _, _ = bmc("setregid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setreuid", r.message.lower())
        r, _, _ = bmc("kill_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kill", r.message.lower())
        self.assertNotIn("raise", r.message.lower())
        r, _, _ = bmc("raise_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("kill", r.message.lower())
        r, _, _ = bmc("join_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_join", r.message.lower())
        self.assertNotIn("pthread_detach", r.message.lower())
        r, _, _ = bmc("pthread_detach_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_join", r.message.lower())
        r, _, _ = bmc("sem_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_wait", r.message.lower())
        self.assertNotIn("sem_post", r.message.lower())
        r, _, _ = bmc("sem_post_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_wait", r.message.lower())
        r, _, _ = bmc("opendir_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("opendir", r.message.lower())
        self.assertNotIn("fdopendir", r.message.lower())
        r, _, _ = bmc("fdopendir_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("opendir", r.message.lower())
        r, _, _ = bmc("cap_rights_get_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("pthread_detach_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover16_libc_unenc_needs_harness(self):
        for name, needle in (
            ("pthread_once_unenc_bad", "pthread join"),
            ("pthread_key_delete_unenc_bad", "pthread_key_create"),
            ("pthread_setspecific_unenc_bad", "pthread_key_create"),
            ("pthread_getspecific_unenc_bad", "pthread_key_create"),
            ("pthread_cond_timedwait_unenc_bad", "pthread_cond"),
            ("pthread_cond_signal_unenc_bad", "pthread_cond"),
            ("pthread_cond_broadcast_unenc_bad", "pthread_cond"),
            ("pthread_cond_init_unenc_bad", "pthread_cond"),
            ("pthread_cond_destroy_unenc_bad", "pthread_cond"),
            ("pthread_rwlock_wrlock_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_unlock_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_init_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_destroy_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_tryrdlock_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_trywrlock_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_timedrdlock_unenc_bad", "pthread_rwlock"),
            ("pthread_rwlock_timedwrlock_unenc_bad", "pthread_rwlock"),
            ("pthread_spin_unlock_unenc_bad", "pthread_spin"),
            ("pthread_spin_trylock_unenc_bad", "pthread_spin"),
            ("pthread_spin_init_unenc_bad", "pthread_spin"),
            ("pthread_spin_destroy_unenc_bad", "pthread_spin"),
            ("pthread_barrier_init_unenc_bad", "pthread_barrier"),
            ("pthread_barrier_destroy_unenc_bad", "pthread_barrier"),
            ("dup2_unenc_bad", "dup"),
            ("dup3_unenc_bad", "dup"),
            ("pipe2_unenc_bad", "pipe"),
            ("wait_unenc_bad", "wait"),
            ("waitid_unenc_bad", "wait"),
            ("poll_unenc_bad", "select"),
            ("pselect_unenc_bad", "select"),
            ("epoll_wait_unenc_bad", "select"),
            ("epoll_ctl_unenc_bad", "select"),
            ("recvmsg_unenc_bad", "sendmsg"),
            ("freeaddrinfo_unenc_bad", "addrinfo"),
            ("sysctlbyname_unenc_bad", "sysctl"),
            ("setfsgid_unenc_bad", "setfsuid"),
            ("setsid_unenc_bad", "setpgid"),
            ("getsid_unenc_bad", "setpgid"),
            ("sem_close_unenc_bad", "sem_open"),
            ("sem_unlink_unenc_bad", "sem_open"),
            ("getrlimit_unenc_bad", "setrlimit"),
            ("lstat_unenc_bad", "stat"),
            ("fstat_unenc_bad", "stat"),
            ("usleep_unenc_bad", "sleep"),
            ("nanosleep_unenc_bad", "sleep"),
            ("aio_write_unenc_bad", "aio"),
            ("aio_error_unenc_bad", "aio"),
            ("aio_return_unenc_bad", "aio"),
            ("aio_suspend_unenc_bad", "aio"),
            ("io_uring_enter_unenc_bad", "io_uring"),
            ("io_uring_register_unenc_bad", "io_uring"),
            ("lsetxattr_unenc_bad", "setxattr"),
            ("fsetxattr_unenc_bad", "setxattr"),
            ("getxattr_unenc_bad", "setxattr"),
            ("listxattr_unenc_bad", "setxattr"),
            ("removexattr_unenc_bad", "setxattr"),
            ("landlock_add_rule_unenc_bad", "landlock"),
            ("landlock_restrict_self_unenc_bad", "landlock"),
            ("mq_timedsend_unenc_bad", "mq_unlink"),
            ("mq_timedreceive_unenc_bad", "mq_unlink"),
            ("mq_notify_unenc_bad", "mq_unlink"),
            ("mq_getsetattr_unenc_bad", "mq_unlink"),
            ("globfree_unenc_bad", "glob"),
            ("posix_spawnp_unenc_bad", "posix_spawn"),
            ("shm_unlink_unenc_bad", "shm_open"),
            ("gettimeofday_unenc_bad", "clock_gettime"),
            ("ftell_unenc_bad", "fseek"),
            ("rewind_unenc_bad", "fseek"),
            ("fgetpos_unenc_bad", "fseek"),
            ("fsetpos_unenc_bad", "fseek"),
            ("setsockopt_unenc_bad", "getsockopt"),
            ("getpeername_unenc_bad", "getsockname"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover16_libc_unenc_ok_still_proves(self):
        for name in (
            "pthread_once_unenc_ok",
            "pthread_key_delete_unenc_ok",
            "pthread_setspecific_unenc_ok",
            "pthread_getspecific_unenc_ok",
            "pthread_cond_timedwait_unenc_ok",
            "pthread_cond_signal_unenc_ok",
            "pthread_cond_broadcast_unenc_ok",
            "pthread_cond_init_unenc_ok",
            "pthread_cond_destroy_unenc_ok",
            "pthread_rwlock_wrlock_unenc_ok",
            "pthread_rwlock_unlock_unenc_ok",
            "pthread_rwlock_init_unenc_ok",
            "pthread_rwlock_destroy_unenc_ok",
            "pthread_rwlock_tryrdlock_unenc_ok",
            "pthread_rwlock_trywrlock_unenc_ok",
            "pthread_rwlock_timedrdlock_unenc_ok",
            "pthread_rwlock_timedwrlock_unenc_ok",
            "pthread_spin_unlock_unenc_ok",
            "pthread_spin_trylock_unenc_ok",
            "pthread_spin_init_unenc_ok",
            "pthread_spin_destroy_unenc_ok",
            "pthread_barrier_init_unenc_ok",
            "pthread_barrier_destroy_unenc_ok",
            "dup2_unenc_ok",
            "dup3_unenc_ok",
            "pipe2_unenc_ok",
            "wait_unenc_ok",
            "waitid_unenc_ok",
            "poll_unenc_ok",
            "pselect_unenc_ok",
            "epoll_wait_unenc_ok",
            "epoll_ctl_unenc_ok",
            "recvmsg_unenc_ok",
            "freeaddrinfo_unenc_ok",
            "sysctlbyname_unenc_ok",
            "setfsgid_unenc_ok",
            "setsid_unenc_ok",
            "getsid_unenc_ok",
            "sem_close_unenc_ok",
            "sem_unlink_unenc_ok",
            "getrlimit_unenc_ok",
            "lstat_unenc_ok",
            "fstat_unenc_ok",
            "usleep_unenc_ok",
            "nanosleep_unenc_ok",
            "aio_write_unenc_ok",
            "aio_error_unenc_ok",
            "aio_return_unenc_ok",
            "aio_suspend_unenc_ok",
            "io_uring_enter_unenc_ok",
            "io_uring_register_unenc_ok",
            "lsetxattr_unenc_ok",
            "fsetxattr_unenc_ok",
            "getxattr_unenc_ok",
            "listxattr_unenc_ok",
            "removexattr_unenc_ok",
            "landlock_add_rule_unenc_ok",
            "landlock_restrict_self_unenc_ok",
            "mq_timedsend_unenc_ok",
            "mq_timedreceive_unenc_ok",
            "mq_notify_unenc_ok",
            "mq_getsetattr_unenc_ok",
            "globfree_unenc_ok",
            "posix_spawnp_unenc_ok",
            "shm_unlink_unenc_ok",
            "gettimeofday_unenc_ok",
            "ftell_unenc_ok",
            "rewind_unenc_ok",
            "fgetpos_unenc_ok",
            "fsetpos_unenc_ok",
            "setsockopt_unenc_ok",
            "getpeername_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover16_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("join_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_join", r.message.lower())
        self.assertNotIn("pthread_once", r.message.lower())
        r, _, _ = bmc("pthread_once_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread join", r.message.lower())
        r, _, _ = bmc("pthread_key_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_key", r.message.lower())
        self.assertNotIn("pthread_key_delete", r.message.lower())
        r, _, _ = bmc("pthread_key_delete_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_key_create", r.message.lower())
        r, _, _ = bmc("pthread_cond_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_cond", r.message.lower())
        self.assertNotIn("pthread_cond_signal", r.message.lower())
        r, _, _ = bmc("pthread_cond_signal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_cond", r.message.lower())
        r, _, _ = bmc("rwlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_rwlock", r.message.lower())
        self.assertNotIn("pthread_rwlock_wrlock", r.message.lower())
        r, _, _ = bmc("pthread_rwlock_wrlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_rwlock", r.message.lower())
        r, _, _ = bmc("spin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_spin", r.message.lower())
        self.assertNotIn("pthread_spin_unlock", r.message.lower())
        r, _, _ = bmc("pthread_spin_unlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_spin", r.message.lower())
        r, _, _ = bmc("pthread_barrier_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_barrier", r.message.lower())
        self.assertNotIn("pthread_barrier_init", r.message.lower())
        r, _, _ = bmc("pthread_barrier_init_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pthread_barrier", r.message.lower())
        r, _, _ = bmc("dup_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("dup", r.message.lower())
        self.assertNotIn("dup2", r.message.lower())
        r, _, _ = bmc("dup2_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("dup", r.message.lower())
        r, _, _ = bmc("pipe_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pipe", r.message.lower())
        self.assertNotIn("pipe2", r.message.lower())
        r, _, _ = bmc("pipe2_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pipe", r.message.lower())
        r, _, _ = bmc("waitpid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait", r.message.lower())
        self.assertNotIn("waitid", r.message.lower())
        r, _, _ = bmc("waitid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wait", r.message.lower())
        r, _, _ = bmc("select_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("select", r.message.lower())
        self.assertNotIn("poll", r.message.lower())
        r, _, _ = bmc("poll_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("select", r.message.lower())
        r, _, _ = bmc("sendmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmsg", r.message.lower())
        self.assertNotIn("recvmsg", r.message.lower())
        r, _, _ = bmc("recvmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmsg", r.message.lower())
        r, _, _ = bmc("addrinfo_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("addrinfo", r.message.lower())
        self.assertNotIn("freeaddrinfo", r.message.lower())
        r, _, _ = bmc("freeaddrinfo_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("addrinfo", r.message.lower())
        r, _, _ = bmc("sysctl_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sysctl", r.message.lower())
        self.assertNotIn("sysctlbyname", r.message.lower())
        r, _, _ = bmc("sysctlbyname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sysctl", r.message.lower())
        r, _, _ = bmc("setfsuid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setfsuid", r.message.lower())
        self.assertNotIn("setfsgid", r.message.lower())
        r, _, _ = bmc("setfsgid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setfsuid", r.message.lower())
        r, _, _ = bmc("setpgid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setpgid", r.message.lower())
        self.assertNotIn("setsid", r.message.lower())
        r, _, _ = bmc("setsid_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setpgid", r.message.lower())
        r, _, _ = bmc("sem_open_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_open", r.message.lower())
        self.assertNotIn("sem_close", r.message.lower())
        r, _, _ = bmc("sem_close_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sem_open", r.message.lower())
        r, _, _ = bmc("setrlimit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setrlimit", r.message.lower())
        self.assertNotIn("getrlimit", r.message.lower())
        r, _, _ = bmc("getrlimit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setrlimit", r.message.lower())
        r, _, _ = bmc("stat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stat", r.message.lower())
        self.assertNotIn("lstat", r.message.lower())
        r, _, _ = bmc("lstat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stat", r.message.lower())
        r, _, _ = bmc("sleep_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sleep", r.message.lower())
        self.assertNotIn("usleep", r.message.lower())
        r, _, _ = bmc("usleep_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sleep", r.message.lower())
        r, _, _ = bmc("aio_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("aio", r.message.lower())
        self.assertNotIn("aio_write", r.message.lower())
        r, _, _ = bmc("aio_write_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("aio", r.message.lower())
        r, _, _ = bmc("iouring_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_uring", r.message.lower())
        self.assertNotIn("io_uring_enter", r.message.lower())
        r, _, _ = bmc("io_uring_enter_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_uring", r.message.lower())
        r, _, _ = bmc("setxattr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setxattr", r.message.lower())
        self.assertNotIn("getxattr", r.message.lower())
        r, _, _ = bmc("getxattr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("setxattr", r.message.lower())
        r, _, _ = bmc("landlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("landlock", r.message.lower())
        self.assertNotIn("landlock_add_rule", r.message.lower())
        r, _, _ = bmc("landlock_add_rule_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("landlock", r.message.lower())
        r, _, _ = bmc("mq_unlink_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mq_unlink", r.message.lower())
        self.assertNotIn("mq_timedsend", r.message.lower())
        r, _, _ = bmc("mq_timedsend_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mq_unlink", r.message.lower())
        r, _, _ = bmc("pthread_once_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("getrlimit_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover17_libc_unenc_needs_harness(self):
        for name, needle in (
            ("posix_memalign_unenc_bad", "aligned_alloc"),
            ("munlock_unenc_bad", "mlock"),
            ("mlockall_unenc_bad", "mlock"),
            ("munlockall_unenc_bad", "mlock"),
            ("sched_getaffinity_unenc_bad", "sched_setaffinity"),
            ("capget_unenc_bad", "capset"),
            ("pidfd_send_signal_unenc_bad", "pidfd_open"),
            ("pidfd_getfd_unenc_bad", "pidfd_open"),
            ("setpriority_unenc_bad", "getpriority"),
            ("setgroups_unenc_bad", "initgroups"),
            ("setns_unenc_bad", "unshare"),
            ("recvmmsg_unenc_bad", "sendmmsg"),
            ("io_destroy_unenc_bad", "io_setup"),
            ("io_cancel_unenc_bad", "io_setup"),
            ("io_pgetevents_unenc_bad", "io_setup"),
            ("io_getevents_unenc_bad", "io_submit"),
            ("shmdt_unenc_bad", "shmat"),
            ("semtimedop_unenc_bad", "semop"),
            ("msgrcv_unenc_bad", "msgsnd"),
            ("timer_gettime_unenc_bad", "timer_delete"),
            ("timer_getoverrun_unenc_bad", "timer_delete"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover17_libc_unenc_ok_still_proves(self):
        for name in (
            "posix_memalign_unenc_ok",
            "munlock_unenc_ok",
            "mlockall_unenc_ok",
            "munlockall_unenc_ok",
            "sched_getaffinity_unenc_ok",
            "capget_unenc_ok",
            "pidfd_send_signal_unenc_ok",
            "pidfd_getfd_unenc_ok",
            "setpriority_unenc_ok",
            "setgroups_unenc_ok",
            "setns_unenc_ok",
            "recvmmsg_unenc_ok",
            "io_destroy_unenc_ok",
            "io_cancel_unenc_ok",
            "io_pgetevents_unenc_ok",
            "io_getevents_unenc_ok",
            "shmdt_unenc_ok",
            "semtimedop_unenc_ok",
            "msgrcv_unenc_ok",
            "timer_gettime_unenc_ok",
            "timer_getoverrun_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover17_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("aligned_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("aligned_alloc", r.message.lower())
        self.assertNotIn("posix_memalign", r.message.lower())
        r, _, _ = bmc("posix_memalign_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("aligned_alloc", r.message.lower())
        r, _, _ = bmc("mlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mlock", r.message.lower())
        self.assertNotIn("munlock", r.message.lower())
        r, _, _ = bmc("munlock_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mlock", r.message.lower())
        r, _, _ = bmc("mlockall_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mlock", r.message.lower())
        r, _, _ = bmc("munlockall_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mlock", r.message.lower())
        r, _, _ = bmc("sched_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sched_setaffinity", r.message.lower())
        self.assertNotIn("sched_getaffinity", r.message.lower())
        r, _, _ = bmc("sched_getaffinity_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sched_setaffinity", r.message.lower())
        r, _, _ = bmc("capset_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("capset", r.message.lower())
        self.assertNotIn("capget", r.message.lower())
        r, _, _ = bmc("capget_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("capset", r.message.lower())
        r, _, _ = bmc("pidfd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pidfd_open", r.message.lower())
        self.assertNotIn("pidfd_send_signal", r.message.lower())
        r, _, _ = bmc("pidfd_send_signal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pidfd_open", r.message.lower())
        r, _, _ = bmc("pidfd_getfd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pidfd_open", r.message.lower())
        r, _, _ = bmc("getpriority_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getpriority", r.message.lower())
        self.assertNotIn("setpriority", r.message.lower())
        r, _, _ = bmc("setpriority_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getpriority", r.message.lower())
        r, _, _ = bmc("initgroups_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("initgroups", r.message.lower())
        self.assertNotIn("setgroups", r.message.lower())
        r, _, _ = bmc("setgroups_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("initgroups", r.message.lower())
        r, _, _ = bmc("clone_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unshare", r.message.lower())
        self.assertNotIn("setns", r.message.lower())
        r, _, _ = bmc("setns_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unshare", r.message.lower())
        r, _, _ = bmc("sendmmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmmsg", r.message.lower())
        self.assertNotIn("recvmmsg", r.message.lower())
        r, _, _ = bmc("recvmmsg_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendmmsg", r.message.lower())
        r, _, _ = bmc("io_setup_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_setup", r.message.lower())
        self.assertNotIn("io_destroy", r.message.lower())
        r, _, _ = bmc("io_destroy_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_setup", r.message.lower())
        r, _, _ = bmc("io_cancel_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_setup", r.message.lower())
        r, _, _ = bmc("io_pgetevents_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_setup", r.message.lower())
        r, _, _ = bmc("io_submit_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_submit", r.message.lower())
        self.assertNotIn("io_getevents", r.message.lower())
        r, _, _ = bmc("io_getevents_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("io_submit", r.message.lower())
        r, _, _ = bmc("shmat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("shmat", r.message.lower())
        self.assertNotIn("shmdt", r.message.lower())
        r, _, _ = bmc("shmdt_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("shmat", r.message.lower())
        r, _, _ = bmc("semop_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("semop", r.message.lower())
        self.assertNotIn("semtimedop", r.message.lower())
        r, _, _ = bmc("semtimedop_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("semop", r.message.lower())
        r, _, _ = bmc("msgsnd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("msgsnd", r.message.lower())
        self.assertNotIn("msgrcv", r.message.lower())
        r, _, _ = bmc("msgrcv_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("msgsnd", r.message.lower())
        r, _, _ = bmc("timer_delete_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timer_delete", r.message.lower())
        self.assertNotIn("timer_gettime", r.message.lower())
        r, _, _ = bmc("timer_gettime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timer_delete", r.message.lower())
        r, _, _ = bmc("timer_getoverrun_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("timer_delete", r.message.lower())
        r, _, _ = bmc("posix_memalign_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("setns_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover18_libc_unenc_needs_harness(self):
        for name, needle in (
            ("mmap_unenc_bad", "mmap"),
            ("munmap_unenc_bad", "mmap"),
            ("mprotect_unenc_bad", "mmap"),
            ("chmod_unenc_bad", "chmod"),
            ("fchmod_unenc_bad", "chmod"),
            ("mkdir_unenc_bad", "mkdir"),
            ("rmdir_unenc_bad", "mkdir"),
            ("rename_unenc_bad", "mkdir"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover18_libc_unenc_ok_still_proves(self):
        for name in (
            "mmap_unenc_ok",
            "munmap_unenc_ok",
            "mprotect_unenc_ok",
            "chmod_unenc_ok",
            "fchmod_unenc_ok",
            "mkdir_unenc_ok",
            "rmdir_unenc_ok",
            "rename_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover18_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("mmap_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mmap", r.message.lower())
        self.assertNotIn("munmap", r.message.lower())
        self.assertNotIn("mprotect", r.message.lower())
        r, _, _ = bmc("munmap_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mmap", r.message.lower())
        r, _, _ = bmc("mprotect_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mmap", r.message.lower())
        r, _, _ = bmc("chmod_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chmod", r.message.lower())
        self.assertNotIn("fchmod", r.message.lower())
        r, _, _ = bmc("fchmod_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("chmod", r.message.lower())
        r, _, _ = bmc("mkdir_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mkdir", r.message.lower())
        self.assertNotIn("rmdir", r.message.lower())
        self.assertNotIn("rename", r.message.lower())
        r, _, _ = bmc("rmdir_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mkdir", r.message.lower())
        r, _, _ = bmc("rename_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mkdir", r.message.lower())
        r, _, _ = bmc("mmap_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("chmod_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("mkdir_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("munmap_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover19_libc_unenc_needs_harness(self):
        for name, needle in (
            ("clone_call_unenc_bad", "unshare"),
            ("listmount_call_unenc_bad", "listmount"),
            ("fallocate_call_unenc_bad", "fallocate"),
            ("epoll_create1_unenc_bad", "epoll_create"),
            ("epoll_pwait2_unenc_bad", "epoll_pwait"),
            ("rt_tgsigqueueinfo_unenc_bad", "rt_sigqueueinfo"),
            ("file_setattr_unenc_bad", "file_getattr"),
            ("clock_adjtime_unenc_bad", "clock_settime"),
            ("clock_nanosleep_unenc_bad", "clock_settime"),
            ("quick_exit_unenc_bad", "quick_exit"),
            ("getopt_long_only_unenc_bad", "getopt"),
            ("getopt_long_unenc_bad", "getopt"),
            ("gethostname_unenc_bad", "uname"),
            ("copy_file_range_unenc_bad", "sendfile"),
            ("preadv2_unenc_bad", "preadv"),
            ("pwritev2_unenc_bad", "preadv"),
            ("pwritev_unenc_bad", "preadv"),
            ("timerfd_gettime_unenc_bad", "timerfd_settime"),
            ("eventfd_write_unenc_bad", "eventfd_read"),
            ("eventfd_unenc_bad", "memfd"),
            ("timerfd_create_unenc_bad", "memfd"),
            ("ptrace_unenc_bad", "prctl"),
            ("tcsetattr_unenc_bad", "tcgetattr"),
            ("cfmakeraw_unenc_bad", "tcgetattr"),
            ("fpathconf_unenc_bad", "sysconf"),
            ("pathconf_unenc_bad", "sysconf"),
            ("ftw_unenc_bad", "nftw"),
            ("wordfree_unenc_bad", "wordexp"),
            ("getlogin_r_unenc_bad", "getlogin"),
            ("ttyname_r_unenc_bad", "getlogin"),
            ("ttyname_unenc_bad", "getlogin"),
            ("inet_ntop_unenc_bad", "inet_pton"),
            ("inet_aton_unenc_bad", "inet_pton"),
            ("posix_fadvise64_unenc_bad", "posix_fadvise"),
            ("vmsplice_unenc_bad", "splice"),
            ("inotify_init1_unenc_bad", "inotify"),
            ("inotify_add_watch_unenc_bad", "inotify"),
            ("fdatasync_unenc_bad", "fsync"),
            ("getentropy_unenc_bad", "getrandom"),
            ("getdelim_unenc_bad", "getline"),
            ("strlcat_unenc_bad", "strlcpy"),
            ("memset_s_unenc_bad", "explicit_bzero"),
            ("explicit_memset_unenc_bad", "explicit_bzero"),
            ("posix_openpt_unenc_bad", "ptsname"),
            ("ptsname_r_unenc_bad", "ptsname"),
            ("grantpt_unenc_bad", "ptsname"),
            ("unlockpt_unenc_bad", "ptsname"),
            ("umount2_unenc_bad", "mount"),
            ("umount_unenc_bad", "mount"),
            ("open_wmemstream_unenc_bad", "fmemopen"),
            ("open_memstream_unenc_bad", "fmemopen"),
            ("getxattrat_unenc_bad", "setxattrat"),
            ("listxattrat_unenc_bad", "setxattrat"),
            ("removexattrat_unenc_bad", "setxattrat"),
            ("sched_getattr_unenc_bad", "sched_setattr"),
            ("sched_getscheduler_unenc_bad", "sched_setscheduler"),
            ("sched_setparam_unenc_bad", "sched_setscheduler"),
            ("sched_getparam_unenc_bad", "sched_setscheduler"),
            ("fanotify_mark_unenc_bad", "fanotify"),
            ("getgrgid_unenc_bad", "getgrnam"),
            ("getspnam_unenc_bad", "getgrnam"),
            ("lsm_set_self_attr_unenc_bad", "lsm_get_self_attr"),
            ("lsm_list_modules_unenc_bad", "lsm_get_self_attr"),
            ("sigsuspend_unenc_bad", "sigprocmask"),
            ("sigwaitinfo_unenc_bad", "sigwait"),
            ("sigtimedwait_unenc_bad", "sigwait"),
            ("sigpending_unenc_bad", "sigwait"),
            ("open_by_handle_at_unenc_bad", "name_to_handle"),
            ("prlimit64_unenc_bad", "prlimit"),
            ("migrate_pages_unenc_bad", "move_pages"),
            ("fsmount_unenc_bad", "fsopen"),
            ("open_tree_unenc_bad", "fsopen"),
            ("move_mount_unenc_bad", "fsopen"),
            ("fspick_unenc_bad", "fsopen"),
            ("fsconfig_unenc_bad", "fsopen"),
            ("ioprio_get_unenc_bad", "ioprio"),
            ("futex_wake_unenc_bad", "futex_wait"),
            ("futex_requeue_unenc_bad", "futex_wait"),
            ("swapoff_unenc_bad", "swapon"),
            ("iopl_unenc_bad", "ioperm"),
            ("finit_module_unenc_bad", "init_module"),
            ("delete_module_unenc_bad", "init_module"),
            ("kexec_file_load_unenc_bad", "kexec"),
            ("pkey_mprotect_unenc_bad", "pkey_free"),
            ("mremap_unenc_bad", "msync"),
            ("getitimer_unenc_bad", "setitimer"),
            ("getdents64_unenc_bad", "getdents"),
            ("futimens_unenc_bad", "utimensat"),
            ("utimes_unenc_bad", "utimensat"),
            ("set_mempolicy_unenc_bad", "mbind"),
            ("get_mempolicy_unenc_bad", "mbind"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover19_libc_unenc_ok_still_proves(self):
        for name in (
            "clone_call_unenc_ok",
            "listmount_call_unenc_ok",
            "fallocate_call_unenc_ok",
            "epoll_create1_unenc_ok",
            "epoll_pwait2_unenc_ok",
            "rt_tgsigqueueinfo_unenc_ok",
            "file_setattr_unenc_ok",
            "clock_adjtime_unenc_ok",
            "clock_nanosleep_unenc_ok",
            "quick_exit_unenc_ok",
            "getopt_long_only_unenc_ok",
            "getopt_long_unenc_ok",
            "gethostname_unenc_ok",
            "copy_file_range_unenc_ok",
            "preadv2_unenc_ok",
            "pwritev2_unenc_ok",
            "pwritev_unenc_ok",
            "timerfd_gettime_unenc_ok",
            "eventfd_write_unenc_ok",
            "eventfd_unenc_ok",
            "timerfd_create_unenc_ok",
            "ptrace_unenc_ok",
            "tcsetattr_unenc_ok",
            "cfmakeraw_unenc_ok",
            "fpathconf_unenc_ok",
            "pathconf_unenc_ok",
            "ftw_unenc_ok",
            "wordfree_unenc_ok",
            "getlogin_r_unenc_ok",
            "ttyname_r_unenc_ok",
            "ttyname_unenc_ok",
            "inet_ntop_unenc_ok",
            "inet_aton_unenc_ok",
            "posix_fadvise64_unenc_ok",
            "vmsplice_unenc_ok",
            "inotify_init1_unenc_ok",
            "inotify_add_watch_unenc_ok",
            "fdatasync_unenc_ok",
            "getentropy_unenc_ok",
            "getdelim_unenc_ok",
            "strlcat_unenc_ok",
            "memset_s_unenc_ok",
            "explicit_memset_unenc_ok",
            "posix_openpt_unenc_ok",
            "ptsname_r_unenc_ok",
            "grantpt_unenc_ok",
            "unlockpt_unenc_ok",
            "umount2_unenc_ok",
            "umount_unenc_ok",
            "open_wmemstream_unenc_ok",
            "open_memstream_unenc_ok",
            "getxattrat_unenc_ok",
            "listxattrat_unenc_ok",
            "removexattrat_unenc_ok",
            "sched_getattr_unenc_ok",
            "sched_getscheduler_unenc_ok",
            "sched_setparam_unenc_ok",
            "sched_getparam_unenc_ok",
            "fanotify_mark_unenc_ok",
            "getgrgid_unenc_ok",
            "getspnam_unenc_ok",
            "lsm_set_self_attr_unenc_ok",
            "lsm_list_modules_unenc_ok",
            "sigsuspend_unenc_ok",
            "sigwaitinfo_unenc_ok",
            "sigtimedwait_unenc_ok",
            "sigpending_unenc_ok",
            "open_by_handle_at_unenc_ok",
            "prlimit64_unenc_ok",
            "migrate_pages_unenc_ok",
            "fsmount_unenc_ok",
            "open_tree_unenc_ok",
            "move_mount_unenc_ok",
            "fspick_unenc_ok",
            "fsconfig_unenc_ok",
            "ioprio_get_unenc_ok",
            "futex_wake_unenc_ok",
            "futex_requeue_unenc_ok",
            "swapoff_unenc_ok",
            "iopl_unenc_ok",
            "finit_module_unenc_ok",
            "delete_module_unenc_ok",
            "kexec_file_load_unenc_ok",
            "pkey_mprotect_unenc_ok",
            "mremap_unenc_ok",
            "getitimer_unenc_ok",
            "getdents64_unenc_ok",
            "futimens_unenc_ok",
            "utimes_unenc_ok",
            "set_mempolicy_unenc_ok",
            "get_mempolicy_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover19_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("clone_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unshare", r.message.lower())
        self.assertNotIn("setns", r.message.lower())
        r, _, _ = bmc("clone_call_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unshare", r.message.lower())
        self.assertNotIn("setns", r.message.lower())
        r, _, _ = bmc("setns_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unshare", r.message.lower())
        r, _, _ = bmc("listmount_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("statmount", r.message.lower())
        r, _, _ = bmc("listmount_call_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("listmount", r.message.lower())
        r, _, _ = bmc("fallocate_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fallocate", r.message.lower())
        r, _, _ = bmc("fallocate_call_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fallocate", r.message.lower())
        r, _, _ = bmc("memfd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("memfd", r.message.lower())
        self.assertNotIn("eventfd", r.message.lower())
        r, _, _ = bmc("eventfd_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("memfd", r.message.lower())
        r, _, _ = bmc("timerfd_create_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("memfd", r.message.lower())
        r, _, _ = bmc("getopt_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getopt", r.message.lower())
        self.assertNotIn("getopt_long", r.message.lower())
        r, _, _ = bmc("getopt_long_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("getopt", r.message.lower())
        r, _, _ = bmc("uname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uname", r.message.lower())
        self.assertNotIn("gethostname", r.message.lower())
        r, _, _ = bmc("gethostname_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("uname", r.message.lower())
        r, _, _ = bmc("sendfile_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendfile", r.message.lower())
        self.assertNotIn("copy_file_range", r.message.lower())
        r, _, _ = bmc("copy_file_range_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("sendfile", r.message.lower())
        r, _, _ = bmc("fsopen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fsopen", r.message.lower())
        self.assertNotIn("fsmount", r.message.lower())
        r, _, _ = bmc("fsmount_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fsopen", r.message.lower())
        r, _, _ = bmc("clone_call_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("eventfd_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("getopt_long_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)


    def test_leftover20_throw_with_nested_unenc_needs_harness(self):
        r, _, _ = bmc("twnested_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn("throw_with_nested", r.message.lower())
        self.assertNotIn(
            r.status,
            {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
        )

    def test_leftover20_throw_with_nested_unenc_ok_still_proves(self):
        r, _, _ = bmc("twnested_unenc_ok")
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover20_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("rinested_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("throw_with_nested", r.message.lower())
        r, _, _ = bmc("twnested_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("throw_with_nested", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover21_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("decimal32_unenc_bad", "decimal"),
            ("decimal128_unenc_bad", "decimal"),
            ("float32_unenc_bad", "ieee"),
            ("float64_unenc_bad", "ieee"),
            ("fp16_unenc_bad", "ieee"),
            ("bitint_unenc_bad", "128"),
            ("int128t_unenc_bad", "128"),
            ("slaarray_unenc_bad", "start_lifetime_as"),
            ("inoutptr_unenc_bad", "out_ptr"),
            ("sref_unenc_bad", "reference_wrapper"),
            ("scref_unenc_bad", "reference_wrapper"),
            ("csem_unenc_bad", "latch"),
            ("poly_unenc_bad", "indirect"),
            ("wview_unenc_bad", "wstring"),
            ("isps_unenc_bad", "spanstream"),
            ("osps_unenc_bad", "spanstream"),
            ("rcuobj_unenc_bad", "rcu"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover21_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "decimal32_unenc_ok",
            "decimal128_unenc_ok",
            "float32_unenc_ok",
            "float64_unenc_ok",
            "fp16_unenc_ok",
            "bitint_unenc_ok",
            "int128t_unenc_ok",
            "slaarray_unenc_ok",
            "inoutptr_unenc_ok",
            "sref_unenc_ok",
            "scref_unenc_ok",
            "csem_unenc_ok",
            "poly_unenc_ok",
            "wview_unenc_ok",
            "isps_unenc_ok",
            "osps_unenc_ok",
            "rcuobj_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover21_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("decimal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("decimal", r.message.lower())
        self.assertNotIn("decimal32", r.message.lower())
        r, _, _ = bmc("decimal32_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("decimal", r.message.lower())
        r, _, _ = bmc("float16_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("ieee", r.message.lower())
        self.assertNotIn("float32", r.message.lower())
        r, _, _ = bmc("float32_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("ieee", r.message.lower())
        r, _, _ = bmc("int128_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("128", r.message.lower())
        self.assertNotIn("bitint", r.message.lower())
        r, _, _ = bmc("bitint_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("128", r.message.lower())
        r, _, _ = bmc("lifetime_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("start_lifetime_as", r.message.lower())
        self.assertNotIn("start_lifetime_as_array", r.message.lower())
        r, _, _ = bmc("slaarray_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("start_lifetime_as", r.message.lower())
        r, _, _ = bmc("out_ptr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("out_ptr", r.message.lower())
        self.assertNotIn("inout_ptr", r.message.lower())
        r, _, _ = bmc("inoutptr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("out_ptr", r.message.lower())
        r, _, _ = bmc("refwrap_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("reference_wrapper", r.message.lower())
        r, _, _ = bmc("sref_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("reference_wrapper", r.message.lower())
        r, _, _ = bmc("latch_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("latch", r.message.lower())
        self.assertNotIn("counting_semaphore", r.message.lower())
        r, _, _ = bmc("csem_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("latch", r.message.lower())
        r, _, _ = bmc("indirect_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("indirect", r.message.lower())
        r, _, _ = bmc("poly_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("indirect", r.message.lower())
        r, _, _ = bmc("wstring_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wstring", r.message.lower())
        self.assertNotIn("wstring_view", r.message.lower())
        r, _, _ = bmc("wview_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wstring", r.message.lower())
        r, _, _ = bmc("spanstream_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("spanstream", r.message.lower())
        self.assertNotIn("ispanstream", r.message.lower())
        r, _, _ = bmc("isps_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("spanstream", r.message.lower())
        r, _, _ = bmc("rcu_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rcu", r.message.lower())
        self.assertNotIn("rcu_obj", r.message.lower())
        r, _, _ = bmc("rcuobj_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("rcu", r.message.lower())
        r, _, _ = bmc("decimal32_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("csem_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("poly_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)


    def test_leftover22_basic_stream_unenc_needs_harness(self):
        for name, needle in (
            ("bosync_unenc_bad", "osyncstream"),
            ("bsyncbuf_unenc_bad", "syncbuf"),
            ("bspan_unenc_bad", "spanstream"),
            ("bispan_unenc_bad", "spanstream"),
            ("bospan_unenc_bad", "spanstream"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover22_basic_stream_unenc_ok_still_proves(self):
        for name in (
            "bosync_unenc_ok",
            "bsyncbuf_unenc_ok",
            "bspan_unenc_ok",
            "bispan_unenc_ok",
            "bospan_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover22_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("isps_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("spanstream", r.message.lower())
        r, _, _ = bmc("bispan_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("spanstream", r.message.lower())
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)

    def test_leftover23_libc_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("tounqual_unenc_bad", "typeof_unqual"),
            ("dlsym_unenc_bad", "dlopen"),
            ("dlclose_unenc_bad", "dlopen"),
            ("clzll_unenc_bad", "clz"),
            ("ctz_unenc_bad", "clz"),
            ("ctzll_unenc_bad", "clz"),
            ("astore_unenc_bad", "atomic"),
            ("syncadd_unenc_bad", "atomic"),
            ("synccas_unenc_bad", "atomic"),
            ("wcscat_unenc_bad", "wcscpy"),
            ("wcsncpy_unenc_bad", "wcscpy"),
            ("wcsncat_unenc_bad", "wcscpy"),
            ("fork_unenc_bad", "fork"),
            ("vfork_unenc_bad", "fork"),
            ("execl_unenc_bad", "fork"),
            ("execlp_unenc_bad", "fork"),
            ("execle_unenc_bad", "fork"),
            ("execv_unenc_bad", "fork"),
            ("execve_unenc_bad", "fork"),
            ("execvp_unenc_bad", "fork"),
            ("execvpe_unenc_bad", "fork"),
            ("symlink_unenc_bad", "symlink"),
            ("readlink_unenc_bad", "symlink"),
            ("sstream_unenc_bad", "stringstream"),
            ("osstream_unenc_bad", "stringstream"),
            ("isstream_unenc_bad", "stringstream"),
            ("bsstream_unenc_bad", "stringstream"),
            ("bosstream_unenc_bad", "stringstream"),
            ("bisstream_unenc_bad", "stringstream"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover23_libc_cxx_unenc_ok_still_proves(self):
        for name in (
            "tounqual_unenc_ok",
            "dlsym_unenc_ok",
            "dlclose_unenc_ok",
            "clzll_unenc_ok",
            "ctz_unenc_ok",
            "ctzll_unenc_ok",
            "astore_unenc_ok",
            "syncadd_unenc_ok",
            "synccas_unenc_ok",
            "wcscat_unenc_ok",
            "wcsncpy_unenc_ok",
            "wcsncat_unenc_ok",
            "fork_unenc_ok",
            "vfork_unenc_ok",
            "execl_unenc_ok",
            "execlp_unenc_ok",
            "execle_unenc_ok",
            "execv_unenc_ok",
            "execve_unenc_ok",
            "execvp_unenc_ok",
            "execvpe_unenc_ok",
            "symlink_unenc_ok",
            "readlink_unenc_ok",
            "sstream_unenc_ok",
            "osstream_unenc_ok",
            "isstream_unenc_ok",
            "bsstream_unenc_ok",
            "bosstream_unenc_ok",
            "bisstream_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover23_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("typeof_unqual_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("typeof_unqual", r.message.lower())
        self.assertNotIn("__typeof_unqual__", r.message.lower())
        r, _, _ = bmc("tounqual_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("typeof_unqual", r.message.lower())
        r, _, _ = bmc("dlopen_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("dlopen", r.message.lower())
        self.assertNotIn("dlsym", r.message.lower())
        r, _, _ = bmc("dlsym_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("dlopen", r.message.lower())
        r, _, _ = bmc("clz_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("clz", r.message.lower())
        self.assertNotIn("ctz", r.message.lower())
        r, _, _ = bmc("ctz_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("clz", r.message.lower())
        r, _, _ = bmc("atomic_builtin_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("atomic", r.message.lower())
        self.assertNotIn("atomic_store", r.message.lower())
        r, _, _ = bmc("astore_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("atomic", r.message.lower())
        r, _, _ = bmc("wcs_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wcscpy", r.message.lower())
        self.assertNotIn("wcscat", r.message.lower())
        r, _, _ = bmc("wcscat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("wcscpy", r.message.lower())
        r, _, _ = bmc("fork_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fork", r.message.lower())
        self.assertNotIn("vfork", r.message.lower())
        r, _, _ = bmc("vfork_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("fork", r.message.lower())
        r, _, _ = bmc("symlink_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("symlink", r.message.lower())
        self.assertNotIn("readlink", r.message.lower())
        r, _, _ = bmc("readlink_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("symlink", r.message.lower())
        r, _, _ = bmc("sstream_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stringstream", r.message.lower())
        self.assertNotIn("ostringstream", r.message.lower())
        r, _, _ = bmc("osstream_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("stringstream", r.message.lower())
        r, _, _ = bmc("tounqual_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("fork_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("sstream_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover24_syntax_layout_container_unenc_needs_harness(self):
        for name, needle in (
            ("tlocal_unenc_bad", "thread-local"),
            ("complex_unenc_bad", "complex"),
            ("typeof_unenc_bad", "typeof"),
            ("alignof_unenc_bad", "alignof"),
            ("sthread_unenc_bad", "std::thread"),
            ("counted_unenc_bad", "counted_iterator"),
            ("vector_unenc_bad", "std::vector"),
            ("optional_unenc_bad", "optional"),
            ("variant_unenc_bad", "variant"),
            ("span_unenc_bad", "std::span"),
            ("ilist_unenc_bad", "initializer_list"),
            ("coro_unenc_bad", "coroutine"),
            ("packed_unenc_bad", "packed"),
            ("asm_unenc_bad", "asm"),
            ("generic_unenc_bad", "_generic"),
            ("offsetof_unenc_bad", "offsetof"),
            ("volatile_unenc_bad", "volatile"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover24_syntax_layout_container_unenc_ok_still_proves(self):
        for name in (
            "tlocal_unenc_ok",
            "complex_unenc_ok",
            "typeof_unenc_ok",
            "alignof_unenc_ok",
            "sthread_unenc_ok",
            "counted_unenc_ok",
            "vector_unenc_ok",
            "optional_unenc_ok",
            "variant_unenc_ok",
            "span_unenc_ok",
            "ilist_unenc_ok",
            "coro_unenc_ok",
            "packed_unenc_ok",
            "asm_unenc_ok",
            "generic_unenc_ok",
            "offsetof_unenc_ok",
            "volatile_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover24_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("tls_local_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thread-local", r.message.lower())
        r, _, _ = bmc("tlocal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("thread-local", r.message.lower())
        r, _, _ = bmc("typeof_unqual_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("typeof_unqual", r.message.lower())
        r, _, _ = bmc("typeof_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("typeof", r.message.lower())
        self.assertNotIn("typeof_unqual", r.message.lower())
        r, _, _ = bmc("jthread_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("jthread", r.message.lower())
        r, _, _ = bmc("sthread_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("std::thread", r.message.lower())
        self.assertNotIn("jthread", r.message.lower())
        r, _, _ = bmc("bspan_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("spanstream", r.message.lower())
        r, _, _ = bmc("span_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("std::span", r.message.lower())
        self.assertNotIn("spanstream", r.message.lower())
        r, _, _ = bmc("tlocal_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("vector_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover25_syntax_cxx_unenc_needs_harness(self):
        for name, needle in (
            ("rangefor_unenc_bad", "range-for"),
            ("lambda_unenc_bad", "lambda"),
            ("ccast_unenc_bad", "const_cast"),
            ("dcast_unenc_bad", "dynamic_cast"),
            ("tid_unenc_bad", "typeid"),
            ("rcast_unenc_bad", "reinterpret_cast"),
            ("sbind_unenc_bad", "std::bind"),
            ("inplace_unenc_bad", "inplace_vector"),
            ("catchall_unenc_bad", "catch-all"),
            ("thrownew_unenc_bad", "throw-new"),
            ("sfrom_unenc_bad", "shared_from_this"),
            ("ppack_unenc_bad", "pragma pack"),
            ("widech_unenc_bad", "wide character"),
            ("widestr_unenc_bad", "wide character"),
            ("trycatch_unenc_bad", "try/catch"),
            ("newdel_unenc_bad", "new/delete"),
            ("cgoto_unenc_bad", "computed goto"),
            ("laddr_unenc_bad", "label-address"),
            ("vaarg_unenc_bad", "va_arg"),
            ("dinit_unenc_bad", "designated init"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover25_syntax_cxx_unenc_ok_still_proves(self):
        for name in (
            "rangefor_unenc_ok",
            "lambda_unenc_ok",
            "ccast_unenc_ok",
            "dcast_unenc_ok",
            "tid_unenc_ok",
            "rcast_unenc_ok",
            "sbind_unenc_ok",
            "inplace_unenc_ok",
            "catchall_unenc_ok",
            "thrownew_unenc_ok",
            "sfrom_unenc_ok",
            "ppack_unenc_ok",
            "widech_unenc_ok",
            "widestr_unenc_ok",
            "trycatch_unenc_ok",
            "newdel_unenc_ok",
            "cgoto_unenc_ok",
            "laddr_unenc_ok",
            "vaarg_unenc_ok",
            "dinit_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover25_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("bind_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotIn("std::bind", r.message.lower())
        r, _, _ = bmc("sbind_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("std::bind", r.message.lower())
        r, _, _ = bmc("packed_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("packed", r.message.lower())
        self.assertNotIn("pragma pack", r.message.lower())
        r, _, _ = bmc("ppack_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pragma pack", r.message.lower())
        r, _, _ = bmc("catchall_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("catch-all", r.message.lower())
        r, _, _ = bmc("trycatch_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("try/catch", r.message.lower())
        self.assertNotIn("catch-all", r.message.lower())
        r, _, _ = bmc("thrownew_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("throw-new", r.message.lower())
        r, _, _ = bmc("cgoto_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("computed goto", r.message.lower())
        self.assertNotIn("label-address", r.message.lower())
        r, _, _ = bmc("laddr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("label-address", r.message.lower())
        r, _, _ = bmc("vector_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("std::vector", r.message.lower())
        self.assertNotIn("inplace_vector", r.message.lower())
        r, _, _ = bmc("inplace_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("inplace_vector", r.message.lower())
        r, _, _ = bmc("ppack_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("lambda_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover26_layout_libc_unenc_needs_harness(self):
        for name, needle in (
            ("nfn_unenc_bad", "nested function"),
            ("uaddr_unenc_bad", "address-of"),
            ("clocal_unenc_bad", "const local"),
            ("regstor_unenc_bad", "register/auto"),
            ("autostor_unenc_bad", "register/auto"),
            ("slocal_unenc_bad", "struct/union local"),
            ("staticloc_unenc_bad", "static/extern"),
            ("externloc_unenc_bad", "static/extern"),
            ("stmtexpr_unenc_bad", "statement expression"),
            ("autotype_unenc_bad", "__auto_type"),
            ("aenum_unenc_bad", "anonymous enum"),
            ("alignas_unenc_bad", "_alignas"),
            ("compound_unenc_bad", "compound literal"),
            ("rviews_unenc_bad", "ranges views"),
            ("unlink_unenc_bad", "unlink"),
            ("strinit_unenc_bad", "array string-init"),
            ("pmtx_unenc_bad", "mutex object"),
            ("cmtx_unenc_bad", "mutex object"),
            ("utypedef_unenc_bad", "unknown typedef"),
            ("memcpy_unenc_bad", "libc buffer"),
            ("memmove_unenc_bad", "libc buffer"),
            ("mkstemp_unenc_bad", "libc buffer"),
            ("chroot_unenc_bad", "libc buffer"),
            ("popen_unenc_bad", "libc buffer"),
            ("umask_unenc_bad", "libc buffer"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover26_layout_libc_unenc_ok_still_proves(self):
        for name in (
            "nfn_unenc_ok",
            "uaddr_unenc_ok",
            "clocal_unenc_ok",
            "regstor_unenc_ok",
            "autostor_unenc_ok",
            "slocal_unenc_ok",
            "staticloc_unenc_ok",
            "externloc_unenc_ok",
            "stmtexpr_unenc_ok",
            "autotype_unenc_ok",
            "aenum_unenc_ok",
            "alignas_unenc_ok",
            "compound_unenc_ok",
            "rviews_unenc_ok",
            "unlink_unenc_ok",
            "strinit_unenc_ok",
            "pmtx_unenc_ok",
            "cmtx_unenc_ok",
            "utypedef_unenc_ok",
            "memcpy_unenc_ok",
            "memmove_unenc_ok",
            "mkstemp_unenc_ok",
            "chroot_unenc_ok",
            "popen_unenc_ok",
            "umask_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover26_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("unlinkat_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unlinkat", r.message.lower())
        r, _, _ = bmc("unlink_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("unlink", r.message.lower())
        self.assertNotIn("unlinkat", r.message.lower())
        r, _, _ = bmc("regstor_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("register/auto", r.message.lower())
        self.assertNotIn("__auto_type", r.message.lower())
        r, _, _ = bmc("autotype_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("__auto_type", r.message.lower())
        r, _, _ = bmc("pmtx_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("mutex object", r.message.lower())
        r, _, _ = bmc("memcpy_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("libc buffer", r.message.lower())
        r, _, _ = bmc("clocal_unenc_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)

    def test_leftover27_sibling_unenc_needs_harness(self):
        for name, needle in (
            ("anycast_lt_unenc_bad", "std::any"),
            ("anycast_id_unenc_bad", "std::any"),
            ("stdfs_unenc_bad", "std::filesystem"),
            ("filesystem_ns_unenc_bad", "std::filesystem"),
            ("regex_match_unenc_bad", "std::regex"),
            ("regex_var_unenc_bad", "std::regex"),
            ("indirect_lt_unenc_bad", "indirect"),
            ("polymorphic_lt_unenc_bad", "indirect"),
            ("int_const_unenc_bad", "const local"),
            ("for_const_unenc_bad", "const local"),
            ("std_bit_cast_unenc_bad", "bit_cast"),
            ("function_lt_unenc_bad", "std::function"),
            ("std_mdspan_unenc_bad", "std::mdspan"),
            ("atomic_ref_lt_unenc_bad", "std::atomic_ref"),
            ("generator_lt_unenc_bad", "std::generator"),
            ("from_chars_bare_unenc_bad", "from_chars"),
            ("flat_map_lt_unenc_bad", "flat_map"),
            ("flat_set_lt_unenc_bad", "flat_set"),
            ("flat_mset_lt_unenc_bad", "flat_multiset"),
            ("flat_mmap_lt_unenc_bad", "flat_multimap"),
            ("chrono_ns_unenc_bad", "chrono"),
            ("ranges_views_std_unenc_bad", "ranges views"),
            ("hive_lt_unenc_bad", "hive"),
            ("bitset_lt_unenc_bad", "bitset"),
            ("linalg_ns_unenc_bad", "linalg"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover27_sibling_unenc_ok_still_proves(self):
        for name in (
            "anycast_lt_unenc_ok",
            "anycast_id_unenc_ok",
            "stdfs_unenc_ok",
            "filesystem_ns_unenc_ok",
            "regex_match_unenc_ok",
            "regex_var_unenc_ok",
            "indirect_lt_unenc_ok",
            "polymorphic_lt_unenc_ok",
            "int_const_unenc_ok",
            "for_const_unenc_ok",
            "std_bit_cast_unenc_ok",
            "function_lt_unenc_ok",
            "std_mdspan_unenc_ok",
            "atomic_ref_lt_unenc_ok",
            "generator_lt_unenc_ok",
            "from_chars_bare_unenc_ok",
            "flat_map_lt_unenc_ok",
            "flat_set_lt_unenc_ok",
            "flat_mset_lt_unenc_ok",
            "flat_mmap_lt_unenc_ok",
            "chrono_ns_unenc_ok",
            "ranges_views_std_unenc_ok",
            "hive_lt_unenc_ok",
            "bitset_lt_unenc_ok",
            "linalg_ns_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover28_extra_sibling_unenc_needs_harness(self):
        for name, needle in (
            ("pmr_ns_unenc_bad", "pmr"),
            ("rcu_obj_lt_unenc_bad", "rcu"),
            ("rcu_sync_only_unenc_bad", "rcu"),
            ("reflect_caret_unenc_bad", "reflection"),
            ("jthread_bare_unenc_bad", "jthread"),
            ("packaged_lt_unenc_bad", "packaged_task"),
            ("lock_guard_lt_unenc_bad", "mutex"),
            ("cv_bare_unenc_bad", "condition_variable"),
            ("future_lt_unenc_bad", "async"),
            ("latch_ctor_unenc_bad", "latch"),
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(needle, r.message.lower())
                self.assertNotIn(
                    r.status,
                    {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED, laws.FAILED},
                )

    def test_leftover28_extra_sibling_unenc_ok_still_proves(self):
        for name in (
            "pmr_ns_unenc_ok",
            "rcu_obj_lt_unenc_ok",
            "rcu_sync_only_unenc_ok",
            "reflect_caret_unenc_ok",
            "jthread_bare_unenc_ok",
            "packaged_lt_unenc_ok",
            "lock_guard_lt_unenc_ok",
            "cv_bare_unenc_ok",
            "future_lt_unenc_ok",
            "latch_ctor_unenc_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)

    def test_leftover27_comment_strip_still_proves(self):
        for name in (
            "nullptr_comment_ok",
            "ppack_comment_ok",
            "import_comment_ok",
        ):
            with self.subTest(name=name):
                r, _, _ = bmc(name)
                self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
                self.assertNotEqual(r.status, laws.ERROR, r.message)
                self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
                low = r.message.lower()
                self.assertNotIn("nullptr", low)
                self.assertNotIn("pragma pack", low)
                self.assertNotIn("module import", low)

    def test_leftover27_neighbors_and_goto_abs_regression(self):
        r, _, _ = bmc("cv_bare_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("condition_variable", r.message.lower())
        self.assertNotIn("condition_variable_any", r.message.lower())
        r, _, _ = bmc("cvany_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("condition_variable_any", r.message.lower())
        r, _, _ = bmc("jthread_bare_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("jthread", r.message.lower())
        self.assertNotIn("thread-lifetime", r.message.lower())
        r, _, _ = bmc("int_const_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("const local", r.message.lower())
        r, _, _ = bmc("clocal_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("const local", r.message.lower())
        r, _, _ = bmc("std_bit_cast_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("bit_cast", r.message.lower())
        r, _, _ = bmc("nullptr_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("nullptr", r.message.lower())
        r, _, _ = bmc("ppack_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("pragma pack", r.message.lower())
        r, _, _ = bmc("import_unenc_bad")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertIn("module import", r.message.lower())
        r, _, _ = bmc("nullptr_comment_ok")
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED}, r.message)
        r, _, _ = bmc("with_goto")
        self.assertEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        r, _, _ = bmc("abs_ok")
        self.assertEqual(r.status, laws.PROVED_UNBOUNDED, r.message)
        self.assertNotEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertNotEqual(r.status, laws.BOUNDED, r.message)


if __name__ == "__main__":
    unittest.main()

