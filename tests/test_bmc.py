"""BMC frontend: switch/enum/loops plus existing scalar oracles."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.bmc import HAS_Z3, bmc_function, extract_enums
from helix.cparse import extract_functions
from helix.models import FunctionInfo

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
        from helix.bmc import k_induction
        f, _ = load("kinduct_closed")
        rec = k_induction(f, 8)
        self.assertEqual(rec.status, laws.PROVED_UNBOUNDED, rec.message)
        self.assertEqual(rec.extra.get("k_induction"), "closed")
        self.assertEqual(rec.extra.get("k_induction_k"), 1)
        raw, _, _ = bmc("kinduct_closed")
        self.assertEqual(raw.status, laws.BOUNDED, raw.message)

    def test_open_step_stays_bounded_never_failed(self):
        from helix.bmc import k_induction
        f, _ = load("kinduct_step_open")
        rec = k_induction(f, 8)
        self.assertEqual(rec.status, laws.BOUNDED, rec.message)
        self.assertNotEqual(rec.status, laws.FAILED)
        self.assertEqual(rec.extra.get("k_induction"), "step-open")
        self.assertEqual(rec.extra.get("k_induction_tried"), [1, 2])

    def test_nested_ok_never_failed(self):
        from helix.bmc import k_induction
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
        from helix.bmc import k_induction
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


if __name__ == "__main__":
    unittest.main()
