"""Unit tests that do not need the model. python -m unittest tests.test_helix"""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.bmc import HAS_Z3, bmc_function
from helix.checkers import run_lints
from helix.cparse import body_needs_pointer_harness, extract_functions
from helix.laws import refuse_merge

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestLaws(unittest.TestCase):
    def test_refuse_merge_proofs(self):
        with self.assertRaises(ValueError):
            refuse_merge(laws.PROVED, laws.BOUNDED)

    def test_refuse_promote_fuzz(self):
        with self.assertRaises(ValueError):
            refuse_merge(laws.CLEAN, laws.PROVED)


class TestClassify(unittest.TestCase):
    def test_scalar(self):
        f, _ = fn("add_overflow")
        self.assertEqual(f.kind, "SCALAR")

    def test_pointer(self):
        f, _ = fn("null_branch")
        self.assertEqual(f.kind, "POINTER")

    def test_void_params(self):
        # shift_ub takes an unused int — SCALAR
        f, _ = fn("shift_ub")
        self.assertEqual(f.kind, "SCALAR")

    def test_gnu_attribute_is_not_pointer(self):
        f, _ = fn("dead_attr")
        self.assertEqual(f.kind, "SCALAR")
        self.assertEqual([p[1] for p in f.params], ["c"])

    def test_char_literal_survives_comment_strip(self):
        f, _ = fn("trunc_ok")
        self.assertIn("'A'", f.body)

    def test_unchecked_alloc_is_void(self):
        f, _ = fn("unchecked_alloc")
        self.assertEqual(f.kind, "VOID")


class TestLints(unittest.TestCase):
    def test_shift(self):
        hits = [f for f in run_lints([TD / "shift_ub.c"], TD) if f.cls == "INT-SHIFT-UB"]
        self.assertTrue(hits)

    def test_realloc(self):
        hits = [f for f in run_lints([TD / "realloc_self.c"], TD) if f.cls == "MEM-REALLOC-SELF"]
        self.assertTrue(hits)

    def test_null_branch(self):
        hits = [f for f in run_lints([TD / "null_branch.c"], TD) if f.cls == "PTR-NULL-DEREF"]
        self.assertTrue(hits)

    def test_masked(self):
        hits = [f for f in run_lints([TD / "masked_switch.c"], TD) if f.cls == "UNINIT-SWITCH"]
        self.assertTrue(hits)

    def test_lock(self):
        hits = [f for f in run_lints([TD / "lock_imbalance.c"], TD) if f.cls == "LOCK-IMBALANCE"]
        self.assertTrue(hits)

    def test_unchecked_alloc(self):
        hits = [f for f in run_lints([TD / "unchecked_alloc.c"], TD)
                if f.cls == "PTR-UNCHECKED-ALLOC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unchecked_alloc", names)
        self.assertNotIn("checked_alloc", names)
        self.assertNotIn("alloc_in_condition", names)

    def test_noreturn_fatal(self):
        hits = [f for f in run_lints([TD / "noreturn_fatal.c"], TD)
                if f.cls == "FUNC-NORETURN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fatal", names)
        self.assertNotIn("dead_noreturn", names)
        self.assertNotIn("dead_attr", names)

    def test_nowait_use(self):
        hits = [f for f in run_lints([TD / "nowait_use.c"], TD)
                if f.cls == "MEM-NOWAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nowait_unchecked", names)
        self.assertNotIn("nowait_checked", names)

    def test_uaf(self):
        hits = [f for f in run_lints([TD / "uaf.c"], TD) if f.cls == "MEM-UAF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uaf_bad", names)
        self.assertNotIn("checked_use", names)
        self.assertTrue(all(f.status == laws.FAILED for f in hits))
        self.assertFalse(any(f.status == laws.PROVED for f in hits))

    def test_double_free(self):
        hits = [f for f in run_lints([TD / "double_free.c"], TD)
                if f.cls == "MEM-DOUBLE-FREE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("double_free_bad", names)

    def test_format_string(self):
        hits = [f for f in run_lints([TD / "format.c"], TD) if f.cls == "FMT-STRING"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fmt_bad", names)
        self.assertNotIn("fmt_ok", names)

    def test_memset_swap(self):
        hits = [f for f in run_lints([TD / "memset_swap.c"], TD)
                if f.cls == "MEM-MEMSET-SWAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("memset_swap_bad", names)
        self.assertNotIn("memset_ok", names)

    def test_taut_bound(self):
        hits = [f for f in run_lints([TD / "taut_bound.c"], TD)
                if f.cls == "INT-TAUTOLOGY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("taut_bound_bad", names)
        self.assertNotIn("taut_bound_ok", names)

    def test_wrap_alloc(self):
        hits = [f for f in run_lints([TD / "malloc_wrap.c"], TD)
                if f.cls == "INT-WRAP-ALLOC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wrap_alloc_bad", names)
        self.assertNotIn("calloc_ok", names)

    def test_trunc(self):
        hits = [f for f in run_lints([TD / "trunc.c"], TD) if f.cls == "INT-TRUNC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("trunc_bad", names)
        self.assertIn("trunc_assign_bad", names)
        self.assertNotIn("trunc_ok", names)

    def test_sign_compare(self):
        hits = [f for f in run_lints([TD / "sign_compare.c"], TD)
                if f.cls == "INT-SIGN-CONV"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sign_compare_bad", names)
        self.assertNotIn("sign_compare_ok", names)
        self.assertNotIn("sign_compare_ok2", names)

    def test_cxx_new_delete(self):
        hits = [f for f in run_lints([TD / "cxx_newdel.cpp"], TD)
                if f.cls == "MEM-NEW-DELETE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("new_mismatch_bad", names)
        self.assertNotIn("new_mismatch_ok", names)
        self.assertNotIn("new_scalar_ok", names)

    def test_stack_escape(self):
        hits = [f for f in run_lints([TD / "stack_escape.c"], TD)
                if f.cls == "MEM-STACK-ESCAPE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("esc_bad", names)
        self.assertIn("arr_esc_bad", names)
        self.assertIn("esc_paren_addr", names)
        self.assertNotIn("esc_ok", names)

    def test_array_decay_needs_pointer_harness(self):
        f, _ = fn("arr_esc_bad")
        self.assertTrue(body_needs_pointer_harness(f.body))
        f2, _ = fn("esc_bad")
        self.assertTrue(body_needs_pointer_harness(f2.body))
        # `return buf[0]` is a scalar element, not array decay.
        f3, _ = fn("write_slot")
        self.assertFalse(body_needs_pointer_harness(f3.body))
        f4, _ = fn("caller")
        self.assertFalse(body_needs_pointer_harness(f4.body))
        f5, _ = fn("arr_esc_plus0")
        self.assertTrue(body_needs_pointer_harness(f5.body))
        f6, _ = fn("arr_esc_plusi")
        self.assertTrue(body_needs_pointer_harness(f6.body))
        f7, _ = fn("arr_esc_plus_rhs")
        self.assertTrue(body_needs_pointer_harness(f7.body))
        f8, _ = fn("arr_esc_paren")
        self.assertTrue(body_needs_pointer_harness(f8.body))
        f9, _ = fn("esc_paren_addr")
        self.assertTrue(body_needs_pointer_harness(f9.body))

    def test_alloca_needs_pointer_harness(self):
        f, _ = fn("alloca_bad")
        self.assertTrue(body_needs_pointer_harness(f.body))
        f2, _ = fn("alloca_ok")
        self.assertTrue(body_needs_pointer_harness(f2.body))
        # alloca itself is the missing stack-frame model, even without char *.
        self.assertTrue(body_needs_pointer_harness("alloca(n);"))
        self.assertTrue(body_needs_pointer_harness("__builtin_alloca(16);"))
        self.assertFalse(body_needs_pointer_harness("return n + 1;"))

    def test_unbounded_copy(self):
        hits = [f for f in run_lints([TD / "unbounded_copy.c"], TD)
                if f.cls == "STR-UNBOUNDED-COPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("copy_bad", names)
        self.assertNotIn("copy_ok", names)

    def test_missing_return(self):
        hits = [f for f in run_lints([TD / "missing_return.c"], TD)
                if f.cls == "CTRL-MISSING-RETURN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("missing_return_bad", names)
        self.assertIn("missing_return_oneline_bad", names)
        self.assertNotIn("missing_return_ok", names)
        self.assertNotIn("missing_return_oneline_ok", names)

    def test_fallthrough(self):
        hits = [f for f in run_lints([TD / "fallthrough.c"], TD)
                if f.cls == "CTRL-FALLTHROUGH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fall_bad", names)
        self.assertNotIn("fall_ok", names)

    def test_dead_guard(self):
        hits = [f for f in run_lints([TD / "dead_guard.c"], TD)
                if f.cls == "CTRL-DEAD-GUARD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dead_guard_bad", names)
        self.assertNotIn("dead_guard_ok", names)

    def test_uninit_return(self):
        hits = [f for f in run_lints([TD / "uninit_return.c"], TD)
                if f.cls == "UNINIT-RETURN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uninit_ret_bad", names)
        self.assertNotIn("uninit_ret_ok", names)

    def test_ptr_uninit(self):
        hits = [f for f in run_lints([TD / "ptr_uninit.c"], TD)
                if f.cls == "PTR-UNINIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ptr_uninit_bad", names)
        self.assertNotIn("ptr_uninit_ok", names)

    def test_uninit_branch(self):
        hits = [f for f in run_lints([TD / "uninit_branch.c"], TD)
                if f.cls == "UNINIT-BRANCH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uninit_br_bad", names)
        self.assertNotIn("uninit_br_ok", names)
        self.assertNotIn("uninit_br_ok2", names)

    def test_off_by_one(self):
        hits = [f for f in run_lints([TD / "off_by_one.c"], TD)
                if f.cls == "STR-OFF-BY-ONE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("offby_bad", names)
        self.assertNotIn("offby_ok", names)

    def test_sibling_guard(self):
        hits = [f for f in run_lints([TD / "sibling_guard.c"], TD)
                if f.cls == "CTRL-SIBLING-ASYMMETRY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("write_slot", names)
        self.assertNotIn("both_ok_write", names)
        self.assertNotIn("read_slot", names)

    def test_ignored_error(self):
        hits = [f for f in run_lints([TD / "ignored_error.c"], TD)
                if f.cls == "API-IGNORED-ERROR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ignored_bad", names)
        self.assertNotIn("ignored_ok", names)

    def test_fd_leak(self):
        hits = [f for f in run_lints([TD / "fd_leak.c"], TD)
                if f.cls == "RES-FD-LEAK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fd_leak_bad", names)
        self.assertNotIn("fd_leak_ok", names)

    def test_vla_size(self):
        hits = [f for f in run_lints([TD / "vla.c"], TD)
                if f.cls == "MEM-VLA-SIZE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vla_bad", names)
        self.assertNotIn("vla_ok", names)

    def test_mem_leak(self):
        hits = [f for f in run_lints([TD / "mem_leak.c"], TD)
                if f.cls == "MEM-LEAK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mem_leak_bad", names)
        self.assertNotIn("mem_leak_ok", names)

    def test_null_arg(self):
        hits = [f for f in run_lints([TD / "null_arg.c"], TD)
                if f.cls == "STR-NULL-ARG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("null_arg_bad", names)
        self.assertNotIn("null_arg_ok", names)

    def test_use_after_move(self):
        hits = [f for f in run_lints([TD / "use_after_move.cpp"], TD)
                if f.cls == "CXX-USE-AFTER-MOVE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("move_bad", names)
        self.assertNotIn("move_ok", names)

    def test_self_assign(self):
        hits = [f for f in run_lints([TD / "self_assign.cpp"], TD)
                if f.cls == "CXX-SELF-ASSIGN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("assign_bad", names)
        self.assertNotIn("assign_ok", names)

    def test_mismatched_free(self):
        hits = [f for f in run_lints([TD / "mismatched_free.c"], TD)
                if f.cls == "MEM-MISMATCHED-FREE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mismatch_free_bad", names)
        self.assertNotIn("mismatch_free_ok", names)

    def test_intent(self):
        hits = [f for f in run_lints([TD / "intent.c"], TD) if f.cls == "INTENT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("intent_bad", names)
        self.assertNotIn("intent_ok", names)
        for stem in ("abs_ok", "saturate"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD) if f.cls == "INTENT"]
            self.assertFalse(other, msg=stem)

    def test_lock_order(self):
        hits = [f for f in run_lints([TD / "lock_order.c"], TD) if f.cls == "LOCK-ORDER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lock_ba", names)
        self.assertIn("lock_ab", names)
        ok = [f for f in run_lints([TD / "lock_ok.c"], TD) if f.cls == "LOCK-ORDER"]
        self.assertFalse(ok)
        for stem in ("abs_ok", "saturate"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD) if f.cls == "LOCK-ORDER"]
            self.assertFalse(other, msg=stem)

    def test_dangling_ref(self):
        hits = [f for f in run_lints([TD / "dangling_ref.cpp"], TD)
                if f.cls == "CXX-DANGLING-REF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("view_bad", names)
        self.assertNotIn("view_ok", names)
        for stem in ("abs_ok", "stack_escape"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CXX-DANGLING-REF"]
            self.assertFalse(other, msg=stem)

    def test_iter_invalid(self):
        hits = [f for f in run_lints([TD / "iter_invalid.cpp"], TD)
                if f.cls == "CXX-ITERATOR-INVALID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("iter_bad", names)
        self.assertNotIn("iter_ok", names)

    def test_virtual_ctor(self):
        hits = [f for f in run_lints([TD / "virtual_ctor.cpp"], TD)
                if f.cls == "CXX-VIRTUAL-IN-CTOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("Widget_ctor_bad", names)
        self.assertNotIn("Widget_ctor_ok", names)
        for stem in ("abs_ok", "saturate"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                       if f.cls == "CXX-VIRTUAL-IN-CTOR"]
            self.assertFalse(other, msg=stem)

    def test_toctou(self):
        hits = [f for f in run_lints([TD / "toctou.c"], TD) if f.cls == "CONC-TOCTOU"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("toctou_bad", names)
        self.assertNotIn("toctou_ok", names)
        for stem in ("abs_ok", "saturate"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD) if f.cls == "CONC-TOCTOU"]
            self.assertFalse(other, msg=stem)

    def test_ptr_arith(self):
        hits = [f for f in run_lints([TD / "ptr_arith.c"], TD)
                if f.cls == "MEM-PTR-ARITH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("arith_bad", names)
        self.assertNotIn("arith_ok", names)
        other = [f for f in run_lints([TD / "abs_ok.c"], TD) if f.cls == "MEM-PTR-ARITH"]
        self.assertFalse(other)

    def test_float_ub(self):
        hits = [f for f in run_lints([TD / "float_ub.c"], TD) if f.cls == "FLOAT-UB"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fdiv_bad", names)
        self.assertNotIn("fdiv_ok", names)

    def test_infoleak_pad(self):
        hits = [f for f in run_lints([TD / "infoleak_pad.c"], TD)
                if f.cls == "INFOLEAK-PAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("leak_bad", names)
        self.assertNotIn("leak_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "INFOLEAK-PAD"]
            self.assertFalse(other, msg=stem)

    def test_api_precondition(self):
        hits = [f for f in run_lints([TD / "api_precond.c"], TD)
                if f.cls == "API-PRECONDITION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("precond_bad", names)
        self.assertNotIn("precond_ok", names)
        for stem in ("abs_ok", "intent"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PRECONDITION"]
            self.assertFalse(other, msg=stem)
        intent = [f for f in run_lints([TD / "api_precond.c"], TD) if f.cls == "INTENT"]
        self.assertFalse(intent)

    def test_overlap(self):
        hits = [f for f in run_lints([TD / "overlap.c"], TD) if f.cls == "MEM-OVERLAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("overlap_bad", names)
        self.assertNotIn("overlap_ok", names)
        self.assertNotIn("overlap_ok2", names)
        other = [f for f in run_lints([TD / "abs_ok.c"], TD) if f.cls == "MEM-OVERLAP"]
        self.assertFalse(other)

    def test_unvalidated_input(self):
        hits = [f for f in run_lints([TD / "unvalidated.c"], TD)
                if f.cls == "TRUST-UNVALIDATED-INPUT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unval_bad", names)
        self.assertIn("unval_div_bad", names)
        self.assertNotIn("unval_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "TRUST-UNVALIDATED-INPUT"]
            self.assertFalse(other, msg=stem)
        taint = [f for f in run_lints([TD / "taint_sink.c"], TD)
                 if f.cls == "TRUST-UNVALIDATED-INPUT"]
        self.assertFalse(taint)

    def test_crypto_misuse(self):
        hits = [f for f in run_lints([TD / "crypto_misuse.c"], TD)
                if f.cls == "CRYPTO-MISUSE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("crypto_key_bad", names)
        self.assertNotIn("crypto_key_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CRYPTO-MISUSE"]
            self.assertFalse(other, msg=stem)

    def test_atomicity(self):
        hits = [f for f in run_lints([TD / "atomicity.c"], TD)
                if f.cls == "CONC-ATOMICITY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("atom_bad", names)
        self.assertNotIn("atom_ok", names)
        for stem in ("abs_ok", "race_global"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CONC-ATOMICITY"]
            self.assertFalse(other, msg=stem)

    def test_exception_leak(self):
        hits = [f for f in run_lints([TD / "exception_leak.cpp"], TD)
                if f.cls == "CXX-EXCEPTION-LEAK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("exc_leak_bad", names)
        self.assertNotIn("exc_leak_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CXX-EXCEPTION-LEAK"]
            self.assertFalse(other, msg=stem)
        cxx = [f for f in run_lints([TD / "cxx_newdel.cpp"], TD)
               if f.cls == "CXX-EXCEPTION-LEAK"]
        self.assertFalse(cxx)

    def test_double_unlock(self):
        hits = [f for f in run_lints([TD / "double_unlock.c"], TD)
                if f.cls == "LOCK-DOUBLE-UNLOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("double_unlock_bad", names)
        self.assertNotIn("double_unlock_ok", names)
        for stem in ("abs_ok", "lock_ok", "lock_imbalance"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "LOCK-DOUBLE-UNLOCK"]
            self.assertFalse(other, msg=stem)

    def test_throw_destructor(self):
        hits = [f for f in run_lints([TD / "throw_dtor.cpp"], TD)
                if f.cls == "CXX-THROW-DESTRUCTOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("~ThrowBad", names)
        self.assertNotIn("~ThrowOk", names)
        self.assertNotIn("throws_not_dtor", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CXX-THROW-DESTRUCTOR"]
            self.assertFalse(other, msg=stem)
        cxx = [f for f in run_lints([TD / "exception_leak.cpp"], TD)
               if f.cls == "CXX-THROW-DESTRUCTOR"]
        self.assertFalse(cxx)

    def test_double_lock(self):
        hits = [f for f in run_lints([TD / "double_lock.c"], TD)
                if f.cls == "LOCK-DOUBLE-LOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("double_lock_bad", names)
        self.assertNotIn("double_lock_ok", names)
        for stem in ("abs_ok", "lock_ok", "lock_imbalance", "double_unlock"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "LOCK-DOUBLE-LOCK"]
            self.assertFalse(other, msg=stem)

    def test_alloca(self):
        hits = [f for f in run_lints([TD / "alloca_var.c"], TD)
                if f.cls == "MEM-ALLOCA"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("alloca_bad", names)
        self.assertNotIn("alloca_ok", names)
        other = [f for f in run_lints([TD / "vla.c"], TD) if f.cls == "MEM-ALLOCA"]
        self.assertFalse(other)
        vla = [f for f in run_lints([TD / "alloca_var.c"], TD) if f.cls == "MEM-VLA-SIZE"]
        self.assertFalse(vla)

    def test_scanf_unchecked(self):
        hits = [f for f in run_lints([TD / "scanf_unchecked.c"], TD)
                if f.cls == "API-SCANF-UNCHECKED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("scanf_bad", names)
        self.assertNotIn("scanf_ok", names)
        self.assertNotIn("sscanf_ok", names)
        for stem in ("abs_ok", "ignored_error", "unvalidated"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCANF-UNCHECKED"]
            self.assertFalse(other, msg=stem)
        ignored = [f for f in run_lints([TD / "scanf_unchecked.c"], TD)
                   if f.cls == "API-IGNORED-ERROR"]
        self.assertFalse(ignored)

    def test_gets(self):
        hits = [f for f in run_lints([TD / "gets.c"], TD) if f.cls == "API-GETS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gets_bad", names)
        self.assertNotIn("gets_ok", names)
        unbounded = [f for f in run_lints([TD / "gets.c"], TD)
                     if f.cls == "STR-UNBOUNDED-COPY"]
        self.assertFalse(unbounded)
        for stem in ("abs_ok", "unbounded_copy", "scanf_unchecked"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETS"]
            self.assertFalse(other, msg=stem)

    def test_throw_spec(self):
        hits = [f for f in run_lints([TD / "throw_spec.cpp"], TD)
                if f.cls == "CXX-THROW-SPEC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("throw_spec_bad", names)
        self.assertIn("throw_spec_dynamic_bad", names)
        self.assertNotIn("throw_spec_ok", names)
        self.assertNotIn("throw_stmt_ok", names)
        parsed = extract_functions(TD / "throw_spec.cpp", "throw_spec.cpp")
        by = {f.name: f for f in parsed}
        self.assertIn("throw_spec_ok", by)
        self.assertEqual(by["throw_spec_bad"].kind, "VOID")
        self.assertEqual(by["throw_spec_ok"].kind, "VOID")
        dtor = [f for f in run_lints([TD / "throw_spec.cpp"], TD)
                if f.cls == "CXX-THROW-DESTRUCTOR"]
        self.assertFalse(dtor)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CXX-THROW-SPEC"]
            self.assertFalse(other, msg=stem)
        cxx = [f for f in run_lints([TD / "throw_dtor.cpp"], TD)
               if f.cls == "CXX-THROW-SPEC"]
        self.assertFalse(cxx)
        cxx2 = [f for f in run_lints([TD / "exception_leak.cpp"], TD)
                if f.cls == "CXX-THROW-SPEC"]
        self.assertFalse(cxx2)

    def test_enum_hole(self):
        hits = [f for f in run_lints([TD / "enum_hole.c"], TD)
                if f.cls == "INT-ENUM-HOLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("enum_hole_bad", names)
        self.assertNotIn("enum_hole_ok", names)
        self.assertNotIn("enum_hole_default_ok", names)
        for stem in ("abs_ok", "masked_switch", "fallthrough"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "INT-ENUM-HOLE"]
            self.assertFalse(other, msg=stem)
        masked = [f for f in run_lints([TD / "enum_hole.c"], TD)
                  if f.cls == "UNINIT-SWITCH"]
        self.assertFalse(masked)
        fall = [f for f in run_lints([TD / "enum_hole.c"], TD)
                if f.cls == "CTRL-FALLTHROUGH"]
        self.assertFalse(fall)

    def test_lock_missing_init(self):
        hits = [f for f in run_lints([TD / "lock_missing_init.c"], TD)
                if f.cls == "LOCK-MISSING-INIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lock_init_bad", names)
        self.assertIn("mtx_init_bad", names)
        self.assertNotIn("lock_init_ok", names)
        self.assertNotIn("lock_init_static_ok", names)
        self.assertNotIn("lock_init_param_ok", names)
        for stem in ("abs_ok", "lock_ok", "lock_imbalance", "double_lock"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "LOCK-MISSING-INIT"]
            self.assertFalse(other, msg=stem)

    def test_cxx_slicing(self):
        hits = [f for f in run_lints([TD / "slicing.cpp"], TD)
                if f.cls == "CXX-SLICING"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("slice_bad", names)
        self.assertNotIn("slice_ok", names)
        self.assertNotIn("slice_base_ok", names)
        self.assertNotIn("take_val", names)
        self.assertNotIn("take_ref", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CXX-SLICING"]
            self.assertFalse(other, msg=stem)
        for stem in ("virtual_ctor", "self_assign", "cxx_newdel"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SLICING"]
            self.assertFalse(other, msg=stem)

    def test_bool_as_bit(self):
        hits = [f for f in run_lints([TD / "bool_as_bit.c"], TD)
                if f.cls == "INT-BOOL-AS-BIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bool_bit_bad", names)
        self.assertIn("bool_bit_or_bad", names)
        self.assertNotIn("bool_bit_ok", names)
        self.assertNotIn("bool_bit_mask_ok", names)
        taut = [f for f in run_lints([TD / "taut_bound.c"], TD)
                if f.cls == "INT-BOOL-AS-BIT"]
        self.assertFalse(taut)
        masked = [f for f in run_lints([TD / "masked_switch.c"], TD)
                  if f.cls == "INT-BOOL-AS-BIT"]
        self.assertFalse(masked)
        cxx = [f for f in run_lints([TD / "self_assign.cpp"], TD)
               if f.cls == "INT-BOOL-AS-BIT"]
        self.assertFalse(cxx)
        for stem in ("abs_ok", "sign_compare"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "INT-BOOL-AS-BIT"]
            self.assertFalse(other, msg=stem)

    def test_strtok_reentrant(self):
        hits = [f for f in run_lints([TD / "strtok_reentrant.c"], TD)
                if f.cls == "API-STRTOK-REENTRANT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strtok_bad", names)
        self.assertNotIn("strtok_ok", names)
        self.assertNotIn("strtok_s_ok", names)
        gets = [f for f in run_lints([TD / "strtok_reentrant.c"], TD)
                if f.cls == "API-GETS"]
        self.assertFalse(gets)
        for stem in ("abs_ok", "gets"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STRTOK-REENTRANT"]
            self.assertFalse(other, msg=stem)

    def test_empty_infinite(self):
        hits = [f for f in run_lints([TD / "empty_infinite.c"], TD)
                if f.cls == "CTRL-EMPTY-INFINITE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("empty_inf_bad", names)
        self.assertIn("empty_inf_while_bad", names)
        self.assertNotIn("empty_inf_ok", names)
        self.assertNotIn("empty_inf_for_ok", names)
        for stem in ("abs_ok", "decreases_loop", "kinduct", "loop_overflow"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CTRL-EMPTY-INFINITE"]
            self.assertFalse(other, msg=stem)

    def test_mem_flex_array(self):
        hits = [f for f in run_lints([TD / "flex_array.c"], TD)
                if f.cls == "MEM-FLEX-ARRAY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flex_bad", names)
        self.assertIn("flex_star_bad", names)
        self.assertNotIn("flex_ok", names)
        self.assertNotIn("flex_plain_ok", names)
        wrap = [f for f in run_lints([TD / "malloc_wrap.c"], TD)
                if f.cls == "MEM-FLEX-ARRAY"]
        self.assertFalse(wrap)
        for stem in ("abs_ok", "capacity", "infoleak_pad"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "MEM-FLEX-ARRAY"]
            self.assertFalse(other, msg=stem)

    def test_str_missing_nul(self):
        hits = [f for f in run_lints([TD / "missing_nul.c"], TD)
                if f.cls == "STR-MISSING-NUL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("missing_nul_bad", names)
        self.assertNotIn("missing_nul_ok", names)
        self.assertNotIn("missing_nul_sizeof_ok", names)
        offby = [f for f in run_lints([TD / "missing_nul.c"], TD)
                 if f.cls == "STR-OFF-BY-ONE"]
        self.assertFalse(offby)
        for stem in ("abs_ok", "overlap", "off_by_one", "infoleak_pad"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "STR-MISSING-NUL"]
            self.assertFalse(other, msg=stem)

    def test_cxx_delete_this(self):
        hits = [f for f in run_lints([TD / "delete_this.cpp"], TD)
                if f.cls == "CXX-DELETE-THIS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("delete_this_bad", names)
        self.assertNotIn("delete_this_ok", names)
        self.assertNotIn("delete_this_array_ok", names)
        for stem in ("cxx_newdel", "self_assign", "throw_dtor"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-DELETE-THIS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DELETE-THIS"]
        self.assertFalse(c)

    def test_cxx_catch_by_value(self):
        hits = [f for f in run_lints([TD / "catch_value.cpp"], TD)
                if f.cls == "CXX-CATCH-BY-VALUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("catch_val_bad", names)
        self.assertNotIn("catch_val_ok", names)
        self.assertNotIn("catch_ptr_ok", names)
        for stem in ("try_catch", "exception_leak"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-CATCH-BY-VALUE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CATCH-BY-VALUE"]
        self.assertFalse(c)

    def test_cxx_throw_noexcept(self):
        hits = [f for f in run_lints([TD / "throw_noexcept.cpp"], TD)
                if f.cls == "CXX-THROW-NOEXCEPT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("throws_noexcept_bad", names)
        self.assertNotIn("throws_noexcept_ok", names)
        self.assertNotIn("noexcept_ok", names)
        spec = [f for f in run_lints([TD / "throw_spec.cpp"], TD)
                if f.cls == "CXX-THROW-NOEXCEPT"]
        self.assertFalse(spec)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-THROW-NOEXCEPT"]
        self.assertFalse(c)

    def test_cxx_missing_virtual_dtor(self):
        hits = [f for f in run_lints([TD / "virtual_dtor.cpp"], TD)
                if f.cls == "CXX-MISSING-VIRTUAL-DTOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("virtual_dtor_bad", names)
        self.assertNotIn("virtual_dtor_ok", names)
        for stem in ("virtual_ctor", "cxx_newdel"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MISSING-VIRTUAL-DTOR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MISSING-VIRTUAL-DTOR"]
        self.assertFalse(c)

    def test_api_mkstemp(self):
        hits = [f for f in run_lints([TD / "mkstemp.c"], TD)
                if f.cls == "API-MKSTEMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mkstemp_bad", names)
        self.assertIn("mkstemps_bad", names)
        self.assertNotIn("mkstemp_ok", names)
        self.assertNotIn("mkdtemp_ok", names)
        ignored = [f for f in run_lints([TD / "mkstemp.c"], TD)
                   if f.cls == "API-IGNORED-ERROR"]
        self.assertFalse(ignored)
        for stem in ("abs_ok", "gets", "ignored_error"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKSTEMP"]
            self.assertFalse(other, msg=stem)

    def test_api_tmpnam(self):
        hits = [f for f in run_lints([TD / "tmpnam.c"], TD)
                if f.cls == "API-TMPNAM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tmpnam_bad", names)
        self.assertIn("tempnam_bad", names)
        self.assertNotIn("tmpnam_ok", names)
        mkstemp = [f for f in run_lints([TD / "tmpnam.c"], TD)
                   if f.cls == "API-MKSTEMP"]
        self.assertFalse(mkstemp)
        for stem in ("abs_ok", "unsigned", "gets", "mkstemp"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TMPNAM"]
            self.assertFalse(other, msg=stem)

    def test_api_mktemp(self):
        hits = [f for f in run_lints([TD / "mktemp.c"], TD)
                if f.cls == "API-MKTEMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mktemp_bad", names)
        self.assertNotIn("mktemp_ok", names)
        for stem in ("abs_ok", "mkstemp", "tmpnam"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKTEMP"]
            self.assertFalse(other, msg=stem)

    def test_api_signal(self):
        hits = [f for f in run_lints([TD / "signal_api.c"], TD)
                if f.cls == "API-SIGNAL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("signal_bad", names)
        self.assertNotIn("signal_ok", names)
        self.assertNotIn("signal_dfl_ok", names)
        for stem in ("abs_ok", "longjmp"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGNAL"]
            self.assertFalse(other, msg=stem)

    def test_fmt_percent_n(self):
        hits = [f for f in run_lints([TD / "percent_n.c"], TD)
                if f.cls == "FMT-PERCENT-N"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("percent_n_bad", names)
        self.assertNotIn("percent_n_ok", names)
        self.assertNotIn("percent_n_escaped_ok", names)
        self.assertNotIn("percent_n_snprintf_ok", names)
        fmt = [f for f in run_lints([TD / "percent_n.c"], TD)
               if f.cls == "FMT-STRING"]
        self.assertFalse(fmt)
        for stem in ("abs_ok", "unsigned", "format"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "FMT-PERCENT-N"]
            self.assertFalse(other, msg=stem)

    def test_api_system(self):
        hits = [f for f in run_lints([TD / "system_call.c"], TD)
                if f.cls == "API-SYSTEM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("system_bad", names)
        self.assertNotIn("system_ok", names)
        for stem in ("abs_ok", "unsigned", "gets"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSTEM"]
            self.assertFalse(other, msg=stem)
        ok = [f for f in run_lints([TD / "taint_sink.c"], TD)
              if f.cls == "API-SYSTEM" and f.function == "ok"]
        self.assertFalse(ok)

    def test_api_chroot(self):
        hits = [f for f in run_lints([TD / "chroot.c"], TD)
                if f.cls == "API-CHROOT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chroot_bad", names)
        self.assertNotIn("chroot_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CHROOT"]
            self.assertFalse(other, msg=stem)

    def test_int_atoi(self):
        hits = [f for f in run_lints([TD / "atoi_index.c"], TD)
                if f.cls == "INT-ATOI"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("atoi_bad", names)
        self.assertNotIn("atoi_ok", names)
        self.assertNotIn("atoi_return_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "INT-ATOI"]
            self.assertFalse(other, msg=stem)

    def test_str_sprintf(self):
        hits = [f for f in run_lints([TD / "sprintf_buf.c"], TD)
                if f.cls == "STR-SPRINTF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sprintf_bad", names)
        self.assertNotIn("sprintf_ok", names)
        unbounded = [f for f in run_lints([TD / "sprintf_buf.c"], TD)
                     if f.cls == "STR-UNBOUNDED-COPY"]
        self.assertFalse(unbounded)
        for stem in ("abs_ok", "unbounded_copy"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "STR-SPRINTF"]
            self.assertFalse(other, msg=stem)

    def test_getenv_null(self):
        hits = [f for f in run_lints([TD / "getenv_null.c"], TD)
                if f.cls == "API-GETENV-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getenv_bad", names)
        self.assertNotIn("getenv_ok", names)
        for stem in ("abs_ok", "taint_sink", "system_call"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETENV-NULL"]
            self.assertFalse(other, msg=stem)

    def test_strdup_null(self):
        hits = [f for f in run_lints([TD / "strdup_null.c"], TD)
                if f.cls == "API-STRDUP-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strdup_bad", names)
        self.assertNotIn("strdup_ok", names)
        for stem in ("abs_ok", "getenv_null", "unchecked_alloc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STRDUP-NULL"]
            self.assertFalse(other, msg=stem)

    def test_sizeof_ptr(self):
        hits = [f for f in run_lints([TD / "sizeof_ptr.c"], TD)
                if f.cls == "MEM-SIZEOF-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sizeof_ptr_bad", names)
        self.assertNotIn("sizeof_ptr_ok", names)
        self.assertNotIn("sizeof_ptr_type_ok", names)
        for stem in ("abs_ok", "unchecked_alloc", "flex_array"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "MEM-SIZEOF-PTR"]
            self.assertFalse(other, msg=stem)

    def test_popen(self):
        hits = [f for f in run_lints([TD / "popen.c"], TD)
                if f.cls == "API-POPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("popen_bad", names)
        self.assertNotIn("popen_ok", names)
        for stem in ("fd_leak", "abs_ok"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-POPEN"]
            self.assertFalse(other, msg=stem)

    def test_umask(self):
        hits = [f for f in run_lints([TD / "umask.c"], TD)
                if f.cls == "API-UMASK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("umask_bad", names)
        self.assertNotIn("umask_ok", names)
        for stem in ("abs_ok", "ignored_error"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UMASK"]
            self.assertFalse(other, msg=stem)

    def test_api_fork(self):
        hits = [f for f in run_lints([TD / "fork_api.c"], TD)
                if f.cls == "API-FORK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fork_wait_bad", names)
        self.assertIn("vfork_bad", names)
        self.assertNotIn("fork_wait_ok", names)
        for stem in ("abs_ok", "signal_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FORK"]
            self.assertFalse(other, msg=stem)

    def test_api_exec(self):
        hits = [f for f in run_lints([TD / "exec_api.c"], TD)
                if f.cls == "API-EXEC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("exec_bad", names)
        self.assertNotIn("exec_ok", names)
        for stem in ("abs_ok", "system_call"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EXEC"]
            self.assertFalse(other, msg=stem)

    def test_api_mmap(self):
        hits = [f for f in run_lints([TD / "mmap_check.c"], TD)
                if f.cls == "API-MMAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mmap_check_bad", names)
        self.assertNotIn("mmap_check_ok", names)
        for stem in ("abs_ok", "unchecked_alloc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MMAP"]
            self.assertFalse(other, msg=stem)

    def test_str_wcscpy(self):
        hits = [f for f in run_lints([TD / "wcs_unbounded.c"], TD)
                if f.cls == "STR-WCSCPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wcs_copy_bad", names)
        self.assertNotIn("wcs_copy_ok", names)
        unbounded = [f for f in run_lints([TD / "wcs_unbounded.c"], TD)
                     if f.cls == "STR-UNBOUNDED-COPY"]
        self.assertFalse(unbounded)
        for stem in ("abs_ok", "unbounded_copy"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "STR-WCSCPY"]
            self.assertFalse(other, msg=stem)

    def test_api_getcwd(self):
        hits = [f for f in run_lints([TD / "getcwd_null.c"], TD)
                if f.cls == "API-GETCWD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getcwd_bad", names)
        self.assertNotIn("getcwd_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETCWD"]
            self.assertFalse(other, msg=stem)

    def test_api_ioctl(self):
        hits = [f for f in run_lints([TD / "ioctl_api.c"], TD)
                if f.cls == "API-IOCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ioctl_bad", names)
        self.assertNotIn("ioctl_ok", names)
        for stem in ("abs_ok", "scanf_unchecked"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-IOCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_dlopen(self):
        hits = [f for f in run_lints([TD / "dlopen_null.c"], TD)
                if f.cls == "API-DLOPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dlopen_null_bad", names)
        self.assertNotIn("dlopen_null_ok", names)
        for stem in ("abs_ok", "getcwd_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-DLOPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_accept(self):
        hits = [f for f in run_lints([TD / "accept_api.c"], TD)
                if f.cls == "API-ACCEPT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("accept_bad", names)
        self.assertNotIn("accept_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ACCEPT"]
            self.assertFalse(other, msg=stem)

    def test_api_realpath(self):
        hits = [f for f in run_lints([TD / "realpath_null.c"], TD)
                if f.cls == "API-REALPATH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("realpath_bad", names)
        self.assertNotIn("realpath_ok", names)
        for stem in ("abs_ok", "getcwd_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REALPATH"]
            self.assertFalse(other, msg=stem)

    def test_api_chmod_world(self):
        hits = [f for f in run_lints([TD / "chmod_world.c"], TD)
                if f.cls == "API-CHMOD-WORLD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chmod_world_bad", names)
        self.assertNotIn("chmod_ok", names)
        for stem in ("abs_ok", "umask"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CHMOD-WORLD"]
            self.assertFalse(other, msg=stem)

    def test_int_clz_zero(self):
        hits = [f for f in run_lints([TD / "clz_zero.c"], TD)
                if f.cls == "INT-CLZ-ZERO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("clz_zero_bad", names)
        self.assertNotIn("clz_zero_ok", names)
        for stem in ("abs_ok", "builtin_vacuous"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "INT-CLZ-ZERO"]
            self.assertFalse(other, msg=stem)

    def test_mem_bcopy(self):
        hits = [f for f in run_lints([TD / "bcopy_overlap.c"], TD)
                if f.cls == "MEM-BCOPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bcopy_bad", names)
        self.assertNotIn("bcopy_ok", names)
        for stem in ("abs_ok", "overlap"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "MEM-BCOPY"]
            self.assertFalse(other, msg=stem)

    def test_api_setuid(self):
        hits = [f for f in run_lints([TD / "setuid_api.c"], TD)
                if f.cls == "API-SETUID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setuid_bad", names)
        self.assertNotIn("setuid_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETUID"]
            self.assertFalse(other, msg=stem)

    def test_api_socket(self):
        hits = [f for f in run_lints([TD / "socket_api.c"], TD)
                if f.cls == "API-SOCKET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("socket_bad", names)
        self.assertNotIn("socket_ok", names)
        for stem in ("abs_ok", "accept_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SOCKET"]
            self.assertFalse(other, msg=stem)

    def test_api_bind(self):
        hits = [f for f in run_lints([TD / "bind_api.c"], TD)
                if f.cls == "API-BIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bind_bad", names)
        self.assertNotIn("bind_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-BIND"]
            self.assertFalse(other, msg=stem)

    def test_str_snprintf(self):
        hits = [f for f in run_lints([TD / "snprintf_buf.c"], TD)
                if f.cls == "STR-SNPRINTF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("snprintf_bad", names)
        self.assertNotIn("snprintf_ok", names)
        sprintf = [f for f in run_lints([TD / "sprintf_buf.c"], TD)
                   if f.cls == "STR-SNPRINTF"]
        self.assertFalse(sprintf)
        for stem in ("abs_ok", "sprintf_buf"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "STR-SNPRINTF"]
            self.assertFalse(other, msg=stem)

    def test_api_unlink(self):
        hits = [f for f in run_lints([TD / "unlink_api.c"], TD)
                if f.cls == "API-UNLINK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unlink_bad", names)
        self.assertNotIn("unlink_ok", names)
        ignored = [f for f in run_lints([TD / "unlink_api.c"], TD)
                   if f.cls == "API-IGNORED-ERROR"]
        self.assertFalse(ignored)
        for stem in ("abs_ok", "ignored_error"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UNLINK"]
            self.assertFalse(other, msg=stem)

    def test_api_mkfifo(self):
        hits = [f for f in run_lints([TD / "mkfifo_api.c"], TD)
                if f.cls == "API-MKFIFO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mkfifo_bad", names)
        self.assertNotIn("mkfifo_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKFIFO"]
            self.assertFalse(other, msg=stem)

    def test_api_listen(self):
        hits = [f for f in run_lints([TD / "listen_api.c"], TD)
                if f.cls == "API-LISTEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("listen_bad", names)
        self.assertNotIn("listen_ok", names)
        for stem in ("abs_ok", "bind_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LISTEN"]
            self.assertFalse(other, msg=stem)

    def test_api_connect(self):
        hits = [f for f in run_lints([TD / "connect_api.c"], TD)
                if f.cls == "API-CONNECT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("connect_bad", names)
        self.assertNotIn("connect_ok", names)
        for stem in ("abs_ok", "bind_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CONNECT"]
            self.assertFalse(other, msg=stem)

    def test_api_pipe(self):
        hits = [f for f in run_lints([TD / "pipe_api.c"], TD)
                if f.cls == "API-PIPE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pipe_bad", names)
        self.assertNotIn("pipe_ok", names)
        for stem in ("abs_ok", "socket_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PIPE"]
            self.assertFalse(other, msg=stem)

    def test_api_dup(self):
        hits = [f for f in run_lints([TD / "dup_api.c"], TD)
                if f.cls == "API-DUP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dup_bad", names)
        self.assertNotIn("dup_ok", names)
        for stem in ("abs_ok", "socket_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-DUP"]
            self.assertFalse(other, msg=stem)

    def test_api_fcntl(self):
        hits = [f for f in run_lints([TD / "fcntl_api.c"], TD)
                if f.cls == "API-FCNTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fcntl_bad", names)
        self.assertNotIn("fcntl_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FCNTL"]
            self.assertFalse(other, msg=stem)

    def test_api_wait(self):
        hits = [f for f in run_lints([TD / "wait_api.c"], TD)
                if f.cls == "API-WAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("waitpid_bad", names)
        self.assertNotIn("waitpid_ok", names)
        for stem in ("abs_ok", "fork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-WAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_select(self):
        hits = [f for f in run_lints([TD / "select_api.c"], TD)
                if f.cls == "API-SELECT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("select_bad", names)
        self.assertNotIn("select_ok", names)
        for stem in ("abs_ok", "wait_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SELECT"]
            self.assertFalse(other, msg=stem)

    def test_api_send(self):
        hits = [f for f in run_lints([TD / "send_api.c"], TD)
                if f.cls == "API-SEND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("send_bad", names)
        self.assertNotIn("send_ok", names)
        for stem in ("abs_ok", "connect_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEND"]
            self.assertFalse(other, msg=stem)

    def test_api_shutdown(self):
        hits = [f for f in run_lints([TD / "shutdown_api.c"], TD)
                if f.cls == "API-SHUTDOWN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shutdown_bad", names)
        self.assertNotIn("shutdown_ok", names)
        for stem in ("abs_ok", "listen_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SHUTDOWN"]
            self.assertFalse(other, msg=stem)

    def test_api_kill(self):
        hits = [f for f in run_lints([TD / "kill_api.c"], TD)
                if f.cls == "API-KILL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kill_bad", names)
        self.assertNotIn("kill_ok", names)
        for stem in ("abs_ok", "signal_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KILL"]
            self.assertFalse(other, msg=stem)

    def test_api_getaddrinfo(self):
        hits = [f for f in run_lints([TD / "addrinfo_api.c"], TD)
                if f.cls == "API-GETADDRINFO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getaddrinfo_bad", names)
        self.assertNotIn("getaddrinfo_ok", names)
        for stem in ("abs_ok", "socket_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETADDRINFO"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_join(self):
        hits = [f for f in run_lints([TD / "pthread_join_api.c"], TD)
                if f.cls == "API-PTHREAD-JOIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("join_bad", names)
        self.assertNotIn("join_ok", names)
        for stem in ("abs_ok", "wait_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-JOIN"]
            self.assertFalse(other, msg=stem)

    def test_api_thrd_join(self):
        hits = [f for f in run_lints([TD / "thrd_join_api.c"], TD)
                if f.cls == "API-THRD-JOIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("thrd_join_bad", names)
        self.assertIn("thrd_detach_bad", names)
        self.assertNotIn("thrd_join_ok", names)
        self.assertEqual(hits[0].strength, laws.STRENGTH_FINDS)
        self.assertFalse(laws.is_proof(hits[0].status))
        for h in hits:
            self.assertIn("thrd", h.message.lower())
            self.assertNotIn("pthread", h.message.lower())
        unenc = [f for f in run_lints([TD / "thrd_unenc.c"], TD)
                 if f.cls == "API-THRD-JOIN"]
        self.assertTrue(unenc)
        self.assertTrue(all(f.status == laws.FAILED for f in unenc))
        self.assertTrue(all(not laws.is_proof(f.status) for f in unenc))
        unenc_names = {f.function for f in unenc}
        self.assertIn("thrd_join_unenc_bad", unenc_names)
        self.assertIn("thrd_detach_unenc_bad", unenc_names)
        for stem in (
            "abs_ok", "pthread_join_api", "thr_api",
            "iso_thread_race",
        ):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-THRD-JOIN"]
            self.assertFalse(other, msg=stem)

    def test_api_sem_wait(self):
        hits = [f for f in run_lints([TD / "sem_api.c"], TD)
                if f.cls == "API-SEM-WAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sem_bad", names)
        self.assertNotIn("sem_ok", names)
        for stem in ("abs_ok", "wait_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEM-WAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_openat(self):
        hits = [f for f in run_lints([TD / "openat_api.c"], TD)
                if f.cls == "API-OPENAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("openat_bad", names)
        self.assertNotIn("openat_ok", names)
        for stem in ("abs_ok", "dup_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-OPENAT"]
            self.assertFalse(other, msg=stem)

    def test_api_flock(self):
        hits = [f for f in run_lints([TD / "flock_api.c"], TD)
                if f.cls == "API-FLOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flock_bad", names)
        self.assertNotIn("flock_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FLOCK"]
            self.assertFalse(other, msg=stem)

    def test_api_chown(self):
        hits = [f for f in run_lints([TD / "chown_api.c"], TD)
                if f.cls == "API-CHOWN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chown_bad", names)
        self.assertNotIn("chown_ok", names)
        for stem in ("abs_ok", "chmod_world"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CHOWN"]
            self.assertFalse(other, msg=stem)

    def test_api_symlink(self):
        hits = [f for f in run_lints([TD / "symlink_api.c"], TD)
                if f.cls == "API-SYMLINK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("symlink_bad", names)
        self.assertNotIn("symlink_ok", names)
        for stem in ("abs_ok", "unlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYMLINK"]
            self.assertFalse(other, msg=stem)

    def test_api_opendir(self):
        hits = [f for f in run_lints([TD / "opendir_api.c"], TD)
                if f.cls == "API-OPENDIR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("opendir_bad", names)
        self.assertNotIn("opendir_ok", names)
        for stem in ("abs_ok", "dlopen_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-OPENDIR"]
            self.assertFalse(other, msg=stem)

    def test_api_setrlimit(self):
        hits = [f for f in run_lints([TD / "setrlimit_api.c"], TD)
                if f.cls == "API-SETRLIMIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setrlimit_bad", names)
        self.assertNotIn("setrlimit_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETRLIMIT"]
            self.assertFalse(other, msg=stem)

    def test_api_getsockopt(self):
        hits = [f for f in run_lints([TD / "getsockopt_api.c"], TD)
                if f.cls == "API-GETSOCKOPT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getsockopt_bad", names)
        self.assertNotIn("getsockopt_ok", names)
        for stem in ("abs_ok", "shutdown_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETSOCKOPT"]
            self.assertFalse(other, msg=stem)

    def test_api_stat(self):
        hits = [f for f in run_lints([TD / "stat_api.c"], TD)
                if f.cls == "API-STAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stat_bad", names)
        self.assertNotIn("stat_ok", names)
        for stem in ("abs_ok", "unlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STAT"]
            self.assertFalse(other, msg=stem)

    def test_api_mkdir(self):
        hits = [f for f in run_lints([TD / "mkdir_api.c"], TD)
                if f.cls == "API-MKDIR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mkdir_bad", names)
        self.assertNotIn("mkdir_ok", names)
        for stem in ("abs_ok", "mkfifo_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKDIR"]
            self.assertFalse(other, msg=stem)

    def test_api_getpwuid(self):
        hits = [f for f in run_lints([TD / "getpwuid_api.c"], TD)
                if f.cls == "API-GETPWUID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getpwuid_bad", names)
        self.assertNotIn("getpwuid_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPWUID"]
            self.assertFalse(other, msg=stem)

    def test_api_clock_gettime(self):
        hits = [f for f in run_lints([TD / "clock_api.c"], TD)
                if f.cls == "API-CLOCK-GETTIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("clock_bad", names)
        self.assertNotIn("clock_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLOCK-GETTIME"]
            self.assertFalse(other, msg=stem)

    def test_api_shm_open(self):
        hits = [f for f in run_lints([TD / "shm_api.c"], TD)
                if f.cls == "API-SHM-OPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shm_bad", names)
        self.assertNotIn("shm_ok", names)
        for stem in ("abs_ok", "openat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SHM-OPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_posix_spawn(self):
        hits = [f for f in run_lints([TD / "spawn_api.c"], TD)
                if f.cls == "API-POSIX-SPAWN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("spawn_bad", names)
        self.assertNotIn("spawn_ok", names)
        for stem in ("abs_ok", "fork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-POSIX-SPAWN"]
            self.assertFalse(other, msg=stem)

    def test_api_glob(self):
        hits = [f for f in run_lints([TD / "glob_api.c"], TD)
                if f.cls == "API-GLOB"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("glob_bad", names)
        self.assertNotIn("glob_ok", names)
        for stem in ("abs_ok", "unlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GLOB"]
            self.assertFalse(other, msg=stem)

    def test_api_fseek(self):
        hits = [f for f in run_lints([TD / "fseek_api.c"], TD)
                if f.cls == "API-FSEEK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fseek_bad", names)
        self.assertNotIn("fseek_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FSEEK"]
            self.assertFalse(other, msg=stem)

    def test_api_access(self):
        hits = [f for f in run_lints([TD / "access_api.c"], TD)
                if f.cls == "API-ACCESS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("access_bad", names)
        self.assertNotIn("access_ok", names)
        toctou = [f for f in run_lints([TD / "access_api.c"], TD)
                  if f.cls == "CONC-TOCTOU"]
        self.assertFalse(toctou)
        for stem in ("abs_ok", "toctou"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ACCESS"]
            self.assertFalse(other, msg=stem)

    def test_api_getopt(self):
        hits = [f for f in run_lints([TD / "getopt_api.c"], TD)
                if f.cls == "API-GETOPT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getopt_bad", names)
        self.assertNotIn("getopt_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETOPT"]
            self.assertFalse(other, msg=stem)

    def test_api_uname(self):
        hits = [f for f in run_lints([TD / "uname_api.c"], TD)
                if f.cls == "API-UNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uname_bad", names)
        self.assertNotIn("uname_ok", names)
        for stem in ("abs_ok", "access_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_sendfile(self):
        hits = [f for f in run_lints([TD / "sendfile_api.c"], TD)
                if f.cls == "API-SENDFILE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sendfile_bad", names)
        self.assertNotIn("sendfile_ok", names)
        for stem in ("abs_ok", "send_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SENDFILE"]
            self.assertFalse(other, msg=stem)

    def test_api_memfd(self):
        hits = [f for f in run_lints([TD / "memfd_api.c"], TD)
                if f.cls == "API-MEMFD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("memfd_bad", names)
        self.assertNotIn("memfd_ok", names)
        for stem in ("abs_ok", "shm_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MEMFD"]
            self.assertFalse(other, msg=stem)

    def test_api_prctl(self):
        hits = [f for f in run_lints([TD / "prctl_api.c"], TD)
                if f.cls == "API-PRCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("prctl_bad", names)
        self.assertNotIn("prctl_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PRCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_tcgetattr(self):
        hits = [f for f in run_lints([TD / "tcgetattr_api.c"], TD)
                if f.cls == "API-TCGETATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tcgetattr_bad", names)
        self.assertNotIn("tcgetattr_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TCGETATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_sysconf(self):
        hits = [f for f in run_lints([TD / "sysconf_api.c"], TD)
                if f.cls == "API-SYSCONF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sysconf_bad", names)
        self.assertNotIn("sysconf_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSCONF"]
            self.assertFalse(other, msg=stem)

    def test_api_getrusage(self):
        hits = [f for f in run_lints([TD / "getrusage_api.c"], TD)
                if f.cls == "API-GETRUSAGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getrusage_bad", names)
        self.assertNotIn("getrusage_ok", names)
        for stem in ("abs_ok", "setrlimit_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETRUSAGE"]
            self.assertFalse(other, msg=stem)

    def test_api_nftw(self):
        hits = [f for f in run_lints([TD / "nftw_api.c"], TD)
                if f.cls == "API-NFTW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nftw_bad", names)
        self.assertNotIn("nftw_ok", names)
        for stem in ("abs_ok", "glob_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NFTW"]
            self.assertFalse(other, msg=stem)

    def test_api_wordexp(self):
        hits = [f for f in run_lints([TD / "wordexp_api.c"], TD)
                if f.cls == "API-WORDEXP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wordexp_bad", names)
        self.assertNotIn("wordexp_ok", names)
        for stem in ("abs_ok", "glob_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-WORDEXP"]
            self.assertFalse(other, msg=stem)

    def test_api_getlogin(self):
        hits = [f for f in run_lints([TD / "getlogin_api.c"], TD)
                if f.cls == "API-GETLOGIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getlogin_bad", names)
        self.assertNotIn("getlogin_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETLOGIN"]
            self.assertFalse(other, msg=stem)

    def test_api_inet_pton(self):
        hits = [f for f in run_lints([TD / "inet_pton_api.c"], TD)
                if f.cls == "API-INET-PTON"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("inet_pton_bad", names)
        self.assertNotIn("inet_pton_ok", names)
        for stem in ("abs_ok", "socket_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-INET-PTON"]
            self.assertFalse(other, msg=stem)

    def test_api_mlock(self):
        hits = [f for f in run_lints([TD / "mlock_api.c"], TD)
                if f.cls == "API-MLOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mlock_bad", names)
        self.assertNotIn("mlock_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MLOCK"]
            self.assertFalse(other, msg=stem)

    def test_api_splice(self):
        hits = [f for f in run_lints([TD / "splice_api.c"], TD)
                if f.cls == "API-SPLICE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("splice_bad", names)
        self.assertNotIn("splice_ok", names)
        for stem in ("abs_ok", "sendfile_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SPLICE"]
            self.assertFalse(other, msg=stem)

    def test_api_inotify(self):
        hits = [f for f in run_lints([TD / "inotify_api.c"], TD)
                if f.cls == "API-INOTIFY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("inotify_bad", names)
        self.assertNotIn("inotify_ok", names)
        for stem in ("abs_ok", "memfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-INOTIFY"]
            self.assertFalse(other, msg=stem)

    def test_api_fsync(self):
        hits = [f for f in run_lints([TD / "fsync_api.c"], TD)
                if f.cls == "API-FSYNC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fsync_bad", names)
        self.assertNotIn("fsync_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FSYNC"]
            self.assertFalse(other, msg=stem)

    def test_api_getrandom(self):
        hits = [f for f in run_lints([TD / "getrandom_api.c"], TD)
                if f.cls == "API-GETRANDOM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getrandom_bad", names)
        self.assertNotIn("getrandom_ok", names)
        for stem in ("abs_ok", "ioctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETRANDOM"]
            self.assertFalse(other, msg=stem)

    def test_api_getline(self):
        hits = [f for f in run_lints([TD / "getline_api.c"], TD)
                if f.cls == "API-GETLINE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getline_bad", names)
        self.assertNotIn("getline_ok", names)
        for stem in ("abs_ok", "fseek_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETLINE"]
            self.assertFalse(other, msg=stem)

    def test_api_asprintf(self):
        hits = [f for f in run_lints([TD / "asprintf_api.c"], TD)
                if f.cls == "API-ASPRINTF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("asprintf_bad", names)
        self.assertNotIn("asprintf_ok", names)
        for stem in ("abs_ok", "getline_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ASPRINTF"]
            self.assertFalse(other, msg=stem)

    def test_api_strlcpy(self):
        hits = [f for f in run_lints([TD / "strlcpy_api.c"], TD)
                if f.cls == "API-STRLCPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strlcpy_bad", names)
        self.assertNotIn("strlcpy_ok", names)
        copy = [f for f in run_lints([TD / "strlcpy_api.c"], TD)
                if f.cls == "STR-UNBOUNDED-COPY"]
        self.assertFalse(copy)
        for stem in ("abs_ok", "unbounded_copy"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STRLCPY"]
            self.assertFalse(other, msg=stem)

    def test_api_isatty(self):
        hits = [f for f in run_lints([TD / "isatty_api.c"], TD)
                if f.cls == "API-ISATTY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("isatty_bad", names)
        self.assertNotIn("isatty_ok", names)
        for stem in ("abs_ok", "getlogin_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ISATTY"]
            self.assertFalse(other, msg=stem)

    def test_api_ptsname(self):
        hits = [f for f in run_lints([TD / "ptsname_api.c"], TD)
                if f.cls == "API-PTSNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ptsname_bad", names)
        self.assertNotIn("ptsname_ok", names)
        for stem in ("abs_ok", "getlogin_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTSNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_mount(self):
        hits = [f for f in run_lints([TD / "mount_api.c"], TD)
                if f.cls == "API-MOUNT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mount_bad", names)
        self.assertNotIn("mount_ok", names)
        for stem in ("abs_ok", "mkdir_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MOUNT"]
            self.assertFalse(other, msg=stem)

    def test_api_fmemopen(self):
        hits = [f for f in run_lints([TD / "fmemopen_api.c"], TD)
                if f.cls == "API-FMEMOPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fmemopen_bad", names)
        self.assertNotIn("fmemopen_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FMEMOPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_scandir(self):
        hits = [f for f in run_lints([TD / "scandir_api.c"], TD)
                if f.cls == "API-SCANDIR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("scandir_bad", names)
        self.assertNotIn("scandir_ok", names)
        for stem in ("abs_ok", "glob_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCANDIR"]
            self.assertFalse(other, msg=stem)

    def test_api_setxattr(self):
        hits = [f for f in run_lints([TD / "setxattr_api.c"], TD)
                if f.cls == "API-SETXATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setxattr_bad", names)
        self.assertNotIn("setxattr_ok", names)
        for stem in ("abs_ok", "stat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETXATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_sched_affinity(self):
        hits = [f for f in run_lints([TD / "sched_api.c"], TD)
                if f.cls == "API-SCHED-AFFINITY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sched_bad", names)
        self.assertNotIn("sched_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCHED-AFFINITY"]
            self.assertFalse(other, msg=stem)

    def test_api_aio(self):
        hits = [f for f in run_lints([TD / "aio_api.c"], TD)
                if f.cls == "API-AIO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("aio_bad", names)
        self.assertNotIn("aio_ok", names)
        for stem in ("abs_ok", "fsync_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-AIO"]
            self.assertFalse(other, msg=stem)

    def test_api_statx(self):
        hits = [f for f in run_lints([TD / "statx_api.c"], TD)
                if f.cls == "API-STATX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("statx_bad", names)
        self.assertNotIn("statx_ok", names)
        for stem in ("abs_ok", "stat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STATX"]
            self.assertFalse(other, msg=stem)

    def test_api_pidfd(self):
        hits = [f for f in run_lints([TD / "pidfd_api.c"], TD)
                if f.cls == "API-PIDFD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pidfd_bad", names)
        self.assertNotIn("pidfd_ok", names)
        for stem in ("abs_ok", "memfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PIDFD"]
            self.assertFalse(other, msg=stem)

    def test_api_capset(self):
        hits = [f for f in run_lints([TD / "capset_api.c"], TD)
                if f.cls == "API-CAPSET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capset_bad", names)
        self.assertNotIn("capset_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAPSET"]
            self.assertFalse(other, msg=stem)

    def test_api_fanotify(self):
        hits = [f for f in run_lints([TD / "fanotify_api.c"], TD)
                if f.cls == "API-FANOTIFY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fanotify_bad", names)
        self.assertNotIn("fanotify_ok", names)
        for stem in ("abs_ok", "inotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FANOTIFY"]
            self.assertFalse(other, msg=stem)

    def test_api_seccomp(self):
        hits = [f for f in run_lints([TD / "seccomp_api.c"], TD)
                if f.cls == "API-SECCOMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("seccomp_bad", names)
        self.assertNotIn("seccomp_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SECCOMP"]
            self.assertFalse(other, msg=stem)

    def test_api_getgrnam(self):
        hits = [f for f in run_lints([TD / "getgrnam_api.c"], TD)
                if f.cls == "API-GETGRNAM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getgrnam_bad", names)
        self.assertNotIn("getgrnam_ok", names)
        for stem in ("abs_ok", "getpwuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETGRNAM"]
            self.assertFalse(other, msg=stem)

    def test_api_fallocate(self):
        hits = [f for f in run_lints([TD / "fallocate_api.c"], TD)
                if f.cls == "API-FALLOCATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fallocate_bad", names)
        self.assertNotIn("fallocate_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FALLOCATE"]
            self.assertFalse(other, msg=stem)

    def test_api_close_range(self):
        hits = [f for f in run_lints([TD / "close_range_api.c"], TD)
                if f.cls == "API-CLOSE-RANGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("close_range_bad", names)
        self.assertNotIn("close_range_ok", names)
        for stem in ("abs_ok", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLOSE-RANGE"]
            self.assertFalse(other, msg=stem)

    def test_api_bpf(self):
        hits = [f for f in run_lints([TD / "bpf_api.c"], TD)
                if f.cls == "API-BPF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bpf_bad", names)
        self.assertNotIn("bpf_ok", names)
        for stem in ("abs_ok", "seccomp_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-BPF"]
            self.assertFalse(other, msg=stem)

    def test_api_userfaultfd(self):
        hits = [f for f in run_lints([TD / "userfaultfd_api.c"], TD)
                if f.cls == "API-USERFAULTFD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uffd_bad", names)
        self.assertNotIn("uffd_ok", names)
        for stem in ("abs_ok", "inotify_api", "fanotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-USERFAULTFD"]
            self.assertFalse(other, msg=stem)

    def test_api_getpass(self):
        hits = [f for f in run_lints([TD / "getpass_api.c"], TD)
                if f.cls == "API-GETPASS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getpass_bad", names)
        self.assertNotIn("getpass_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPASS"]
            self.assertFalse(other, msg=stem)

    def test_api_initgroups(self):
        hits = [f for f in run_lints([TD / "initgroups_api.c"], TD)
                if f.cls == "API-INITGROUPS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("initgroups_bad", names)
        self.assertNotIn("initgroups_ok", names)
        for stem in ("abs_ok", "setuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-INITGROUPS"]
            self.assertFalse(other, msg=stem)

    def test_api_clone(self):
        hits = [f for f in run_lints([TD / "clone_api.c"], TD)
                if f.cls == "API-CLONE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unshare_bad", names)
        self.assertNotIn("unshare_ok", names)
        for stem in ("abs_ok", "pthread_join_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLONE"]
            self.assertFalse(other, msg=stem)

    def test_api_openat2(self):
        hits = [f for f in run_lints([TD / "openat2_api.c"], TD)
                if f.cls == "API-OPENAT2"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("openat2_bad", names)
        self.assertNotIn("openat2_ok", names)
        for stem in ("abs_ok", "openat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-OPENAT2"]
            self.assertFalse(other, msg=stem)

    def test_api_landlock(self):
        hits = [f for f in run_lints([TD / "landlock_api.c"], TD)
                if f.cls == "API-LANDLOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("landlock_bad", names)
        self.assertNotIn("landlock_ok", names)
        for stem in ("abs_ok", "fanotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LANDLOCK"]
            self.assertFalse(other, msg=stem)

    def test_api_getpriority(self):
        hits = [f for f in run_lints([TD / "getpriority_api.c"], TD)
                if f.cls == "API-GETPRIORITY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getpriority_bad", names)
        self.assertNotIn("getpriority_ok", names)
        for stem in ("abs_ok", "setrlimit_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPRIORITY"]
            self.assertFalse(other, msg=stem)

    def test_api_signalfd(self):
        hits = [f for f in run_lints([TD / "signalfd_api.c"], TD)
                if f.cls == "API-SIGNALFD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("signalfd_bad", names)
        self.assertNotIn("signalfd_ok", names)
        for stem in ("abs_ok", "fanotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGNALFD"]
            self.assertFalse(other, msg=stem)

    def test_api_sendmmsg(self):
        hits = [f for f in run_lints([TD / "sendmmsg_api.c"], TD)
                if f.cls == "API-SENDMMSG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sendmmsg_bad", names)
        self.assertNotIn("sendmmsg_ok", names)
        for stem in ("abs_ok", "send_api", "send_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SENDMMSG"]
            self.assertFalse(other, msg=stem)

    def test_api_personality(self):
        hits = [f for f in run_lints([TD / "personality_api.c"], TD)
                if f.cls == "API-PERSONALITY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("personality_bad", names)
        self.assertNotIn("personality_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PERSONALITY"]
            self.assertFalse(other, msg=stem)

    def test_api_quotactl(self):
        hits = [f for f in run_lints([TD / "quotactl_api.c"], TD)
                if f.cls == "API-QUOTACTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("quotactl_bad", names)
        self.assertNotIn("quotactl_ok", names)
        for stem in ("abs_ok", "mount_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-QUOTACTL"]
            self.assertFalse(other, msg=stem)

    def test_api_name_to_handle(self):
        hits = [f for f in run_lints([TD / "name_to_handle_api.c"], TD)
                if f.cls == "API-NAME-TO-HANDLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nth_bad", names)
        self.assertNotIn("nth_ok", names)
        for stem in ("abs_ok", "openat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NAME-TO-HANDLE"]
            self.assertFalse(other, msg=stem)

    def test_api_process_madvise(self):
        hits = [f for f in run_lints([TD / "process_madvise_api.c"], TD)
                if f.cls == "API-PROCESS-MADVISE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pmadvise_bad", names)
        self.assertNotIn("pmadvise_ok", names)
        for stem in ("abs_ok", "mlock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PROCESS-MADVISE"]
            self.assertFalse(other, msg=stem)

    def test_api_pivot_root(self):
        hits = [f for f in run_lints([TD / "pivot_root_api.c"], TD)
                if f.cls == "API-PIVOT-ROOT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pivot_root_bad", names)
        self.assertNotIn("pivot_root_ok", names)
        for stem in ("abs_ok", "mount_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PIVOT-ROOT"]
            self.assertFalse(other, msg=stem)

    def test_api_statfs(self):
        hits = [f for f in run_lints([TD / "statfs_api.c"], TD)
                if f.cls == "API-STATFS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("statfs_bad", names)
        self.assertNotIn("statfs_ok", names)
        for stem in ("abs_ok", "stat_api", "statx_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STATFS"]
            self.assertFalse(other, msg=stem)

    def test_api_prlimit(self):
        hits = [f for f in run_lints([TD / "prlimit_api.c"], TD)
                if f.cls == "API-PRLIMIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("prlimit_bad", names)
        self.assertNotIn("prlimit_ok", names)
        for stem in ("abs_ok", "setrlimit_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PRLIMIT"]
            self.assertFalse(other, msg=stem)

    def test_api_perf_event(self):
        hits = [f for f in run_lints([TD / "perf_event_api.c"], TD)
                if f.cls == "API-PERF-EVENT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("perf_event_bad", names)
        self.assertNotIn("perf_event_ok", names)
        for stem in ("abs_ok", "fanotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PERF-EVENT"]
            self.assertFalse(other, msg=stem)

    def test_api_membarrier(self):
        hits = [f for f in run_lints([TD / "membarrier_api.c"], TD)
                if f.cls == "API-MEMBARRIER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("membarrier_bad", names)
        self.assertNotIn("membarrier_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MEMBARRIER"]
            self.assertFalse(other, msg=stem)

    def test_api_pkey(self):
        hits = [f for f in run_lints([TD / "pkey_api.c"], TD)
                if f.cls == "API-PKEY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pkey_bad", names)
        self.assertNotIn("pkey_ok", names)
        for stem in ("abs_ok", "fanotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PKEY"]
            self.assertFalse(other, msg=stem)

    def test_api_syncfs(self):
        hits = [f for f in run_lints([TD / "syncfs_api.c"], TD)
                if f.cls == "API-SYNCFS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("syncfs_bad", names)
        self.assertNotIn("syncfs_ok", names)
        for stem in ("abs_ok", "fsync_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYNCFS"]
            self.assertFalse(other, msg=stem)

    def test_api_process_vm(self):
        hits = [f for f in run_lints([TD / "process_vm_api.c"], TD)
                if f.cls == "API-PROCESS-VM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pvm_bad", names)
        self.assertNotIn("pvm_ok", names)
        for stem in ("abs_ok", "process_madvise_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PROCESS-VM"]
            self.assertFalse(other, msg=stem)

    def test_api_clone3(self):
        hits = [f for f in run_lints([TD / "clone3_api.c"], TD)
                if f.cls == "API-CLONE3"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("clone3_bad", names)
        self.assertNotIn("clone3_ok", names)
        for stem in ("abs_ok", "clone_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLONE3"]
            self.assertFalse(other, msg=stem)

    def test_api_futex(self):
        hits = [f for f in run_lints([TD / "futex_api.c"], TD)
                if f.cls == "API-FUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("futex_bad", names)
        self.assertNotIn("futex_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FUTEX"]
            self.assertFalse(other, msg=stem)

    def test_api_keyctl(self):
        hits = [f for f in run_lints([TD / "keyctl_api.c"], TD)
                if f.cls == "API-KEYCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("keyctl_bad", names)
        self.assertNotIn("keyctl_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KEYCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_kcmp(self):
        hits = [f for f in run_lints([TD / "kcmp_api.c"], TD)
                if f.cls == "API-KCMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kcmp_bad", names)
        self.assertNotIn("kcmp_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KCMP"]
            self.assertFalse(other, msg=stem)

    def test_api_fsopen(self):
        hits = [f for f in run_lints([TD / "fsopen_api.c"], TD)
                if f.cls == "API-FSOPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fsopen_bad", names)
        self.assertNotIn("fsopen_ok", names)
        for stem in ("abs_ok", "openat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FSOPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_mq_open(self):
        hits = [f for f in run_lints([TD / "mq_open_api.c"], TD)
                if f.cls == "API-MQ-OPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mq_open_bad", names)
        self.assertNotIn("mq_open_ok", names)
        for stem in ("abs_ok", "openat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MQ-OPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_shmget(self):
        hits = [f for f in run_lints([TD / "shmget_api.c"], TD)
                if f.cls == "API-SHMGET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shmget_bad", names)
        self.assertNotIn("shmget_ok", names)
        for stem in ("abs_ok", "shm_api", "shm_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SHMGET"]
            self.assertFalse(other, msg=stem)

    def test_api_reboot(self):
        hits = [f for f in run_lints([TD / "reboot_api.c"], TD)
                if f.cls == "API-REBOOT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reboot_bad", names)
        self.assertNotIn("reboot_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REBOOT"]
            self.assertFalse(other, msg=stem)

    def test_api_adjtimex(self):
        hits = [f for f in run_lints([TD / "adjtimex_api.c"], TD)
                if f.cls == "API-ADJTIMEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("adjtimex_bad", names)
        self.assertNotIn("adjtimex_ok", names)
        for stem in ("abs_ok", "clock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ADJTIMEX"]
            self.assertFalse(other, msg=stem)

    def test_api_sethostname(self):
        hits = [f for f in run_lints([TD / "sethostname_api.c"], TD)
                if f.cls == "API-SETHOSTNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sethostname_bad", names)
        self.assertNotIn("sethostname_ok", names)
        for stem in ("abs_ok", "uname_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETHOSTNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_swapon(self):
        hits = [f for f in run_lints([TD / "swapon_api.c"], TD)
                if f.cls == "API-SWAPON"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("swapon_bad", names)
        self.assertNotIn("swapon_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SWAPON"]
            self.assertFalse(other, msg=stem)

    def test_api_acct(self):
        hits = [f for f in run_lints([TD / "acct_api.c"], TD)
                if f.cls == "API-ACCT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("acct_bad", names)
        self.assertNotIn("acct_ok", names)
        for stem in ("abs_ok", "access_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ACCT"]
            self.assertFalse(other, msg=stem)

    def test_api_ioperm(self):
        hits = [f for f in run_lints([TD / "ioperm_api.c"], TD)
                if f.cls == "API-IOPERM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ioperm_bad", names)
        self.assertNotIn("ioperm_ok", names)
        for stem in ("abs_ok", "ioprio_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-IOPERM"]
            self.assertFalse(other, msg=stem)

    def test_api_mincore(self):
        hits = [f for f in run_lints([TD / "mincore_api.c"], TD)
                if f.cls == "API-MINCORE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mincore_bad", names)
        self.assertNotIn("mincore_ok", names)
        for stem in ("abs_ok", "mlock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MINCORE"]
            self.assertFalse(other, msg=stem)

    def test_api_rseq(self):
        hits = [f for f in run_lints([TD / "rseq_api.c"], TD)
                if f.cls == "API-RSEQ"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rseq_bad", names)
        self.assertNotIn("rseq_ok", names)
        for stem in ("abs_ok", "membarrier_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RSEQ"]
            self.assertFalse(other, msg=stem)

    def test_api_timer_create(self):
        hits = [f for f in run_lints([TD / "timer_create_api.c"], TD)
                if f.cls == "API-TIMER-CREATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("timer_create_bad", names)
        self.assertNotIn("timer_create_ok", names)
        for stem in ("abs_ok", "memfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TIMER-CREATE"]
            self.assertFalse(other, msg=stem)

    def test_api_semget(self):
        hits = [f for f in run_lints([TD / "semget_api.c"], TD)
                if f.cls == "API-SEMGET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("semget_bad", names)
        self.assertNotIn("semget_ok", names)
        for stem in ("abs_ok", "sem_api", "shmget_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEMGET"]
            self.assertFalse(other, msg=stem)

    def test_api_msgget(self):
        hits = [f for f in run_lints([TD / "msgget_api.c"], TD)
                if f.cls == "API-MSGGET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("msgget_bad", names)
        self.assertNotIn("msgget_ok", names)
        for stem in ("abs_ok", "mq_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MSGGET"]
            self.assertFalse(other, msg=stem)

    def test_api_klogctl(self):
        hits = [f for f in run_lints([TD / "klogctl_api.c"], TD)
                if f.cls == "API-KLOGCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("klogctl_bad", names)
        self.assertNotIn("klogctl_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KLOGCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_mount_setattr(self):
        hits = [f for f in run_lints([TD / "mount_setattr_api.c"], TD)
                if f.cls == "API-MOUNT-SETATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mntsa_bad", names)
        self.assertNotIn("mntsa_ok", names)
        for stem in ("abs_ok", "mount_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MOUNT-SETATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_getcpu(self):
        hits = [f for f in run_lints([TD / "getcpu_api.c"], TD)
                if f.cls == "API-GETCPU"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getcpu_bad", names)
        self.assertNotIn("getcpu_ok", names)
        for stem in ("abs_ok", "getrusage_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETCPU"]
            self.assertFalse(other, msg=stem)

    def test_api_process_mrelease(self):
        hits = [f for f in run_lints([TD / "process_mrelease_api.c"], TD)
                if f.cls == "API-PROCESS-MRELEASE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pmrel_bad", names)
        self.assertNotIn("pmrel_ok", names)
        for stem in ("abs_ok", "process_madvise_api", "process_vm_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PROCESS-MRELEASE"]
            self.assertFalse(other, msg=stem)

    def test_api_memfd_secret(self):
        hits = [f for f in run_lints([TD / "memfd_secret_api.c"], TD)
                if f.cls == "API-MEMFD-SECRET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mfds_bad", names)
        self.assertNotIn("mfds_ok", names)
        for stem in ("abs_ok", "memfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MEMFD-SECRET"]
            self.assertFalse(other, msg=stem)

    def test_api_ioprio(self):
        hits = [f for f in run_lints([TD / "ioprio_api.c"], TD)
                if f.cls == "API-IOPRIO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ioprio_bad", names)
        self.assertNotIn("ioprio_ok", names)
        for stem in ("abs_ok", "ioperm_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-IOPRIO"]
            self.assertFalse(other, msg=stem)

    def test_api_init_module(self):
        hits = [f for f in run_lints([TD / "init_module_api.c"], TD)
                if f.cls == "API-INIT-MODULE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("insmod_bad", names)
        self.assertNotIn("insmod_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-INIT-MODULE"]
            self.assertFalse(other, msg=stem)

    def test_api_kexec(self):
        hits = [f for f in run_lints([TD / "kexec_api.c"], TD)
                if f.cls == "API-KEXEC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kexec_bad", names)
        self.assertNotIn("kexec_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KEXEC"]
            self.assertFalse(other, msg=stem)

    def test_api_quotactl_fd(self):
        hits = [f for f in run_lints([TD / "quotactl_fd_api.c"], TD)
                if f.cls == "API-QUOTACTL-FD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("qcfd_bad", names)
        self.assertNotIn("qcfd_ok", names)
        for stem in ("abs_ok", "quotactl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-QUOTACTL-FD"]
            self.assertFalse(other, msg=stem)

    def test_api_pkey_free(self):
        hits = [f for f in run_lints([TD / "pkey_free_api.c"], TD)
                if f.cls == "API-PKEY-FREE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pkeyf_bad", names)
        self.assertNotIn("pkeyf_ok", names)
        for stem in ("abs_ok", "pkey_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PKEY-FREE"]
            self.assertFalse(other, msg=stem)

    def test_api_tgkill(self):
        hits = [f for f in run_lints([TD / "tgkill_api.c"], TD)
                if f.cls == "API-TGKILL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tgkill_bad", names)
        self.assertNotIn("tgkill_ok", names)
        for stem in ("abs_ok", "kill_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TGKILL"]
            self.assertFalse(other, msg=stem)

    def test_api_add_key(self):
        hits = [f for f in run_lints([TD / "add_key_api.c"], TD)
                if f.cls == "API-ADD-KEY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("addkey_bad", names)
        self.assertNotIn("addkey_ok", names)
        for stem in ("abs_ok", "keyctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ADD-KEY"]
            self.assertFalse(other, msg=stem)

    def test_api_semctl(self):
        hits = [f for f in run_lints([TD / "semctl_api.c"], TD)
                if f.cls == "API-SEMCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("semctl_bad", names)
        self.assertNotIn("semctl_ok", names)
        for stem in ("abs_ok", "semget_api", "sem_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEMCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_msgctl(self):
        hits = [f for f in run_lints([TD / "msgctl_api.c"], TD)
                if f.cls == "API-MSGCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("msgctl_bad", names)
        self.assertNotIn("msgctl_ok", names)
        for stem in ("abs_ok", "msgget_api", "mq_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MSGCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_io_setup(self):
        hits = [f for f in run_lints([TD / "io_setup_api.c"], TD)
                if f.cls == "API-IO-SETUP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("io_setup_bad", names)
        self.assertNotIn("io_setup_ok", names)
        for stem in ("abs_ok", "io_submit_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-IO-SETUP"]
            self.assertFalse(other, msg=stem)

    def test_api_request_key(self):
        hits = [f for f in run_lints([TD / "request_key_api.c"], TD)
                if f.cls == "API-REQUEST-KEY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reqkey_bad", names)
        self.assertNotIn("reqkey_ok", names)
        for stem in ("abs_ok", "keyctl_api", "add_key_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REQUEST-KEY"]
            self.assertFalse(other, msg=stem)

    def test_api_tkill(self):
        hits = [f for f in run_lints([TD / "tkill_api.c"], TD)
                if f.cls == "API-TKILL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tkill_bad", names)
        self.assertNotIn("tkill_ok", names)
        for stem in ("abs_ok", "kill_api", "tgkill_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TKILL"]
            self.assertFalse(other, msg=stem)

    def test_api_timer_delete(self):
        hits = [f for f in run_lints([TD / "timer_delete_api.c"], TD)
                if f.cls == "API-TIMER-DELETE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tdel_bad", names)
        self.assertNotIn("tdel_ok", names)
        for stem in ("abs_ok", "timer_create_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TIMER-DELETE"]
            self.assertFalse(other, msg=stem)

    def test_api_mq_unlink(self):
        hits = [f for f in run_lints([TD / "mq_unlink_api.c"], TD)
                if f.cls == "API-MQ-UNLINK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mqun_bad", names)
        self.assertNotIn("mqun_ok", names)
        for stem in ("abs_ok", "mq_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MQ-UNLINK"]
            self.assertFalse(other, msg=stem)

    def test_api_shmat(self):
        hits = [f for f in run_lints([TD / "shmat_api.c"], TD)
                if f.cls == "API-SHMAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shmat_bad", names)
        self.assertNotIn("shmat_ok", names)
        for stem in ("abs_ok", "shmget_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SHMAT"]
            self.assertFalse(other, msg=stem)

    def test_api_semop(self):
        hits = [f for f in run_lints([TD / "semop_api.c"], TD)
                if f.cls == "API-SEMOP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("semop_bad", names)
        self.assertNotIn("semop_ok", names)
        for stem in ("abs_ok", "semget_api", "sem_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEMOP"]
            self.assertFalse(other, msg=stem)

    def test_api_msgsnd(self):
        hits = [f for f in run_lints([TD / "msgsnd_api.c"], TD)
                if f.cls == "API-MSGSND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("msgsnd_bad", names)
        self.assertNotIn("msgsnd_ok", names)
        for stem in ("abs_ok", "msgget_api", "mq_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MSGSND"]
            self.assertFalse(other, msg=stem)

    def test_api_sync_file_range(self):
        hits = [f for f in run_lints([TD / "sync_file_range_api.c"], TD)
                if f.cls == "API-SYNC-FILE-RANGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sfr_bad", names)
        self.assertNotIn("sfr_ok", names)
        for stem in ("abs_ok", "syncfs_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYNC-FILE-RANGE"]
            self.assertFalse(other, msg=stem)

    def test_api_msync(self):
        hits = [f for f in run_lints([TD / "msync_api.c"], TD)
                if f.cls == "API-MSYNC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("msync_bad", names)
        self.assertNotIn("msync_ok", names)
        for stem in ("abs_ok", "mmap_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MSYNC"]
            self.assertFalse(other, msg=stem)

    def test_api_socketpair(self):
        hits = [f for f in run_lints([TD / "socketpair_api.c"], TD)
                if f.cls == "API-SOCKETPAIR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("spair_bad", names)
        self.assertNotIn("spair_ok", names)
        for stem in ("abs_ok", "socket_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SOCKETPAIR"]
            self.assertFalse(other, msg=stem)

    def test_api_sysinfo(self):
        hits = [f for f in run_lints([TD / "sysinfo_api.c"], TD)
                if f.cls == "API-SYSINFO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sysinfo_bad", names)
        self.assertNotIn("sysinfo_ok", names)
        for stem in ("abs_ok", "getrusage_api", "uname_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSINFO"]
            self.assertFalse(other, msg=stem)

    def test_api_clock_settime(self):
        hits = [f for f in run_lints([TD / "clock_settime_api.c"], TD)
                if f.cls == "API-CLOCK-SETTIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("clock_set_bad", names)
        self.assertNotIn("clock_set_ok", names)
        for stem in ("abs_ok", "clock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLOCK-SETTIME"]
            self.assertFalse(other, msg=stem)

    def test_api_settimeofday(self):
        hits = [f for f in run_lints([TD / "settimeofday_api.c"], TD)
                if f.cls == "API-SETTIMEOFDAY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stod_bad", names)
        self.assertNotIn("stod_ok", names)
        for stem in ("abs_ok", "clock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETTIMEOFDAY"]
            self.assertFalse(other, msg=stem)

    def test_api_gettid(self):
        hits = [f for f in run_lints([TD / "gettid_api.c"], TD)
                if f.cls == "API-GETTID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gettid_bad", names)
        self.assertNotIn("gettid_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETTID"]
            self.assertFalse(other, msg=stem)

    def test_api_sched_setscheduler(self):
        hits = [f for f in run_lints([TD / "sched_setscheduler_api.c"], TD)
                if f.cls == "API-SCHED-SETSCHEDULER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setsched_bad", names)
        self.assertNotIn("setsched_ok", names)
        for stem in ("abs_ok", "sched_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCHED-SETSCHEDULER"]
            self.assertFalse(other, msg=stem)

    def test_api_setitimer(self):
        hits = [f for f in run_lints([TD / "setitimer_api.c"], TD)
                if f.cls == "API-SETITIMER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sitimer_bad", names)
        self.assertNotIn("sitimer_ok", names)
        for stem in ("abs_ok", "timer_create_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETITIMER"]
            self.assertFalse(other, msg=stem)

    def test_api_nice(self):
        hits = [f for f in run_lints([TD / "nice_api.c"], TD)
                if f.cls == "API-NICE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nice_bad", names)
        self.assertNotIn("nice_ok", names)
        for stem in ("abs_ok", "getpriority_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NICE"]
            self.assertFalse(other, msg=stem)

    def test_api_arch_prctl(self):
        hits = [f for f in run_lints([TD / "arch_prctl_api.c"], TD)
                if f.cls == "API-ARCH-PRCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("archpr_bad", names)
        self.assertNotIn("archpr_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ARCH-PRCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_getdents(self):
        hits = [f for f in run_lints([TD / "getdents_api.c"], TD)
                if f.cls == "API-GETDENTS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getdents_bad", names)
        self.assertNotIn("getdents_ok", names)
        for stem in ("abs_ok", "opendir_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETDENTS"]
            self.assertFalse(other, msg=stem)

    def test_api_utimensat(self):
        hits = [f for f in run_lints([TD / "utimensat_api.c"], TD)
                if f.cls == "API-UTIMENSAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("utimens_bad", names)
        self.assertNotIn("utimens_ok", names)
        for stem in ("abs_ok", "clock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UTIMENSAT"]
            self.assertFalse(other, msg=stem)

    def test_api_linkat(self):
        hits = [f for f in run_lints([TD / "linkat_api.c"], TD)
                if f.cls == "API-LINKAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("linkat_bad", names)
        self.assertNotIn("linkat_ok", names)
        for stem in ("abs_ok", "symlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LINKAT"]
            self.assertFalse(other, msg=stem)

    def test_api_mbind(self):
        hits = [f for f in run_lints([TD / "mbind_api.c"], TD)
                if f.cls == "API-MBIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mbind_bad", names)
        self.assertNotIn("mbind_ok", names)
        for stem in ("abs_ok", "mmap_api", "mlock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MBIND"]
            self.assertFalse(other, msg=stem)

    def test_api_futex_waitv(self):
        hits = [f for f in run_lints([TD / "futex_waitv_api.c"], TD)
                if f.cls == "API-FUTEX-WAITV"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("futexv_bad", names)
        self.assertNotIn("futexv_ok", names)
        for stem in ("abs_ok", "futex_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FUTEX-WAITV"]
            self.assertFalse(other, msg=stem)

    def test_api_syslog(self):
        hits = [f for f in run_lints([TD / "syslog_api.c"], TD)
                if f.cls == "API-SYSLOG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("syslog_bad", names)
        self.assertNotIn("syslog_ok", names)
        for stem in ("abs_ok", "klogctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSLOG"]
            self.assertFalse(other, msg=stem)

    def test_api_setpgid(self):
        hits = [f for f in run_lints([TD / "setpgid_api.c"], TD)
                if f.cls == "API-SETPGID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setpgid_bad", names)
        self.assertNotIn("setpgid_ok", names)
        for stem in ("abs_ok", "setuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETPGID"]
            self.assertFalse(other, msg=stem)

    def test_api_setreuid(self):
        hits = [f for f in run_lints([TD / "setreuid_api.c"], TD)
                if f.cls == "API-SETREUID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setreuid_bad", names)
        self.assertNotIn("setreuid_ok", names)
        for stem in ("abs_ok", "setuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETREUID"]
            self.assertFalse(other, msg=stem)

    def test_api_getgroups(self):
        hits = [f for f in run_lints([TD / "getgroups_api.c"], TD)
                if f.cls == "API-GETGROUPS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getgroups_bad", names)
        self.assertNotIn("getgroups_ok", names)
        for stem in ("abs_ok", "initgroups_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETGROUPS"]
            self.assertFalse(other, msg=stem)

    def test_api_epoll_create(self):
        hits = [f for f in run_lints([TD / "epoll_create_api.c"], TD)
                if f.cls == "API-EPOLL-CREATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("epollc_bad", names)
        self.assertNotIn("epollc_ok", names)
        for stem in ("abs_ok", "select_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EPOLL-CREATE"]
            self.assertFalse(other, msg=stem)

    def test_api_timerfd_settime(self):
        hits = [f for f in run_lints([TD / "timerfd_settime_api.c"], TD)
                if f.cls == "API-TIMERFD-SETTIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tfds_bad", names)
        self.assertNotIn("tfds_ok", names)
        for stem in ("abs_ok", "memfd_api", "timer_create_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TIMERFD-SETTIME"]
            self.assertFalse(other, msg=stem)

    def test_api_remap_file_pages(self):
        hits = [f for f in run_lints([TD / "remap_file_pages_api.c"], TD)
                if f.cls == "API-REMAP-FILE-PAGES"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("remap_bad", names)
        self.assertNotIn("remap_ok", names)
        for stem in ("abs_ok", "mmap_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REMAP-FILE-PAGES"]
            self.assertFalse(other, msg=stem)

    def test_api_move_pages(self):
        hits = [f for f in run_lints([TD / "move_pages_api.c"], TD)
                if f.cls == "API-MOVE-PAGES"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mvpg_bad", names)
        self.assertNotIn("mvpg_ok", names)
        for stem in ("abs_ok", "process_vm_api", "mbind_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MOVE-PAGES"]
            self.assertFalse(other, msg=stem)

    def test_api_cachestat(self):
        hits = [f for f in run_lints([TD / "cachestat_api.c"], TD)
                if f.cls == "API-CACHESTAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cachestat_bad", names)
        self.assertNotIn("cachestat_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CACHESTAT"]
            self.assertFalse(other, msg=stem)

    def test_api_map_shadow_stack(self):
        hits = [f for f in run_lints([TD / "map_shadow_stack_api.c"], TD)
                if f.cls == "API-MAP-SHADOW-STACK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mshadow_bad", names)
        self.assertNotIn("mshadow_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MAP-SHADOW-STACK"]
            self.assertFalse(other, msg=stem)

    def test_api_sched_yield(self):
        hits = [f for f in run_lints([TD / "sched_yield_api.c"], TD)
                if f.cls == "API-SCHED-YIELD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("yield_bad", names)
        self.assertNotIn("yield_ok", names)
        for stem in ("abs_ok", "sched_api", "sched_setscheduler_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCHED-YIELD"]
            self.assertFalse(other, msg=stem)

    def test_api_setfsuid(self):
        hits = [f for f in run_lints([TD / "setfsuid_api.c"], TD)
                if f.cls == "API-SETFSUID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setfsuid_bad", names)
        self.assertNotIn("setfsuid_ok", names)
        for stem in ("abs_ok", "setuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETFSUID"]
            self.assertFalse(other, msg=stem)

    def test_api_wait4(self):
        hits = [f for f in run_lints([TD / "wait4_api.c"], TD)
                if f.cls == "API-WAIT4"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wait4_bad", names)
        self.assertNotIn("wait4_ok", names)
        for stem in ("abs_ok", "wait_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-WAIT4"]
            self.assertFalse(other, msg=stem)

    def test_api_preadv(self):
        hits = [f for f in run_lints([TD / "preadv_api.c"], TD)
                if f.cls == "API-PREADV"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("preadv_bad", names)
        self.assertNotIn("preadv_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PREADV"]
            self.assertFalse(other, msg=stem)

    def test_api_sendmsg(self):
        hits = [f for f in run_lints([TD / "sendmsg_api.c"], TD)
                if f.cls == "API-SENDMSG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sendmsg_bad", names)
        self.assertNotIn("sendmsg_ok", names)
        for stem in ("abs_ok", "send_api", "sendmmsg_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SENDMSG"]
            self.assertFalse(other, msg=stem)

    def test_api_getsockname(self):
        hits = [f for f in run_lints([TD / "getsockname_api.c"], TD)
                if f.cls == "API-GETSOCKNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getsock_bad", names)
        self.assertNotIn("getsock_ok", names)
        for stem in ("abs_ok", "bind_api", "accept_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETSOCKNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_epoll_pwait(self):
        hits = [f for f in run_lints([TD / "epoll_pwait_api.c"], TD)
                if f.cls == "API-EPOLL-PWAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("epollpw_bad", names)
        self.assertNotIn("epollpw_ok", names)
        for stem in ("abs_ok", "epoll_create_api", "select_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EPOLL-PWAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_inotify_rm_watch(self):
        hits = [f for f in run_lints([TD / "inotify_rm_watch_api.c"], TD)
                if f.cls == "API-INOTIFY-RM-WATCH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("inorm_bad", names)
        self.assertNotIn("inorm_ok", names)
        for stem in ("abs_ok", "inotify_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-INOTIFY-RM-WATCH"]
            self.assertFalse(other, msg=stem)

    def test_api_eventfd_read(self):
        hits = [f for f in run_lints([TD / "eventfd_rw_api.c"], TD)
                if f.cls == "API-EVENTFD-READ"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("efd_rw_bad", names)
        self.assertNotIn("efd_rw_ok", names)
        for stem in ("abs_ok", "memfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EVENTFD-READ"]
            self.assertFalse(other, msg=stem)

    def test_api_sched_setattr(self):
        hits = [f for f in run_lints([TD / "sched_setattr_api.c"], TD)
                if f.cls == "API-SCHED-SETATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setattr_bad", names)
        self.assertNotIn("setattr_ok", names)
        for stem in ("abs_ok", "sched_api", "sched_setscheduler_api",
                     "sched_yield_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SCHED-SETATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_renameat2(self):
        hits = [f for f in run_lints([TD / "renameat2_api.c"], TD)
                if f.cls == "API-RENAMEAT2"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("renameat2_bad", names)
        self.assertNotIn("renameat2_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RENAMEAT2"]
            self.assertFalse(other, msg=stem)

    def test_api_execveat(self):
        hits = [f for f in run_lints([TD / "execveat_api.c"], TD)
                if f.cls == "API-EXECVEAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("execveat_bad", names)
        self.assertNotIn("execveat_ok", names)
        for stem in ("abs_ok", "exec_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EXECVEAT"]
            self.assertFalse(other, msg=stem)

    def test_api_mlock2(self):
        hits = [f for f in run_lints([TD / "mlock2_api.c"], TD)
                if f.cls == "API-MLOCK2"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mlock2_bad", names)
        self.assertNotIn("mlock2_ok", names)
        for stem in ("abs_ok", "mlock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MLOCK2"]
            self.assertFalse(other, msg=stem)

    def test_api_faccessat2(self):
        hits = [f for f in run_lints([TD / "faccessat2_api.c"], TD)
                if f.cls == "API-FACCESSAT2"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("faccessat2_bad", names)
        self.assertNotIn("faccessat2_ok", names)
        for stem in ("abs_ok", "access_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FACCESSAT2"]
            self.assertFalse(other, msg=stem)

    def test_api_posix_fadvise(self):
        hits = [f for f in run_lints([TD / "fadvise_api.c"], TD)
                if f.cls == "API-POSIX-FADVISE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fadvise_bad", names)
        self.assertNotIn("fadvise_ok", names)
        for stem in ("abs_ok", "process_madvise_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-POSIX-FADVISE"]
            self.assertFalse(other, msg=stem)

    def test_api_readahead(self):
        hits = [f for f in run_lints([TD / "readahead_api.c"], TD)
                if f.cls == "API-READAHEAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("readahead_bad", names)
        self.assertNotIn("readahead_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-READAHEAD"]
            self.assertFalse(other, msg=stem)

    def test_api_sigaction(self):
        hits = [f for f in run_lints([TD / "sigaction_api.c"], TD)
                if f.cls == "API-SIGACTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sigaction_bad", names)
        self.assertNotIn("sigaction_ok", names)
        for stem in ("abs_ok", "signal_api", "signalfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGACTION"]
            self.assertFalse(other, msg=stem)

    def test_api_sigprocmask(self):
        hits = [f for f in run_lints([TD / "sigprocmask_api.c"], TD)
                if f.cls == "API-SIGPROCMASK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sigprocmask_bad", names)
        self.assertNotIn("sigprocmask_ok", names)
        for stem in ("abs_ok", "signal_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGPROCMASK"]
            self.assertFalse(other, msg=stem)

    def test_api_sem_open(self):
        hits = [f for f in run_lints([TD / "sem_open_api.c"], TD)
                if f.cls == "API-SEM-OPEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("semopen_bad", names)
        self.assertNotIn("semopen_ok", names)
        for stem in ("abs_ok", "sem_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEM-OPEN"]
            self.assertFalse(other, msg=stem)

    def test_api_rwlock(self):
        hits = [f for f in run_lints([TD / "rwlock_api.c"], TD)
                if f.cls == "API-RWLOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rwlock_bad", names)
        self.assertNotIn("rwlock_ok", names)
        for stem in ("abs_ok", "lock_ok", "pthread_join_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RWLOCK"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_cond(self):
        hits = [f for f in run_lints([TD / "pthread_cond_api.c"], TD)
                if f.cls == "API-PTHREAD-COND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pcond_bad", names)
        self.assertNotIn("pcond_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-COND"]
            self.assertFalse(other, msg=stem)
        for stem in ("cond_wait", "condvar"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "API-PTHREAD-COND"]
            self.assertFalse(other, msg=stem)

    def test_api_sigaltstack(self):
        hits = [f for f in run_lints([TD / "sigaltstack_api.c"], TD)
                if f.cls == "API-SIGALTSTACK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sigalt_bad", names)
        self.assertNotIn("sigalt_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGALTSTACK"]
            self.assertFalse(other, msg=stem)

    def test_api_renameat(self):
        hits = [f for f in run_lints([TD / "renameat_api.c"], TD)
                if f.cls == "API-RENAMEAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("renameat_bad", names)
        self.assertNotIn("renameat_ok", names)
        for stem in ("abs_ok", "renameat2_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RENAMEAT"]
            self.assertFalse(other, msg=stem)

    def test_api_faccessat(self):
        hits = [f for f in run_lints([TD / "faccessat_api.c"], TD)
                if f.cls == "API-FACCESSAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("faccessat_bad", names)
        self.assertNotIn("faccessat_ok", names)
        for stem in ("abs_ok", "faccessat2_api", "access_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FACCESSAT"]
            self.assertFalse(other, msg=stem)

    def test_api_fchmodat(self):
        hits = [f for f in run_lints([TD / "fchmodat_api.c"], TD)
                if f.cls == "API-FCHMODAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fchmodat_bad", names)
        self.assertNotIn("fchmodat_ok", names)
        for stem in ("abs_ok", "chmod_world"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FCHMODAT"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_barrier(self):
        hits = [f for f in run_lints([TD / "pthread_barrier_api.c"], TD)
                if f.cls == "API-PTHREAD-BARRIER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pbar_bad", names)
        self.assertNotIn("pbar_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-BARRIER"]
            self.assertFalse(other, msg=stem)
        other = [f for f in run_lints([TD / "barrier_lint.cpp"], TD)
                 if f.cls == "API-PTHREAD-BARRIER"]
        self.assertFalse(other, msg="barrier_lint")

    def test_api_symlinkat(self):
        hits = [f for f in run_lints([TD / "symlinkat_api.c"], TD)
                if f.cls == "API-SYMLINKAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("symlinkat_bad", names)
        self.assertNotIn("symlinkat_ok", names)
        for stem in ("abs_ok", "symlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYMLINKAT"]
            self.assertFalse(other, msg=stem)

    def test_api_unlinkat(self):
        hits = [f for f in run_lints([TD / "unlinkat_api.c"], TD)
                if f.cls == "API-UNLINKAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unlinkat_bad", names)
        self.assertNotIn("unlinkat_ok", names)
        for stem in ("abs_ok", "unlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UNLINKAT"]
            self.assertFalse(other, msg=stem)

    def test_api_mkdirat(self):
        hits = [f for f in run_lints([TD / "mkdirat_api.c"], TD)
                if f.cls == "API-MKDIRAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mkdirat_bad", names)
        self.assertNotIn("mkdirat_ok", names)
        for stem in ("abs_ok", "mkdir_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKDIRAT"]
            self.assertFalse(other, msg=stem)

    def test_api_mknodat(self):
        hits = [f for f in run_lints([TD / "mknodat_api.c"], TD)
                if f.cls == "API-MKNODAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mknodat_bad", names)
        self.assertNotIn("mknodat_ok", names)
        for stem in ("abs_ok", "mkfifo_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MKNODAT"]
            self.assertFalse(other, msg=stem)

    def test_api_readlinkat(self):
        hits = [f for f in run_lints([TD / "readlinkat_api.c"], TD)
                if f.cls == "API-READLINKAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("readlinkat_bad", names)
        self.assertNotIn("readlinkat_ok", names)
        for stem in ("abs_ok", "symlink_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-READLINKAT"]
            self.assertFalse(other, msg=stem)

    def test_api_fstatat(self):
        hits = [f for f in run_lints([TD / "fstatat_api.c"], TD)
                if f.cls == "API-FSTATAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fstatat_bad", names)
        self.assertNotIn("fstatat_ok", names)
        for stem in ("abs_ok", "stat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FSTATAT"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_spin(self):
        hits = [f for f in run_lints([TD / "spin_api.c"], TD)
                if f.cls == "API-PTHREAD-SPIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("spin_bad", names)
        self.assertNotIn("spin_ok", names)
        for stem in ("abs_ok", "lock_ok", "rwlock_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-SPIN"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_key(self):
        hits = [f for f in run_lints([TD / "pthread_key_api.c"], TD)
                if f.cls == "API-PTHREAD-KEY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tkey_bad", names)
        self.assertNotIn("tkey_ok", names)
        for stem in ("abs_ok", "pthread_join_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-KEY"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_cancel(self):
        hits = [f for f in run_lints([TD / "pthread_cancel_api.c"], TD)
                if f.cls == "API-PTHREAD-CANCEL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pcancel_bad", names)
        self.assertNotIn("pcancel_ok", names)
        for stem in ("abs_ok", "pthread_join_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-CANCEL"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_kill(self):
        hits = [f for f in run_lints([TD / "pthread_kill_api.c"], TD)
                if f.cls == "API-PTHREAD-KILL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pkill_bad", names)
        self.assertNotIn("pkill_ok", names)
        for stem in ("abs_ok", "kill_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-KILL"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_sigmask(self):
        hits = [f for f in run_lints([TD / "pthread_sigmask_api.c"], TD)
                if f.cls == "API-PTHREAD-SIGMASK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("psigmask_bad", names)
        self.assertNotIn("psigmask_ok", names)
        for stem in ("abs_ok", "sigprocmask_api", "signal_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-SIGMASK"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_atfork(self):
        hits = [f for f in run_lints([TD / "pthread_atfork_api.c"], TD)
                if f.cls == "API-PTHREAD-ATFORK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("patfork_bad", names)
        self.assertNotIn("patfork_ok", names)
        for stem in ("abs_ok", "fork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-ATFORK"]
            self.assertFalse(other, msg=stem)

    def test_api_pledge(self):
        hits = [f for f in run_lints([TD / "pledge_api.c"], TD)
                if f.cls == "API-PLEDGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pledge_bad", names)
        self.assertNotIn("pledge_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PLEDGE"]
            self.assertFalse(other, msg=stem)

    def test_api_unveil(self):
        hits = [f for f in run_lints([TD / "unveil_api.c"], TD)
                if f.cls == "API-UNVEIL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unveil_bad", names)
        self.assertNotIn("unveil_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UNVEIL"]
            self.assertFalse(other, msg=stem)

    def test_api_sysctl(self):
        hits = [f for f in run_lints([TD / "sysctl_api.c"], TD)
                if f.cls == "API-SYSCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sysctl_bad", names)
        self.assertNotIn("sysctl_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_kqueue(self):
        hits = [f for f in run_lints([TD / "kqueue_api.c"], TD)
                if f.cls == "API-KQUEUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kqueue_bad", names)
        self.assertNotIn("kqueue_ok", names)
        for stem in ("abs_ok", "kevent_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KQUEUE"]
            self.assertFalse(other, msg=stem)

    def test_api_kevent(self):
        hits = [f for f in run_lints([TD / "kevent_api.c"], TD)
                if f.cls == "API-KEVENT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kevent_bad", names)
        self.assertNotIn("kevent_ok", names)
        for stem in ("abs_ok", "kqueue_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KEVENT"]
            self.assertFalse(other, msg=stem)

    def test_api_pause(self):
        hits = [f for f in run_lints([TD / "pause_api.c"], TD)
                if f.cls == "API-PAUSE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pause_bad", names)
        self.assertNotIn("pause_ok", names)
        for stem in ("abs_ok", "sleep_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PAUSE"]
            self.assertFalse(other, msg=stem)

    def test_api_ppoll(self):
        hits = [f for f in run_lints([TD / "ppoll_api.c"], TD)
                if f.cls == "API-PPOLL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ppoll_bad", names)
        self.assertNotIn("ppoll_ok", names)
        for stem in ("abs_ok", "select_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PPOLL"]
            self.assertFalse(other, msg=stem)

    def test_api_sigwait(self):
        hits = [f for f in run_lints([TD / "sigwait_api.c"], TD)
                if f.cls == "API-SIGWAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sigwait_bad", names)
        self.assertNotIn("sigwait_ok", names)
        for stem in ("abs_ok", "sigprocmask_api", "signal_api", "signalfd_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGWAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_sigqueue(self):
        hits = [f for f in run_lints([TD / "sigqueue_api.c"], TD)
                if f.cls == "API-SIGQUEUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sigqueue_bad", names)
        self.assertNotIn("sigqueue_ok", names)
        for stem in ("abs_ok", "kill_api", "sigqueueinfo_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SIGQUEUE"]
            self.assertFalse(other, msg=stem)

    def test_api_ucontext(self):
        hits = [f for f in run_lints([TD / "ucontext_api.c"], TD)
                if f.cls == "API-UCONTEXT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ucontext_bad", names)
        self.assertNotIn("ucontext_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UCONTEXT"]
            self.assertFalse(other, msg=stem)

    def test_api_sem_timedwait(self):
        hits = [f for f in run_lints([TD / "sem_timedwait_api.c"], TD)
                if f.cls == "API-SEM-TIMEDWAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stimed_bad", names)
        self.assertNotIn("stimed_ok", names)
        for stem in ("abs_ok", "sem_open_api", "sem_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEM-TIMEDWAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_attr(self):
        hits = [f for f in run_lints([TD / "pthread_attr_api.c"], TD)
                if f.cls == "API-PTHREAD-ATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pattr_bad", names)
        self.assertNotIn("pattr_ok", names)
        for stem in ("abs_ok", "pthread_join_api", "pthread_atfork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-ATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_enter(self):
        hits = [f for f in run_lints([TD / "cap_enter_api.c"], TD)
                if f.cls == "API-CAP-ENTER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capenter_bad", names)
        self.assertNotIn("capenter_ok", names)
        for stem in ("abs_ok", "pledge_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-ENTER"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_rights(self):
        hits = [f for f in run_lints([TD / "cap_rights_api.c"], TD)
                if f.cls == "API-CAP-RIGHTS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capr_bad", names)
        self.assertNotIn("capr_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-RIGHTS"]
            self.assertFalse(other, msg=stem)

    def test_api_pdfork(self):
        hits = [f for f in run_lints([TD / "pdfork_api.c"], TD)
                if f.cls == "API-PDFORK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pdfork_bad", names)
        self.assertNotIn("pdfork_ok", names)
        for stem in ("abs_ok", "fork_api", "pthread_atfork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PDFORK"]
            self.assertFalse(other, msg=stem)

    def test_api_procctl(self):
        hits = [f for f in run_lints([TD / "procctl_api.c"], TD)
                if f.cls == "API-PROCCTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("procctl_bad", names)
        self.assertNotIn("procctl_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PROCCTL"]
            self.assertFalse(other, msg=stem)

    def test_api_closefrom(self):
        hits = [f for f in run_lints([TD / "closefrom_api.c"], TD)
                if f.cls == "API-CLOSEFROM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("closefrom_bad", names)
        self.assertNotIn("closefrom_ok", names)
        for stem in ("abs_ok", "close_range_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CLOSEFROM"]
            self.assertFalse(other, msg=stem)

    def test_api_issetugid(self):
        hits = [f for f in run_lints([TD / "issetugid_api.c"], TD)
                if f.cls == "API-ISSETUGID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("issetugid_bad", names)
        self.assertNotIn("issetugid_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ISSETUGID"]
            self.assertFalse(other, msg=stem)

    def test_api_arc4random(self):
        hits = [f for f in run_lints([TD / "arc4random_api.c"], TD)
                if f.cls == "API-ARC4RANDOM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("arc4_bad", names)
        self.assertNotIn("arc4_ok", names)
        for stem in ("abs_ok", "getrandom_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ARC4RANDOM"]
            self.assertFalse(other, msg=stem)

    def test_api_chflags(self):
        hits = [f for f in run_lints([TD / "chflags_api.c"], TD)
                if f.cls == "API-CHFLAGS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chflags_bad", names)
        self.assertNotIn("chflags_ok", names)
        for stem in ("abs_ok", "chmod_world"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CHFLAGS"]
            self.assertFalse(other, msg=stem)

    def test_api_getfsstat(self):
        hits = [f for f in run_lints([TD / "getfsstat_api.c"], TD)
                if f.cls == "API-GETFSSTAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getfsstat_bad", names)
        self.assertNotIn("getfsstat_ok", names)
        for stem in ("abs_ok", "statfs_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETFSSTAT"]
            self.assertFalse(other, msg=stem)

    def test_api_pthread_yield(self):
        hits = [f for f in run_lints([TD / "pthread_yield_api.c"], TD)
                if f.cls == "API-PTHREAD-YIELD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pyield_bad", names)
        self.assertNotIn("pyield_ok", names)
        for stem in ("abs_ok", "sched_yield_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PTHREAD-YIELD"]
            self.assertFalse(other, msg=stem)

    def test_api_sem_trywait(self):
        hits = [f for f in run_lints([TD / "sem_trywait_api.c"], TD)
                if f.cls == "API-SEM-TRYWAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stry_bad", names)
        self.assertNotIn("stry_ok", names)
        for stem in ("abs_ok", "sem_api", "sem_timedwait_api", "sem_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SEM-TRYWAIT"]
            self.assertFalse(other, msg=stem)

    def test_api_adjtime(self):
        hits = [f for f in run_lints([TD / "adjtime_api.c"], TD)
                if f.cls == "API-ADJTIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("adjtime_bad", names)
        self.assertNotIn("adjtime_ok", names)
        for stem in ("abs_ok", "adjtimex_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-ADJTIME"]
            self.assertFalse(other, msg=stem)

    def test_api_revoke(self):
        hits = [f for f in run_lints([TD / "revoke_api.c"], TD)
                if f.cls == "API-REVOKE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("revoke_bad", names)
        self.assertNotIn("revoke_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REVOKE"]
            self.assertFalse(other, msg=stem)

    def test_api_ktrace(self):
        hits = [f for f in run_lints([TD / "ktrace_api.c"], TD)
                if f.cls == "API-KTRACE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ktrace_bad", names)
        self.assertNotIn("ktrace_ok", names)
        for stem in ("abs_ok", "prctl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KTRACE"]
            self.assertFalse(other, msg=stem)

    def test_api_rfork(self):
        hits = [f for f in run_lints([TD / "rfork_api.c"], TD)
                if f.cls == "API-RFORK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rfork_bad", names)
        self.assertNotIn("rfork_ok", names)
        for stem in ("abs_ok", "fork_api", "pdfork_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RFORK"]
            self.assertFalse(other, msg=stem)

    def test_api_jail(self):
        hits = [f for f in run_lints([TD / "jail_api.c"], TD)
                if f.cls == "API-JAIL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("jail_bad", names)
        self.assertNotIn("jail_ok", names)
        for stem in ("abs_ok", "chroot"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-JAIL"]
            self.assertFalse(other, msg=stem)

    def test_api_setlogin(self):
        hits = [f for f in run_lints([TD / "setlogin_api.c"], TD)
                if f.cls == "API-SETLOGIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setlogin_bad", names)
        self.assertNotIn("setlogin_ok", names)
        for stem in ("abs_ok", "getlogin_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETLOGIN"]
            self.assertFalse(other, msg=stem)

    def test_api_getresuid(self):
        hits = [f for f in run_lints([TD / "getresuid_api.c"], TD)
                if f.cls == "API-GETRESUID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getresuid_bad", names)
        self.assertNotIn("getresuid_ok", names)
        for stem in ("abs_ok", "setreuid_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETRESUID"]
            self.assertFalse(other, msg=stem)

    def test_api_getpeereid(self):
        hits = [f for f in run_lints([TD / "getpeereid_api.c"], TD)
                if f.cls == "API-GETPEEREID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getpeereid_bad", names)
        self.assertNotIn("getpeereid_ok", names)
        for stem in ("abs_ok", "getsockname_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPEEREID"]
            self.assertFalse(other, msg=stem)

    def test_api_strtonum(self):
        hits = [f for f in run_lints([TD / "strtonum_api.c"], TD)
                if f.cls == "API-STRTONUM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strtonum_bad", names)
        self.assertNotIn("strtonum_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STRTONUM"]
            self.assertFalse(other, msg=stem)

    def test_api_reallocarray(self):
        hits = [f for f in run_lints([TD / "reallocarray_api.c"], TD)
                if f.cls == "API-REALLOCARRAY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reallocarr_bad", names)
        self.assertNotIn("reallocarr_ok", names)
        for stem in ("abs_ok", "realloc_self", "realloc_zero"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REALLOCARRAY"]
            self.assertFalse(other, msg=stem)

    def test_api_timingsafe(self):
        hits = [f for f in run_lints([TD / "timingsafe_api.c"], TD)
                if f.cls == "API-TIMINGSAFE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tsafe_bad", names)
        self.assertNotIn("tsafe_ok", names)
        for stem in ("abs_ok", "bzero_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-TIMINGSAFE"]
            self.assertFalse(other, msg=stem)

    def test_api_getprogname(self):
        hits = [f for f in run_lints([TD / "getprogname_api.c"], TD)
                if f.cls == "API-GETPROGNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getprog_bad", names)
        self.assertNotIn("getprog_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPROGNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_daemon(self):
        hits = [f for f in run_lints([TD / "daemon_api.c"], TD)
                if f.cls == "API-DAEMON"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("daemon_bad", names)
        self.assertNotIn("daemon_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-DAEMON"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_fcntls(self):
        hits = [f for f in run_lints([TD / "cap_fcntls_api.c"], TD)
                if f.cls == "API-CAP-FCNTLS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capfcntls_bad", names)
        self.assertNotIn("capfcntls_ok", names)
        for stem in ("abs_ok", "cap_enter_api", "cap_rights_api", "fcntl_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-FCNTLS"]
            self.assertFalse(other, msg=stem)

    def test_api_pdgetpid(self):
        hits = [f for f in run_lints([TD / "pdgetpid_api.c"], TD)
                if f.cls == "API-PDGETPID"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pdgetpid_bad", names)
        self.assertNotIn("pdgetpid_ok", names)
        for stem in ("abs_ok", "pdfork_api", "wait4_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-PDGETPID"]
            self.assertFalse(other, msg=stem)

    def test_api_kldload(self):
        hits = [f for f in run_lints([TD / "kldload_api.c"], TD)
                if f.cls == "API-KLDLOAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kldload_bad", names)
        self.assertNotIn("kldload_ok", names)
        for stem in ("abs_ok", "dlopen_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KLDLOAD"]
            self.assertFalse(other, msg=stem)

    def test_api_extattr(self):
        hits = [f for f in run_lints([TD / "extattr_api.c"], TD)
                if f.cls == "API-EXTATTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("extattr_bad", names)
        self.assertNotIn("extattr_ok", names)
        for stem in ("abs_ok", "setxattr_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EXTATTR"]
            self.assertFalse(other, msg=stem)

    def test_api_mac(self):
        hits = [f for f in run_lints([TD / "mac_api.c"], TD)
                if f.cls == "API-MAC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mac_bad", names)
        self.assertNotIn("mac_ok", names)
        for stem in ("abs_ok", "pledge_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MAC"]
            self.assertFalse(other, msg=stem)

    def test_api_audit(self):
        hits = [f for f in run_lints([TD / "audit_api.c"], TD)
                if f.cls == "API-AUDIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("audit_bad", names)
        self.assertNotIn("audit_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-AUDIT"]
            self.assertFalse(other, msg=stem)

    def test_api_kvm(self):
        hits = [f for f in run_lints([TD / "kvm_api.c"], TD)
                if f.cls == "API-KVM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kvm_bad", names)
        self.assertNotIn("kvm_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KVM"]
            self.assertFalse(other, msg=stem)

    def test_api_reallocf(self):
        hits = [f for f in run_lints([TD / "reallocf_api.c"], TD)
                if f.cls == "API-REALLOCF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reallocf_bad", names)
        self.assertNotIn("reallocf_ok", names)
        for stem in ("abs_ok", "reallocarray_api", "realloc_self"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-REALLOCF"]
            self.assertFalse(other, msg=stem)

    def test_api_uuidgen(self):
        hits = [f for f in run_lints([TD / "uuidgen_api.c"], TD)
                if f.cls == "API-UUIDGEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uuidgen_bad", names)
        self.assertNotIn("uuidgen_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UUIDGEN"]
            self.assertFalse(other, msg=stem)

    def test_api_setfib(self):
        hits = [f for f in run_lints([TD / "setfib_api.c"], TD)
                if f.cls == "API-SETFIB"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setfib_bad", names)
        self.assertNotIn("setfib_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SETFIB"]
            self.assertFalse(other, msg=stem)

    def test_api_ntp_gettime(self):
        hits = [f for f in run_lints([TD / "ntp_gettime_api.c"], TD)
                if f.cls == "API-NTP-GETTIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ntpgt_bad", names)
        self.assertNotIn("ntpgt_ok", names)
        for stem in ("abs_ok", "adjtime_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NTP-GETTIME"]
            self.assertFalse(other, msg=stem)

    def test_api_crypt_newhash(self):
        hits = [f for f in run_lints([TD / "crypt_newhash_api.c"], TD)
                if f.cls == "API-CRYPT-NEWHASH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cnew_bad", names)
        self.assertNotIn("cnew_ok", names)
        for stem in ("abs_ok", "crypto_misuse"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CRYPT-NEWHASH"]
            self.assertFalse(other, msg=stem)

    def test_api_wait6(self):
        hits = [f for f in run_lints([TD / "wait6_api.c"], TD)
                if f.cls == "API-WAIT6"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wait6_bad", names)
        self.assertNotIn("wait6_ok", names)
        for stem in ("abs_ok", "wait4_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-WAIT6"]
            self.assertFalse(other, msg=stem)

    def test_api_cpuset(self):
        hits = [f for f in run_lints([TD / "cpuset_api.c"], TD)
                if f.cls == "API-CPUSET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cpuset_bad", names)
        self.assertNotIn("cpuset_ok", names)
        for stem in ("abs_ok", "sched_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CPUSET"]
            self.assertFalse(other, msg=stem)

    def test_api_rtprio(self):
        hits = [f for f in run_lints([TD / "rtprio_api.c"], TD)
                if f.cls == "API-RTPRIO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rtprio_bad", names)
        self.assertNotIn("rtprio_ok", names)
        for stem in ("abs_ok", "nice_api", "getpriority_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-RTPRIO"]
            self.assertFalse(other, msg=stem)

    def test_api_kenv(self):
        hits = [f for f in run_lints([TD / "kenv_api.c"], TD)
                if f.cls == "API-KENV"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kenv_bad", names)
        self.assertNotIn("kenv_ok", names)
        for stem in ("abs_ok", "getenv_null"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KENV"]
            self.assertFalse(other, msg=stem)

    def test_api_getfh(self):
        hits = [f for f in run_lints([TD / "getfh_api.c"], TD)
                if f.cls == "API-GETFH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getfh_bad", names)
        self.assertNotIn("getfh_ok", names)
        for stem in ("abs_ok", "stat_api", "statfs_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETFH"]
            self.assertFalse(other, msg=stem)

    def test_api_getmntinfo(self):
        hits = [f for f in run_lints([TD / "getmntinfo_api.c"], TD)
                if f.cls == "API-GETMNTINFO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("getmntinfo_bad", names)
        self.assertNotIn("getmntinfo_ok", names)
        for stem in ("abs_ok", "getfsstat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETMNTINFO"]
            self.assertFalse(other, msg=stem)

    def test_api_nmount(self):
        hits = [f for f in run_lints([TD / "nmount_api.c"], TD)
                if f.cls == "API-NMOUNT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nmount_bad", names)
        self.assertNotIn("nmount_ok", names)
        for stem in ("abs_ok", "mount_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NMOUNT"]
            self.assertFalse(other, msg=stem)

    def test_api_strmode(self):
        hits = [f for f in run_lints([TD / "strmode_api.c"], TD)
                if f.cls == "API-STRMODE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strmode_bad", names)
        self.assertNotIn("strmode_ok", names)
        for stem in ("abs_ok", "strtonum_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-STRMODE"]
            self.assertFalse(other, msg=stem)

    def test_api_getosreldate(self):
        hits = [f for f in run_lints([TD / "getosreldate_api.c"], TD)
                if f.cls == "API-GETOSRELDATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("osrel_bad", names)
        self.assertNotIn("osrel_ok", names)
        for stem in ("abs_ok", "uname_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETOSRELDATE"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_sandboxed(self):
        hits = [f for f in run_lints([TD / "cap_sandboxed_api.c"], TD)
                if f.cls == "API-CAP-SANDBOXED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capsand_bad", names)
        self.assertNotIn("capsand_ok", names)
        for stem in ("abs_ok", "cap_enter_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-SANDBOXED"]
            self.assertFalse(other, msg=stem)

    def test_api_getgrouplist(self):
        hits = [f for f in run_lints([TD / "getgrouplist_api.c"], TD)
                if f.cls == "API-GETGROUPLIST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ggl_bad", names)
        self.assertNotIn("ggl_ok", names)
        for stem in ("abs_ok", "getgroups_api", "initgroups_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETGROUPLIST"]
            self.assertFalse(other, msg=stem)

    def test_api_eaccess(self):
        hits = [f for f in run_lints([TD / "eaccess_api.c"], TD)
                if f.cls == "API-EACCESS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("eaccess_bad", names)
        self.assertNotIn("eaccess_ok", names)
        for stem in ("abs_ok", "access_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-EACCESS"]
            self.assertFalse(other, msg=stem)

    def test_api_login_getclass(self):
        hits = [f for f in run_lints([TD / "login_class_api.c"], TD)
                if f.cls == "API-LOGIN-GETCLASS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lclass_bad", names)
        self.assertNotIn("lclass_ok", names)
        for stem in ("abs_ok", "getlogin_api", "setlogin_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LOGIN-GETCLASS"]
            self.assertFalse(other, msg=stem)

    def test_api_fflags(self):
        hits = [f for f in run_lints([TD / "fflags_api.c"], TD)
                if f.cls == "API-FFLAGS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fflags_bad", names)
        self.assertNotIn("fflags_ok", names)
        for stem in ("abs_ok", "strmode_api", "strtonum_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FFLAGS"]
            self.assertFalse(other, msg=stem)

    def test_api_getdirentries(self):
        hits = [f for f in run_lints([TD / "getdirentries_api.c"], TD)
                if f.cls == "API-GETDIRENTRIES"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gde_bad", names)
        self.assertNotIn("gde_ok", names)
        for stem in ("abs_ok", "getdents_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETDIRENTRIES"]
            self.assertFalse(other, msg=stem)

    def test_api_kinfo(self):
        hits = [f for f in run_lints([TD / "kinfo_api.c"], TD)
                if f.cls == "API-KINFO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kinfo_bad", names)
        self.assertNotIn("kinfo_ok", names)
        for stem in ("abs_ok", "kvm_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KINFO"]
            self.assertFalse(other, msg=stem)

    def test_api_umtx(self):
        hits = [f for f in run_lints([TD / "umtx_api.c"], TD)
                if f.cls == "API-UMTX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("umtx_bad", names)
        self.assertNotIn("umtx_ok", names)
        for stem in ("abs_ok", "futex_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-UMTX"]
            self.assertFalse(other, msg=stem)

    def test_api_thr(self):
        hits = [f for f in run_lints([TD / "thr_api.c"], TD)
                if f.cls == "API-THR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("thr_bad", names)
        self.assertNotIn("thr_ok", names)
        for stem in ("abs_ok", "pthread_kill_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-THR"]
            self.assertFalse(other, msg=stem)

    def test_api_modfind(self):
        hits = [f for f in run_lints([TD / "modfind_api.c"], TD)
                if f.cls == "API-MODFIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("modfind_bad", names)
        self.assertNotIn("modfind_ok", names)
        for stem in ("abs_ok", "kldload_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MODFIND"]
            self.assertFalse(other, msg=stem)

    def test_api_lpathconf(self):
        hits = [f for f in run_lints([TD / "lpathconf_api.c"], TD)
                if f.cls == "API-LPATHCONF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lpath_bad", names)
        self.assertNotIn("lpath_ok", names)
        for stem in ("abs_ok", "sysconf_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LPATHCONF"]
            self.assertFalse(other, msg=stem)

    def test_api_loginclass(self):
        hits = [f for f in run_lints([TD / "loginclass_api.c"], TD)
                if f.cls == "API-LOGINCLASS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lgncls_bad", names)
        self.assertNotIn("lgncls_ok", names)
        for stem in ("abs_ok", "getlogin_api", "setlogin_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-LOGINCLASS"]
            self.assertFalse(other, msg=stem)

    def test_api_getfsent(self):
        hits = [f for f in run_lints([TD / "getfsent_api.c"], TD)
                if f.cls == "API-GETFSENT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fsent_bad", names)
        self.assertNotIn("fsent_ok", names)
        for stem in ("abs_ok", "getfsstat_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETFSENT"]
            self.assertFalse(other, msg=stem)

    def test_api_minherit(self):
        hits = [f for f in run_lints([TD / "minherit_api.c"], TD)
                if f.cls == "API-MINHERIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("minherit_bad", names)
        self.assertNotIn("minherit_ok", names)
        for stem in ("abs_ok", "mmap_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-MINHERIT"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_getmode(self):
        hits = [f for f in run_lints([TD / "cap_getmode_api.c"], TD)
                if f.cls == "API-CAP-GETMODE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capmode_bad", names)
        self.assertNotIn("capmode_ok", names)
        for stem in ("abs_ok", "cap_enter_api", "cap_sandboxed_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-GETMODE"]
            self.assertFalse(other, msg=stem)

    def test_api_nfssvc(self):
        hits = [f for f in run_lints([TD / "nfssvc_api.c"], TD)
                if f.cls == "API-NFSSVC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nfssvc_bad", names)
        self.assertNotIn("nfssvc_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-NFSSVC"]
            self.assertFalse(other, msg=stem)

    def test_api_sysarch(self):
        hits = [f for f in run_lints([TD / "sysarch_api.c"], TD)
                if f.cls == "API-SYSARCH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sysarch_bad", names)
        self.assertNotIn("sysarch_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SYSARCH"]
            self.assertFalse(other, msg=stem)

    def test_api_getpagesizes(self):
        hits = [f for f in run_lints([TD / "getpagesizes_api.c"], TD)
                if f.cls == "API-GETPAGESIZES"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gps_bad", names)
        self.assertNotIn("gps_ok", names)
        for stem in ("abs_ok", "sysconf_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETPAGESIZES"]
            self.assertFalse(other, msg=stem)

    def test_api_sbrk(self):
        hits = [f for f in run_lints([TD / "sbrk_api.c"], TD)
                if f.cls == "API-SBRK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sbrk_bad", names)
        self.assertNotIn("sbrk_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-SBRK"]
            self.assertFalse(other, msg=stem)

    def test_api_ksem(self):
        hits = [f for f in run_lints([TD / "ksem_api.c"], TD)
                if f.cls == "API-KSEM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ksem_bad", names)
        self.assertNotIn("ksem_ok", names)
        for stem in ("abs_ok", "sem_open_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KSEM"]
            self.assertFalse(other, msg=stem)

    def test_api_cap_getrights(self):
        hits = [f for f in run_lints([TD / "cap_getrights_api.c"], TD)
                if f.cls == "API-CAP-GETRIGHTS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("capgr_bad", names)
        self.assertNotIn("capgr_ok", names)
        for stem in ("abs_ok", "cap_rights_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-CAP-GETRIGHTS"]
            self.assertFalse(other, msg=stem)

    def test_api_devname(self):
        hits = [f for f in run_lints([TD / "devname_api.c"], TD)
                if f.cls == "API-DEVNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("devname_bad", names)
        self.assertNotIn("devname_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-DEVNAME"]
            self.assertFalse(other, msg=stem)

    def test_api_getbootfile(self):
        hits = [f for f in run_lints([TD / "getbootfile_api.c"], TD)
                if f.cls == "API-GETBOOTFILE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bootfile_bad", names)
        self.assertNotIn("bootfile_ok", names)
        for stem in ("abs_ok",):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETBOOTFILE"]
            self.assertFalse(other, msg=stem)

    def test_api_kldfirstmod(self):
        hits = [f for f in run_lints([TD / "kldfirstmod_api.c"], TD)
                if f.cls == "API-KLDFIRSTMOD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("kldfm_bad", names)
        self.assertNotIn("kldfm_ok", names)
        for stem in ("abs_ok", "kldload_api", "modfind_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-KLDFIRSTMOD"]
            self.assertFalse(other, msg=stem)

    def test_api_fhlink(self):
        hits = [f for f in run_lints([TD / "fhlink_api.c"], TD)
                if f.cls == "API-FHLINK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fhlink_bad", names)
        self.assertNotIn("fhlink_ok", names)
        for stem in ("abs_ok", "linkat_api", "getfh_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-FHLINK"]
            self.assertFalse(other, msg=stem)

    def test_api_valloc(self):
        hits = [f for f in run_lints([TD / "valloc_api.c"], TD)
                if f.cls == "API-VALLOC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("valloc_bad", names)
        self.assertNotIn("valloc_ok", names)
        for stem in ("abs_ok", "aligned_unenc"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-VALLOC"]
            self.assertFalse(other, msg=stem)

    def test_api_getdomainname(self):
        hits = [f for f in run_lints([TD / "getdomainname_api.c"], TD)
                if f.cls == "API-GETDOMAINNAME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gdname_bad", names)
        self.assertNotIn("gdname_ok", names)
        for stem in ("abs_ok", "uname_api"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "API-GETDOMAINNAME"]
            self.assertFalse(other, msg=stem)

    def test_str_strncpy_nul(self):
        hits = [f for f in run_lints([TD / "strncpy_nul.c"], TD)
                if f.cls == "STR-STRNCPY-NUL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("strncpy_nul_bad", names)
        self.assertNotIn("strncpy_nul_ok", names)
        memcpy = [f for f in run_lints([TD / "strncpy_nul.c"], TD)
                  if f.cls == "STR-MISSING-NUL"]
        self.assertFalse(memcpy)
        for stem in ("abs_ok", "missing_nul"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "STR-STRNCPY-NUL"]
            self.assertFalse(other, msg=stem)

    def test_srand_time(self):
        hits = [f for f in run_lints([TD / "srand_time.c"], TD)
                if f.cls == "CRYPTO-SRAND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("srand_bad", names)
        self.assertNotIn("srand_ok", names)
        for stem in ("abs_ok", "crypto_misuse"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "CRYPTO-SRAND"]
            self.assertFalse(other, msg=stem)

    def test_realloc_zero(self):
        hits = [f for f in run_lints([TD / "realloc_zero.c"], TD)
                if f.cls == "MEM-REALLOC-ZERO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("realloc_zero_bad", names)
        self.assertNotIn("realloc_zero_ok", names)
        for stem in ("abs_ok", "realloc_self"):
            other = [f for f in run_lints([TD / f"{stem}.c"], TD)
                     if f.cls == "MEM-REALLOC-ZERO"]
            self.assertFalse(other, msg=stem)

    def test_cxx_lambda_dangle(self):
        hits = [f for f in run_lints([TD / "lambda_dangle.cpp"], TD)
                if f.cls == "CXX-LAMBDA-DANGLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dangle_bad", names)
        self.assertNotIn("dangle_ok", names)
        self.assertNotIn("dangle_ref_ok", names)
        for stem in ("dangling_ref", "use_after_move"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-LAMBDA-DANGLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LAMBDA-DANGLE"]
        self.assertFalse(c)

    def test_cxx_unique_reset(self):
        hits = [f for f in run_lints([TD / "unique_reset.cpp"], TD)
                if f.cls == "CXX-UNIQUE-RESET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unique_reset_bad", names)
        self.assertNotIn("unique_reset_ok", names)
        self.assertNotIn("unique_reset_no_raw", names)
        for stem in ("use_after_move", "cxx_newdel"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNIQUE-RESET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNIQUE-RESET"]
        self.assertFalse(c)

    def test_cxx_const_cast(self):
        hits = [f for f in run_lints([TD / "const_cast.cpp"], TD)
                if f.cls == "CXX-CONST-CAST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cxx_cv_write_bad", names)
        self.assertNotIn("cxx_cv_write_ok", names)
        c = [f for f in run_lints([TD / "const_local.c"], TD)
             if f.cls == "CXX-CONST-CAST"]
        self.assertFalse(c)

    def test_cxx_dynamic_cast_null(self):
        hits = [f for f in run_lints([TD / "dyn_cast_null.cpp"], TD)
                if f.cls == "CXX-DYNAMIC-CAST-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dyn_null_bad", names)
        self.assertNotIn("dyn_null_ok", names)
        for stem in ("virtual_ctor", "virtual_dtor", "cxx_newdel"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-DYNAMIC-CAST-NULL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DYNAMIC-CAST-NULL"]
        self.assertFalse(c)

    def test_cxx_reinterpret(self):
        hits = [f for f in run_lints([TD / "reinterp.cpp"], TD)
                if f.cls == "CXX-REINTERPRET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reinterp_pun_bad", names)
        self.assertNotIn("reinterp_void_ok", names)
        for stem in ("const_cast",):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-REINTERPRET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REINTERPRET"]
        self.assertFalse(c)

    def test_cxx_throw_copy(self):
        hits = [f for f in run_lints([TD / "throw_copy.cpp"], TD)
                if f.cls == "CXX-THROW-COPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("throw_copy_bad", names)
        self.assertNotIn("throw_copy_ok", names)
        for stem in ("throw_dtor", "catch_value", "exception_leak"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-THROW-COPY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-THROW-COPY"]
        self.assertFalse(c)

    def test_cxx_bit_cast(self):
        hits = [f for f in run_lints([TD / "bit_cast_lint.cpp"], TD)
                if f.cls == "CXX-BIT-CAST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bitcast_pun_bad", names)
        self.assertNotIn("bitcast_int_ok", names)
        for stem in ("reinterp", "const_cast"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-BIT-CAST"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BIT-CAST"]
        self.assertFalse(c)

    def test_cxx_placement_new(self):
        hits = [f for f in run_lints([TD / "placement_new.cpp"], TD)
                if f.cls == "CXX-PLACEMENT-NEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("place_new_bad", names)
        self.assertNotIn("place_new_ok", names)
        for stem in ("cxx_newdel",):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-PLACEMENT-NEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PLACEMENT-NEW"]
        self.assertFalse(c)

    def test_cxx_std_thread(self):
        hits = [f for f in run_lints([TD / "std_thread.cpp"], TD)
                if f.cls == "CXX-STD-THREAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("thread_dtor_bad", names)
        self.assertNotIn("thread_join_ok", names)
        other = [f for f in run_lints([TD / "jthread_lint.cpp"], TD)
                 if f.cls == "CXX-STD-THREAD"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-THREAD"]
        self.assertFalse(c)

    def test_cxx_optional_null(self):
        hits = [f for f in run_lints([TD / "optional_null.cpp"], TD)
                if f.cls == "CXX-OPTIONAL-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("opt_star_bad", names)
        self.assertNotIn("opt_star_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-OPTIONAL-NULL"]
        self.assertFalse(c)

    def test_cxx_variant_get(self):
        hits = [f for f in run_lints([TD / "variant_get.cpp"], TD)
                if f.cls == "CXX-VARIANT-GET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("variant_get_bad", names)
        self.assertNotIn("variant_get_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VARIANT-GET"]
        self.assertFalse(c)

    def test_cxx_span_dangle(self):
        hits = [f for f in run_lints([TD / "span_dangle.cpp"], TD)
                if f.cls == "CXX-SPAN-DANGLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("span_dangle_bad", names)
        self.assertNotIn("span_ok", names)
        dangling = (TD / "dangling_ref.cpp").read_text(encoding="utf-8")
        if "span" not in dangling:
            other = [f for f in run_lints([TD / "dangling_ref.cpp"], TD)
                     if f.cls == "CXX-SPAN-DANGLE"]
            self.assertFalse(other)
        md = [f for f in run_lints([TD / "mdspan_dangle.cpp"], TD)
              if f.cls == "CXX-SPAN-DANGLE"]
        self.assertFalse(md)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SPAN-DANGLE"]
        self.assertFalse(c)

    def test_cxx_vector_index(self):
        hits = [f for f in run_lints([TD / "vec_index.cpp"], TD)
                if f.cls == "CXX-VECTOR-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vec_index_bad", names)
        self.assertNotIn("vec_index_ok", names)
        dangling = (TD / "dangling_ref.cpp").read_text(encoding="utf-8")
        if "vector<" not in dangling:
            other = [f for f in run_lints([TD / "dangling_ref.cpp"], TD)
                     if f.cls == "CXX-VECTOR-INDEX"]
            self.assertFalse(other)
        for stem in ("iter_invalid", "span_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-VECTOR-INDEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VECTOR-INDEX"]
        self.assertFalse(c)

    def test_cxx_catch_all(self):
        hits = [f for f in run_lints([TD / "catch_all.cpp"], TD)
                if f.cls == "CXX-CATCH-ALL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("catch_all_bad", names)
        self.assertNotIn("catch_all_ok", names)
        for stem in ("try_catch", "catch_value"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-CATCH-ALL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CATCH-ALL"]
        self.assertFalse(c)

    def test_cxx_throw_new(self):
        hits = [f for f in run_lints([TD / "throw_new.cpp"], TD)
                if f.cls == "CXX-THROW-NEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("throw_new_bad", names)
        self.assertNotIn("throw_new_ok", names)
        for stem in ("throw_copy", "catch_value", "exception_leak"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-THROW-NEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-THROW-NEW"]
        self.assertFalse(c)

    def test_cxx_uninit_member(self):
        hits = [f for f in run_lints([TD / "uninit_member.cpp"], TD)
                if f.cls == "CXX-UNINIT-MEMBER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uninit_mem_bad", names)
        self.assertNotIn("uninit_mem_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNINIT-MEMBER"]
        self.assertFalse(c)

    def test_cxx_copy_assign_ptr(self):
        hits = [f for f in run_lints([TD / "copy_assign.cpp"], TD)
                if f.cls == "CXX-COPY-ASSIGN-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("copy_assign_bad", names)
        self.assertNotIn("copy_assign_ok", names)
        ok = [f for f in run_lints([TD / "self_assign.cpp"], TD)
              if f.cls == "CXX-COPY-ASSIGN-PTR"]
        self.assertNotIn("assign_ok", {f.function for f in ok})
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-COPY-ASSIGN-PTR"]
        self.assertFalse(c)

    def test_cxx_volatile_cast(self):
        hits = [f for f in run_lints([TD / "vol_cast.cpp"], TD)
                if f.cls == "CXX-VOLATILE-CAST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vol_cast_bad", names)
        self.assertNotIn("vol_cast_ok", names)
        other = [f for f in run_lints([TD / "const_cast.cpp"], TD)
                 if f.cls == "CXX-VOLATILE-CAST"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VOLATILE-CAST"]
        self.assertFalse(c)

    def test_cxx_shared_get(self):
        hits = [f for f in run_lints([TD / "shared_get.cpp"], TD)
                if f.cls == "CXX-SHARED-PTR-GET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shared_get_bad", names)
        self.assertNotIn("shared_get_ok", names)
        for stem in ("unique_reset", "use_after_move", "cxx_newdel"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SHARED-PTR-GET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SHARED-PTR-GET"]
        self.assertFalse(c)

    def test_cxx_auto_ptr(self):
        hits = [f for f in run_lints([TD / "auto_ptr.cpp"], TD)
                if f.cls == "CXX-AUTO-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("auto_ptr_bad", names)
        self.assertNotIn("auto_ptr_ok", names)
        other = [f for f in run_lints([TD / "unique_reset.cpp"], TD)
                 if f.cls == "CXX-AUTO-PTR"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-AUTO-PTR"]
        self.assertFalse(c)

    def test_cxx_string_data(self):
        hits = [f for f in run_lints([TD / "string_data.cpp"], TD)
                if f.cls == "CXX-STRING-DATA"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("string_data_bad", names)
        self.assertNotIn("string_data_ok", names)
        for stem in ("dangling_ref", "iter_invalid", "vec_index"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STRING-DATA"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STRING-DATA"]
        self.assertFalse(c)

    def test_cxx_enable_shared(self):
        hits = [f for f in run_lints([TD / "enable_shared.cpp"], TD)
                if f.cls == "CXX-ENABLE-SHARED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("enable_shared_bad", names)
        self.assertNotIn("enable_shared_ok", names)
        for stem in ("unique_reset", "copy_assign", "shared_get"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ENABLE-SHARED"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ENABLE-SHARED"]
        self.assertFalse(c)

    def test_cxx_fwd_ref(self):
        hits = [f for f in run_lints([TD / "fwd_ref.cpp"], TD)
                if f.cls == "CXX-FORWARDING-REF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fwd_bad", names)
        self.assertNotIn("fwd_ok", names)
        for stem in ("iter_invalid", "use_after_move", "vec_index"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FORWARDING-REF"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FORWARDING-REF"]
        self.assertFalse(c)

    def test_cxx_explicit_ctor(self):
        hits = [f for f in run_lints([TD / "explicit_bool.cpp"], TD)
                if f.cls == "CXX-EXPLICIT-CTOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("explicit_bool_bad", names)
        self.assertNotIn("explicit_bool_ok", names)
        for stem in ("optional_null", "virtual_dtor", "slicing"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-EXPLICIT-CTOR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXPLICIT-CTOR"]
        self.assertFalse(c)

    def test_cxx_move_const(self):
        hits = [f for f in run_lints([TD / "move_const.cpp"], TD)
                if f.cls == "CXX-MOVE-CONST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("move_const_bad", names)
        self.assertNotIn("move_const_ok", names)
        for stem in ("use_after_move", "fwd_ref"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MOVE-CONST"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MOVE-CONST"]
        self.assertFalse(c)

    def test_cxx_bind_tmp(self):
        hits = [f for f in run_lints([TD / "bind_tmp.cpp"], TD)
                if f.cls == "CXX-BIND-TMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bind_tmp_bad", names)
        self.assertNotIn("bind_tmp_ok", names)
        for stem in ("dangling_ref", "span_dangle", "lambda_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-BIND-TMP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BIND-TMP"]
        self.assertFalse(c)

    def test_cxx_expected_null(self):
        hits = [f for f in run_lints([TD / "expected_null.cpp"], TD)
                if f.cls == "CXX-EXPECTED-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("expected_null_bad", names)
        self.assertNotIn("expected_null_ok", names)
        self.assertNotIn("expected_unenc_bad", names)
        for stem in ("optional_null", "expected"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-EXPECTED-NULL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXPECTED-NULL"]
        self.assertFalse(c)

    def test_cxx_std_format(self):
        hits = [f for f in run_lints([TD / "std_format.cpp"], TD)
                if f.cls == "CXX-STD-FORMAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("format_bad", names)
        self.assertNotIn("format_ok", names)
        self.assertNotIn("format_unenc_bad", names)
        other = [f for f in run_lints([TD / "format.c"], TD)
                 if f.cls == "CXX-STD-FORMAT"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-FORMAT"]
        self.assertFalse(c)

    def test_cxx_this_capture(self):
        hits = [f for f in run_lints([TD / "this_capture.cpp"], TD)
                if f.cls == "CXX-THIS-CAPTURE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("this_capture_bad", names)
        self.assertNotIn("this_capture_ok", names)
        other = [f for f in run_lints([TD / "lambda_dangle.cpp"], TD)
                 if f.cls == "CXX-THIS-CAPTURE"]
        self.assertFalse(other)
        lam = [f for f in run_lints([TD / "lambda_dangle.cpp"], TD)
               if f.cls == "CXX-LAMBDA-DANGLE"]
        self.assertTrue(lam)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-THIS-CAPTURE"]
        self.assertFalse(c)

    def test_cxx_spaceship_default(self):
        hits = [f for f in run_lints([TD / "spaceship_ptr.cpp"], TD)
                if f.cls == "CXX-SPACESHIP-DEFAULT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("spaceship_ptr_bad", names)
        self.assertNotIn("spaceship_ptr_ok", names)
        self.assertNotIn("spaceship_user_ok", names)
        self.assertNotIn("spaceship_unenc_bad", names)
        for stem in ("copy_assign", "spaceship"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SPACESHIP-DEFAULT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SPACESHIP-DEFAULT"]
        self.assertFalse(c)

    def test_cxx_std_async(self):
        hits = [f for f in run_lints([TD / "cxx_async_lint.cpp"], TD)
                if f.cls == "CXX-STD-ASYNC"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("async_bad", names)
        self.assertNotIn("async_ok", names)
        self.assertNotIn("async_unenc_bad", names)
        for stem in ("cxx_async", "future_get"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STD-ASYNC"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-ASYNC"]
        self.assertFalse(c)

    def test_cxx_future_get(self):
        hits = [f for f in run_lints([TD / "future_get.cpp"], TD)
                if f.cls == "CXX-FUTURE-GET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("future_get_bad", names)
        self.assertNotIn("future_get_ok", names)
        for stem in ("cxx_async_lint", "variant_get"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FUTURE-GET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FUTURE-GET"]
        self.assertFalse(c)

    def test_cxx_function_null(self):
        hits = [f for f in run_lints([TD / "function_null.cpp"], TD)
                if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("function_null_bad", names)
        self.assertNotIn("function_null_ok", names)
        self.assertNotIn("function_unenc_bad", names)
        for stem in ("cxx_function", "optional_null"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STD-FUNCTION-NULL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertFalse(c)

    def test_cxx_nodiscard(self):
        hits = [f for f in run_lints([TD / "nodiscard.cpp"], TD)
                if f.cls == "CXX-NODISCARD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nodiscard_bad", names)
        self.assertNotIn("nodiscard_ok", names)
        other = [f for f in run_lints([TD / "use_after_move.cpp"], TD)
                 if f.cls == "CXX-NODISCARD"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-NODISCARD"]
        self.assertFalse(c)

    def test_cxx_std_jthread(self):
        hits = [f for f in run_lints([TD / "jthread_lint.cpp"], TD)
                if f.cls == "CXX-STD-JTHREAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("jthread_bad", names)
        self.assertNotIn("jthread_ok", names)
        self.assertNotIn("jthread_unenc_bad", names)
        for stem in ("jthread", "std_thread"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STD-JTHREAD"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-JTHREAD"]
        self.assertFalse(c)

    def test_cxx_mdspan_dangle(self):
        hits = [f for f in run_lints([TD / "mdspan_dangle.cpp"], TD)
                if f.cls == "CXX-MDSPAN-DANGLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mdspan_bad", names)
        self.assertNotIn("mdspan_ok", names)
        self.assertNotIn("mdspan_unenc_bad", names)
        span = [f for f in run_lints([TD / "mdspan_dangle.cpp"], TD)
                if f.cls == "CXX-SPAN-DANGLE"]
        self.assertFalse(span)
        for stem in ("mdspan", "span_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MDSPAN-DANGLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MDSPAN-DANGLE"]
        self.assertFalse(c)

    def test_cxx_atomic_ref(self):
        hits = [f for f in run_lints([TD / "atomic_ref_lint.cpp"], TD)
                if f.cls == "CXX-ATOMIC-REF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("atomic_ref_bad", names)
        self.assertNotIn("atomic_ref_ok", names)
        self.assertNotIn("atomic_ref_unenc_bad", names)
        for stem in ("atomic_ref", "unique_reset", "mdspan_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ATOMIC-REF"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ATOMIC-REF"]
        self.assertFalse(c)

    def test_cxx_condition_wait(self):
        hits = [f for f in run_lints([TD / "cond_wait.cpp"], TD)
                if f.cls == "CXX-CONDITION-WAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cond_wait_bad", names)
        self.assertNotIn("cond_wait_ok", names)
        self.assertNotIn("condvar_unenc_bad", names)
        for stem in ("condvar", "std_thread", "cxx_mutex"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-CONDITION-WAIT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CONDITION-WAIT"]
        self.assertFalse(c)

    def test_cxx_std_bind(self):
        hits = [f for f in run_lints([TD / "std_bind.cpp"], TD)
                if f.cls == "CXX-STD-BIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("std_bind_bad", names)
        self.assertNotIn("std_bind_ok", names)
        self.assertNotIn("bind_add", names)
        for stem in ("bind_tmp", "std_thread", "this_capture"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STD-BIND"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STD-BIND"]
        self.assertFalse(c)
        posix = [f for f in run_lints([TD / "bind_api.c"], TD)
                 if f.cls == "CXX-STD-BIND"]
        self.assertFalse(posix)

    def test_cxx_assume(self):
        hits = [f for f in run_lints([TD / "assume_lint.cpp"], TD)
                if f.cls == "CXX-ASSUME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("assume_bad", names)
        self.assertNotIn("assume_ok", names)
        self.assertNotIn("assume_unenc_bad", names)
        for stem in ("cxx_assume", "nodiscard"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ASSUME"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ASSUME"]
        self.assertFalse(c)

    def test_cxx_generator(self):
        hits = [f for f in run_lints([TD / "generator_lint.cpp"], TD)
                if f.cls == "CXX-GENERATOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("generator_bad", names)
        self.assertNotIn("generator_ok", names)
        self.assertNotIn("generator_unenc_bad", names)
        for stem in ("cxx_generator", "coroutine", "mdspan_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-GENERATOR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-GENERATOR"]
        self.assertFalse(c)

    def test_cxx_shared_mutex(self):
        hits = [f for f in run_lints([TD / "shared_mutex.cpp"], TD)
                if f.cls == "CXX-SHARED-MUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("shared_mutex_bad", names)
        self.assertNotIn("shared_mutex_ok", names)
        for stem in ("cxx_mutex", "std_thread", "unique_reset"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SHARED-MUTEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SHARED-MUTEX"]
        self.assertFalse(c)

    def test_cxx_any_cast(self):
        hits = [f for f in run_lints([TD / "any_cast_lint.cpp"], TD)
                if f.cls == "CXX-ANY-CAST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("any_cast_bad", names)
        self.assertNotIn("any_cast_ok", names)
        self.assertNotIn("any_unenc_bad", names)
        vg = [f for f in run_lints([TD / "any_cast_lint.cpp"], TD)
              if f.cls == "CXX-VARIANT-GET"]
        self.assertFalse(vg)
        for stem in ("cxx_any", "variant_get", "optional_null"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ANY-CAST"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ANY-CAST"]
        self.assertFalse(c)

    def test_cxx_filesystem(self):
        hits = [f for f in run_lints([TD / "fs_remove.cpp"], TD)
                if f.cls == "CXX-FILESYSTEM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fs_remove_bad", names)
        self.assertNotIn("fs_remove_ok", names)
        self.assertNotIn("fs_unenc_bad", names)
        for stem in ("cxx_fs", "unique_reset", "string_data"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FILESYSTEM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FILESYSTEM"]
        self.assertFalse(c)

    def test_cxx_regex(self):
        hits = [f for f in run_lints([TD / "regex_lint.cpp"], TD)
                if f.cls == "CXX-REGEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("regex_bad", names)
        self.assertNotIn("regex_ok", names)
        self.assertNotIn("regex_unenc_bad", names)
        for stem in ("cxx_regex", "std_format", "cxx_format"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-REGEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REGEX"]
        self.assertFalse(c)

    def test_cxx_latch(self):
        hits = [f for f in run_lints([TD / "latch_lint.cpp"], TD)
                if f.cls == "CXX-LATCH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("latch_bad", names)
        self.assertNotIn("latch_ok", names)
        self.assertNotIn("latch_unenc_bad", names)
        for stem in ("cxx_latch", "cond_wait", "shared_mutex"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-LATCH"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LATCH"]
        self.assertFalse(c)

    def test_cxx_from_chars(self):
        hits = [f for f in run_lints([TD / "from_chars_lint.cpp"], TD)
                if f.cls == "CXX-FROM-CHARS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("from_chars_bad", names)
        self.assertNotIn("from_chars_ok", names)
        self.assertNotIn("from_chars_unenc_bad", names)
        for stem in ("cxx_from_chars", "expected_null", "optional_null"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FROM-CHARS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FROM-CHARS"]
        self.assertFalse(c)

    def test_cxx_init_list_dangle(self):
        hits = [f for f in run_lints([TD / "init_list.cpp"], TD)
                if f.cls == "CXX-INIT-LIST-DANGLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("init_list_bad", names)
        self.assertNotIn("init_list_ok", names)
        self.assertNotIn("visit_unenc_bad", names)
        vg = [f for f in run_lints([TD / "init_list.cpp"], TD)
              if f.cls == "CXX-VARIANT-GET"]
        self.assertFalse(vg)
        for stem in ("cxx_visit", "span_dangle", "bind_tmp", "mdspan_dangle"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-INIT-LIST-DANGLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-INIT-LIST-DANGLE"]
        self.assertFalse(c)

    def test_cxx_stop_token(self):
        hits = [f for f in run_lints([TD / "stop_token_lint.cpp"], TD)
                if f.cls == "CXX-STOP-TOKEN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stop_token_bad", names)
        self.assertNotIn("stop_token_ok", names)
        self.assertNotIn("stop_token_unenc_bad", names)
        for stem in ("jthread_lint", "jthread", "std_thread"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STOP-TOKEN"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STOP-TOKEN"]
        self.assertFalse(c)

    def test_cxx_flat_map(self):
        hits = [f for f in run_lints([TD / "flat_map_lint.cpp"], TD)
                if f.cls == "CXX-FLAT-MAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flat_map_bad", names)
        self.assertNotIn("flat_map_ok", names)
        self.assertNotIn("flat_map_unenc_bad", names)
        for stem in ("vec_index", "optional_null", "variant_get"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FLAT-MAP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FLAT-MAP"]
        self.assertFalse(c)

    def test_cxx_semaphore(self):
        hits = [f for f in run_lints([TD / "semaphore_lint.cpp"], TD)
                if f.cls == "CXX-SEMAPHORE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("semaphore_bad", names)
        self.assertNotIn("semaphore_ok", names)
        latch = [f for f in run_lints([TD / "semaphore_lint.cpp"], TD)
                 if f.cls == "CXX-LATCH"]
        self.assertFalse(latch)
        for stem in ("latch_lint", "cxx_latch", "shared_mutex"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SEMAPHORE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SEMAPHORE"]
        self.assertFalse(c)

    def test_cxx_stacktrace(self):
        hits = [f for f in run_lints([TD / "stacktrace_lint.cpp"], TD)
                if f.cls == "CXX-STACKTRACE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stacktrace_bad", names)
        self.assertNotIn("stacktrace_ok", names)
        self.assertNotIn("stacktrace_unenc_bad", names)
        for stem in ("vec_index", "span_dangle", "optional_null"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-STACKTRACE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STACKTRACE"]
        self.assertFalse(c)

    def test_cxx_unique_release(self):
        hits = [f for f in run_lints([TD / "unique_release.cpp"], TD)
                if f.cls == "CXX-UNIQUE-RELEASE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unique_release_bad", names)
        self.assertNotIn("unique_release_ok", names)
        reset = [f for f in run_lints([TD / "unique_release.cpp"], TD)
                 if f.cls == "CXX-UNIQUE-RESET"]
        self.assertFalse(reset)
        other = [f for f in run_lints([TD / "unique_reset.cpp"], TD)
                 if f.cls == "CXX-UNIQUE-RELEASE"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNIQUE-RELEASE"]
        self.assertFalse(c)

    def test_cxx_pack_pragma(self):
        hits = [f for f in run_lints([TD / "pack_pragma.cpp"], TD)
                if f.cls == "CXX-PACK-PRAGMA"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pack_bad", names)
        self.assertNotIn("pack_ok", names)
        packed = [f for f in run_lints([TD / "packed.c"], TD)
                  if f.cls == "CXX-PACK-PRAGMA"]
        self.assertFalse(packed)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PACK-PRAGMA"]
        self.assertFalse(c)

    def test_cxx_function_ref(self):
        hits = [f for f in run_lints([TD / "function_ref_lint.cpp"], TD)
                if f.cls == "CXX-FUNCTION-REF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("function_ref_bad", names)
        self.assertNotIn("function_ref_ok", names)
        self.assertNotIn("function_ref_named", names)
        self.assertNotIn("fn_ref_unenc_bad", names)
        stdfn = [f for f in run_lints([TD / "function_ref_lint.cpp"], TD)
                 if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertFalse(stdfn)
        lam = [f for f in run_lints([TD / "function_ref_lint.cpp"], TD)
               if f.cls == "CXX-LAMBDA-DANGLE"]
        self.assertFalse(lam)
        other = [f for f in run_lints([TD / "cxx_fn_ref.cpp"], TD)
                 if f.cls == "CXX-FUNCTION-REF"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FUNCTION-REF"]
        self.assertFalse(c)

    def test_cxx_move_only_function(self):
        hits = [f for f in run_lints([TD / "move_only_fn.cpp"], TD)
                if f.cls == "CXX-MOVE-ONLY-FUNCTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("move_only_fn_bad", names)
        self.assertNotIn("move_only_fn_ok", names)
        stdfn = [f for f in run_lints([TD / "move_only_fn.cpp"], TD)
                 if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertFalse(stdfn)
        other = [f for f in run_lints([TD / "function_null.cpp"], TD)
                 if f.cls == "CXX-MOVE-ONLY-FUNCTION"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MOVE-ONLY-FUNCTION"]
        self.assertFalse(c)

    def test_cxx_ranges_dangle(self):
        hits = [f for f in run_lints([TD / "ranges_dangle.cpp"], TD)
                if f.cls == "CXX-RANGES-DANGLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ranges_dangle_bad", names)
        self.assertNotIn("ranges_dangle_ok", names)
        self.assertNotIn("ranges_unenc_bad", names)
        span = [f for f in run_lints([TD / "ranges_dangle.cpp"], TD)
                if f.cls in {"CXX-SPAN-DANGLE", "CXX-DANGLING-REF"}]
        self.assertFalse(span)
        other = [f for f in run_lints([TD / "cxx_ranges.cpp"], TD)
                 if f.cls == "CXX-RANGES-DANGLE"]
        self.assertFalse(other)
        for stem in ("span_dangle", "mdspan_dangle"):
            cross = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-RANGES-DANGLE"]
            self.assertFalse(cross, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RANGES-DANGLE"]
        self.assertFalse(c)

    def test_cxx_chrono_seed(self):
        hits = [f for f in run_lints([TD / "chrono_seed.cpp"], TD)
                if f.cls == "CXX-CHRONO-SEED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chrono_seed_bad", names)
        self.assertNotIn("chrono_seed_ok", names)
        self.assertNotIn("chrono_unenc_bad", names)
        srand = [f for f in run_lints([TD / "chrono_seed.cpp"], TD)
                 if f.cls == "CRYPTO-SRAND"]
        self.assertFalse(srand)
        other = [f for f in run_lints([TD / "cxx_chrono.cpp"], TD)
                 if f.cls == "CXX-CHRONO-SEED"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CHRONO-SEED"]
        self.assertFalse(c)

    def test_cxx_inplace_vector(self):
        hits = [f for f in run_lints([TD / "inplace_vec.cpp"], TD)
                if f.cls == "CXX-INPLACE-VECTOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("inplace_vec_bad", names)
        self.assertNotIn("inplace_vec_ok", names)
        other = [f for f in run_lints([TD / "vec_index.cpp"], TD)
                 if f.cls == "CXX-INPLACE-VECTOR"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-INPLACE-VECTOR"]
        self.assertFalse(c)

    def test_cxx_flat_set(self):
        hits = [f for f in run_lints([TD / "flat_set_lint.cpp"], TD)
                if f.cls == "CXX-FLAT-SET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flat_set_bad", names)
        self.assertNotIn("flat_set_ok", names)
        self.assertNotIn("flat_set_unenc_bad", names)
        fmap = [f for f in run_lints([TD / "flat_set_lint.cpp"], TD)
                if f.cls == "CXX-FLAT-MAP"]
        self.assertFalse(fmap)
        other = [f for f in run_lints([TD / "cxx_flat_set.cpp"], TD)
                 if f.cls == "CXX-FLAT-SET"]
        self.assertFalse(other)
        cross = [f for f in run_lints([TD / "flat_map_lint.cpp"], TD)
                 if f.cls == "CXX-FLAT-SET"]
        self.assertFalse(cross)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FLAT-SET"]
        self.assertFalse(c)

    def test_cxx_copyable_function(self):
        hits = [f for f in run_lints([TD / "copyable_fn.cpp"], TD)
                if f.cls == "CXX-COPYABLE-FUNCTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("copyable_fn_bad", names)
        self.assertNotIn("copyable_fn_ok", names)
        stdfn = [f for f in run_lints([TD / "copyable_fn.cpp"], TD)
                 if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertFalse(stdfn)
        mof = [f for f in run_lints([TD / "copyable_fn.cpp"], TD)
               if f.cls == "CXX-MOVE-ONLY-FUNCTION"]
        self.assertFalse(mof)
        other = [f for f in run_lints([TD / "function_null.cpp"], TD)
                 if f.cls == "CXX-COPYABLE-FUNCTION"]
        self.assertFalse(other)
        mof_file = [f for f in run_lints([TD / "move_only_fn.cpp"], TD)
                    if f.cls == "CXX-COPYABLE-FUNCTION"]
        self.assertFalse(mof_file)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-COPYABLE-FUNCTION"]
        self.assertFalse(c)

    def test_cxx_hive(self):
        hits = [f for f in run_lints([TD / "hive_lint.cpp"], TD)
                if f.cls == "CXX-HIVE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("hive_bad", names)
        self.assertNotIn("hive_ok", names)
        self.assertNotIn("hive_unenc_bad", names)
        itinv = [f for f in run_lints([TD / "hive_lint.cpp"], TD)
                 if f.cls == "CXX-ITERATOR-INVALID"]
        self.assertFalse(itinv)
        uam = [f for f in run_lints([TD / "hive_lint.cpp"], TD)
               if f.cls == "CXX-USE-AFTER-MOVE"]
        self.assertFalse(uam)
        other = [f for f in run_lints([TD / "cxx_hive.cpp"], TD)
                 if f.cls == "CXX-HIVE"]
        self.assertFalse(other)
        cross = [f for f in run_lints([TD / "iter_invalid.cpp"], TD)
                 if f.cls == "CXX-HIVE"]
        self.assertFalse(cross)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-HIVE"]
        self.assertFalse(c)

    def test_cxx_bitset_index(self):
        hits = [f for f in run_lints([TD / "bitset_lint.cpp"], TD)
                if f.cls == "CXX-BITSET-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bitset_bad", names)
        self.assertNotIn("bitset_ok", names)
        self.assertNotIn("bitset_unenc_bad", names)
        vec = [f for f in run_lints([TD / "bitset_lint.cpp"], TD)
               if f.cls == "CXX-VECTOR-INDEX"]
        self.assertFalse(vec)
        other = [f for f in run_lints([TD / "cxx_bitset.cpp"], TD)
                 if f.cls == "CXX-BITSET-INDEX"]
        self.assertFalse(other)
        cross = [f for f in run_lints([TD / "vec_index.cpp"], TD)
                 if f.cls == "CXX-BITSET-INDEX"]
        self.assertFalse(cross)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BITSET-INDEX"]
        self.assertFalse(c)

    def test_cxx_sstream_view(self):
        hits = [f for f in run_lints([TD / "sstream_view.cpp"], TD)
                if f.cls == "CXX-SSTREAM-VIEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sstream_view_bad", names)
        self.assertNotIn("sstream_view_ok", names)
        sdata = [f for f in run_lints([TD / "sstream_view.cpp"], TD)
                 if f.cls == "CXX-STRING-DATA"]
        self.assertFalse(sdata)
        other = [f for f in run_lints([TD / "string_data.cpp"], TD)
                 if f.cls == "CXX-SSTREAM-VIEW"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SSTREAM-VIEW"]
        self.assertFalse(c)

    def test_cxx_optional_value(self):
        hits = [f for f in run_lints([TD / "optional_value.cpp"], TD)
                if f.cls == "CXX-OPTIONAL-VALUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("optional_value_bad", names)
        self.assertNotIn("optional_value_ok", names)
        optnull = [f for f in run_lints([TD / "optional_value.cpp"], TD)
                   if f.cls == "CXX-OPTIONAL-NULL"]
        self.assertFalse(optnull)
        other = [f for f in run_lints([TD / "optional_null.cpp"], TD)
                 if f.cls == "CXX-OPTIONAL-VALUE"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-OPTIONAL-VALUE"]
        self.assertFalse(c)

    def test_cxx_indirect(self):
        hits = [f for f in run_lints([TD / "indirect_lint.cpp"], TD)
                if f.cls == "CXX-INDIRECT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("indirect_bad", names)
        self.assertNotIn("indirect_ok", names)
        self.assertNotIn("indirect_unenc_bad", names)
        uam = [f for f in run_lints([TD / "indirect_lint.cpp"], TD)
               if f.cls == "CXX-USE-AFTER-MOVE"]
        self.assertFalse(uam)
        other = [f for f in run_lints([TD / "cxx_indirect.cpp"], TD)
                 if f.cls == "CXX-INDIRECT"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-INDIRECT"]
        self.assertFalse(c)

    def test_cxx_to_chars(self):
        hits = [f for f in run_lints([TD / "to_chars_lint.cpp"], TD)
                if f.cls == "CXX-TO-CHARS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("to_chars_bad", names)
        self.assertNotIn("to_chars_ok", names)
        self.assertNotIn("to_chars_unenc_bad", names)
        fromc = [f for f in run_lints([TD / "to_chars_lint.cpp"], TD)
                 if f.cls == "CXX-FROM-CHARS"]
        self.assertFalse(fromc)
        other = [f for f in run_lints([TD / "cxx_to_chars.cpp"], TD)
                 if f.cls == "CXX-TO-CHARS"]
        self.assertFalse(other)
        cross = [f for f in run_lints([TD / "from_chars_lint.cpp"], TD)
                 if f.cls == "CXX-TO-CHARS"]
        self.assertFalse(cross)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TO-CHARS"]
        self.assertFalse(c)

    def test_cxx_hazard_pointer(self):
        hits = [f for f in run_lints([TD / "hazard_lint.cpp"], TD)
                if f.cls == "CXX-HAZARD-POINTER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("hazard_bad", names)
        self.assertNotIn("hazard_ok", names)
        self.assertNotIn("hazard_unenc_bad", names)
        other = [f for f in run_lints([TD / "cxx_hazard.cpp"], TD)
                 if f.cls == "CXX-HAZARD-POINTER"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-HAZARD-POINTER"]
        self.assertFalse(c)

    def test_cxx_text_encoding(self):
        hits = [f for f in run_lints([TD / "text_enc_lint.cpp"], TD)
                if f.cls == "CXX-TEXT-ENCODING"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("text_enc_bad", names)
        self.assertNotIn("text_enc_ok", names)
        self.assertNotIn("text_enc_unenc_bad", names)
        other = [f for f in run_lints([TD / "cxx_text_enc.cpp"], TD)
                 if f.cls == "CXX-TEXT-ENCODING"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TEXT-ENCODING"]
        self.assertFalse(c)

    def test_cxx_expected_error(self):
        hits = [f for f in run_lints([TD / "expected_error.cpp"], TD)
                if f.cls == "CXX-EXPECTED-ERROR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("expected_error_bad", names)
        self.assertNotIn("expected_error_ok", names)
        enull = [f for f in run_lints([TD / "expected_error.cpp"], TD)
                 if f.cls == "CXX-EXPECTED-NULL"]
        self.assertFalse(enull)
        other = [f for f in run_lints([TD / "expected_null.cpp"], TD)
                 if f.cls == "CXX-EXPECTED-ERROR"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXPECTED-ERROR"]
        self.assertFalse(c)

    def test_cxx_variant_valueless(self):
        hits = [f for f in run_lints([TD / "variant_valueless.cpp"], TD)
                if f.cls == "CXX-VARIANT-VALUELSS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("variant_valueless_bad", names)
        self.assertNotIn("variant_valueless_ok", names)
        vg = [f for f in run_lints([TD / "variant_valueless.cpp"], TD)
              if f.cls == "CXX-VARIANT-GET"]
        self.assertFalse(vg)
        other = [f for f in run_lints([TD / "variant_get.cpp"], TD)
                 if f.cls == "CXX-VARIANT-VALUELSS"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VARIANT-VALUELSS"]
        self.assertFalse(c)

    def test_cxx_simd_index(self):
        hits = [f for f in run_lints([TD / "simd_index.cpp"], TD)
                if f.cls == "CXX-SIMD-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("simd_index_bad", names)
        self.assertNotIn("simd_index_ok", names)
        self.assertNotIn("simd_unenc_bad", names)
        vec = [f for f in run_lints([TD / "simd_index.cpp"], TD)
               if f.cls == "CXX-VECTOR-INDEX"]
        self.assertFalse(vec)
        other = [f for f in run_lints([TD / "cxx_simd.cpp"], TD)
                 if f.cls == "CXX-SIMD-INDEX"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SIMD-INDEX"]
        self.assertFalse(c)

    def test_cxx_rcu(self):
        hits = [f for f in run_lints([TD / "rcu_lint.cpp"], TD)
                if f.cls == "CXX-RCU"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rcu_bad", names)
        self.assertNotIn("rcu_ok", names)
        self.assertNotIn("rcu_unenc_bad", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RCU"]
        self.assertFalse(c)

    def test_cxx_linalg(self):
        hits = [f for f in run_lints([TD / "linalg_lint.cpp"], TD)
                if f.cls == "CXX-LINALG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("linalg_bad", names)
        self.assertNotIn("linalg_ok", names)
        self.assertNotIn("linalg_unenc_bad", names)
        md = [f for f in run_lints([TD / "linalg_lint.cpp"], TD)
              if f.cls == "CXX-MDSPAN-DANGLE"]
        self.assertFalse(md)
        simd = [f for f in run_lints([TD / "linalg_lint.cpp"], TD)
                if f.cls == "CXX-SIMD-INDEX"]
        self.assertFalse(simd)
        other = [f for f in run_lints([TD / "mdspan_dangle.cpp"], TD)
                 if f.cls == "CXX-LINALG"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LINALG"]
        self.assertFalse(c)

    def test_cxx_sync_wait(self):
        hits = [f for f in run_lints([TD / "sync_wait.cpp"], TD)
                if f.cls == "CXX-SYNC-WAIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("sync_wait_bad", names)
        self.assertNotIn("sync_wait_ok", names)
        async_hits = [f for f in run_lints([TD / "sync_wait.cpp"], TD)
                      if f.cls == "CXX-STD-ASYNC"]
        self.assertFalse(async_hits)
        other = [f for f in run_lints([TD / "cxx_async_lint.cpp"], TD)
                 if f.cls == "CXX-SYNC-WAIT"]
        self.assertFalse(other)
        execu = [f for f in run_lints([TD / "cxx_execution.cpp"], TD)
                 if f.cls == "CXX-SYNC-WAIT"]
        self.assertFalse(execu)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SYNC-WAIT"]
        self.assertFalse(c)

    def test_cxx_embed(self):
        hits = [f for f in run_lints([TD / "embed_lint.cpp"], TD)
                if f.cls == "CXX-EMBED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("embed_bad", names)
        self.assertNotIn("embed_ok", names)
        self.assertNotIn("embed_unenc_bad", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EMBED"]
        self.assertFalse(c)

    def test_cxx_contracts(self):
        hits = [f for f in run_lints([TD / "contracts_lint.cpp"], TD)
                if f.cls == "CXX-CONTRACTS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("contracts_bad", names)
        self.assertNotIn("contracts_ok", names)
        assume = [f for f in run_lints([TD / "contracts_lint.cpp"], TD)
                  if f.cls == "CXX-ASSUME"]
        self.assertFalse(assume)
        other = [f for f in run_lints([TD / "assume_lint.cpp"], TD)
                 if f.cls == "CXX-CONTRACTS"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CONTRACTS"]
        self.assertFalse(c)

    def test_cxx_reflection(self):
        hits = [f for f in run_lints([TD / "reflection_lint.cpp"], TD)
                if f.cls == "CXX-REFLECTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reflection_bad", names)
        self.assertNotIn("reflection_ok", names)
        self.assertNotIn("import_unenc_bad", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REFLECTION"]
        self.assertFalse(c)

    def test_cxx_out_ptr(self):
        hits = [f for f in run_lints([TD / "out_ptr_lint.cpp"], TD)
                if f.cls == "CXX-OUT-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("out_ptr_bad", names)
        self.assertNotIn("out_ptr_ok", names)
        self.assertNotIn("out_ptr_unenc_bad", names)
        reset = [f for f in run_lints([TD / "out_ptr_lint.cpp"], TD)
                 if f.cls == "CXX-UNIQUE-RESET"]
        self.assertFalse(reset)
        other = [f for f in run_lints([TD / "unique_reset.cpp"], TD)
                 if f.cls == "CXX-OUT-PTR"]
        self.assertFalse(other)
        honest = [f for f in run_lints([TD / "cxx_out_ptr.cpp"], TD)
                  if f.cls == "CXX-OUT-PTR"]
        self.assertFalse(honest)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-OUT-PTR"]
        self.assertFalse(c)

    def test_cxx_flat_multimap(self):
        hits = [f for f in run_lints([TD / "flat_multimap_lint.cpp"], TD)
                if f.cls == "CXX-FLAT-MULTIMAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flat_multimap_bad", names)
        self.assertNotIn("flat_multimap_ok", names)
        self.assertNotIn("flat_mmap_unenc_bad", names)
        fmap = [f for f in run_lints([TD / "flat_multimap_lint.cpp"], TD)
                if f.cls in {"CXX-FLAT-MAP", "CXX-FLAT-SET"}]
        self.assertFalse(fmap)
        for stem in ("flat_map_lint", "flat_set_lint", "cxx_flat_mmap"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FLAT-MULTIMAP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FLAT-MULTIMAP"]
        self.assertFalse(c)

    def test_cxx_spanstream(self):
        hits = [f for f in run_lints([TD / "spanstream_lint.cpp"], TD)
                if f.cls == "CXX-SPANSTREAM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("spanstream_bad", names)
        self.assertNotIn("spanstream_ok", names)
        self.assertNotIn("spanstream_unenc_bad", names)
        ssv = [f for f in run_lints([TD / "spanstream_lint.cpp"], TD)
               if f.cls == "CXX-SSTREAM-VIEW"]
        self.assertFalse(ssv)
        other = [f for f in run_lints([TD / "sstream_view.cpp"], TD)
                 if f.cls == "CXX-SPANSTREAM"]
        self.assertFalse(other)
        honest = [f for f in run_lints([TD / "cxx_spanstream.cpp"], TD)
                  if f.cls == "CXX-SPANSTREAM"]
        self.assertFalse(honest)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SPANSTREAM"]
        self.assertFalse(c)

    def test_cxx_barrier(self):
        hits = [f for f in run_lints([TD / "barrier_lint.cpp"], TD)
                if f.cls == "CXX-BARRIER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("barrier_bad", names)
        self.assertNotIn("barrier_ok", names)
        latch = [f for f in run_lints([TD / "barrier_lint.cpp"], TD)
                 if f.cls in {"CXX-LATCH", "CXX-SEMAPHORE"}]
        self.assertFalse(latch)
        for stem in ("latch_lint", "semaphore_lint", "cxx_latch"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-BARRIER"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BARRIER"]
        self.assertFalse(c)

    def test_cxx_task(self):
        hits = [f for f in run_lints([TD / "task_lint.cpp"], TD)
                if f.cls == "CXX-TASK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("task_bad", names)
        self.assertNotIn("task_ok", names)
        self.assertNotIn("task_unenc_bad", names)
        sw = [f for f in run_lints([TD / "task_lint.cpp"], TD)
              if f.cls in {"CXX-SYNC-WAIT", "CXX-STD-ASYNC"}]
        self.assertFalse(sw)
        for stem in ("sync_wait", "cxx_async_lint", "cxx_execution", "cxx_task"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-TASK"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TASK"]
        self.assertFalse(c)

    def test_cxx_generator_discard(self):
        hits = [f for f in run_lints([TD / "generator_discard.cpp"], TD)
                if f.cls == "CXX-GENERATOR-DISCARD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("generator_discard_bad", names)
        self.assertNotIn("generator_discard_ok", names)
        gen = [f for f in run_lints([TD / "generator_discard.cpp"], TD)
               if f.cls == "CXX-GENERATOR"]
        self.assertFalse(gen)
        for stem in ("generator_lint", "cxx_generator"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-GENERATOR-DISCARD"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-GENERATOR-DISCARD"]
        self.assertFalse(c)

    def test_cxx_osyncstream(self):
        hits = [f for f in run_lints([TD / "osync_lint.cpp"], TD)
                if f.cls == "CXX-OSYNCSTREAM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("osync_bad", names)
        self.assertNotIn("osync_ok", names)
        self.assertNotIn("osync_unenc_bad", names)
        sb = [f for f in run_lints([TD / "osync_lint.cpp"], TD)
              if f.cls == "CXX-SYNCBUF"]
        self.assertFalse(sb)
        for stem in ("syncbuf_lint", "cxx_osync", "sstream_view"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-OSYNCSTREAM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-OSYNCSTREAM"]
        self.assertFalse(c)

    def test_cxx_packaged_task(self):
        hits = [f for f in run_lints([TD / "packaged_lint.cpp"], TD)
                if f.cls == "CXX-PACKAGED-TASK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("packaged_bad", names)
        self.assertNotIn("packaged_ok", names)
        self.assertNotIn("packaged_unenc_bad", names)
        fnull = [f for f in run_lints([TD / "packaged_lint.cpp"], TD)
                 if f.cls == "CXX-STD-FUNCTION-NULL"]
        self.assertFalse(fnull)
        for stem in ("function_null", "copyable_fn", "cxx_packaged"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-PACKAGED-TASK"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PACKAGED-TASK"]
        self.assertFalse(c)

    def test_cxx_flat_multiset(self):
        hits = [f for f in run_lints([TD / "flat_multiset_lint.cpp"], TD)
                if f.cls == "CXX-FLAT-MULTISET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("flat_multiset_bad", names)
        self.assertNotIn("flat_multiset_ok", names)
        self.assertNotIn("flat_mset_unenc_bad", names)
        sib = [f for f in run_lints([TD / "flat_multiset_lint.cpp"], TD)
               if f.cls in {"CXX-FLAT-SET", "CXX-FLAT-MULTIMAP"}]
        self.assertFalse(sib)
        for stem in ("flat_set_lint", "flat_multimap_lint", "cxx_flat_mset"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FLAT-MULTISET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FLAT-MULTISET"]
        self.assertFalse(c)

    def test_cxx_syncbuf(self):
        hits = [f for f in run_lints([TD / "syncbuf_lint.cpp"], TD)
                if f.cls == "CXX-SYNCBUF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("syncbuf_bad", names)
        self.assertNotIn("syncbuf_ok", names)
        self.assertNotIn("syncbuf_unenc_bad", names)
        osy = [f for f in run_lints([TD / "syncbuf_lint.cpp"], TD)
               if f.cls == "CXX-OSYNCSTREAM"]
        self.assertFalse(osy)
        for stem in ("osync_lint", "cxx_syncbuf", "sstream_view"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SYNCBUF"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SYNCBUF"]
        self.assertFalse(c)

    def test_cxx_counted_iterator(self):
        hits = [f for f in run_lints([TD / "counted_iter.cpp"], TD)
                if f.cls == "CXX-COUNTED-ITERATOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("counted_iter_bad", names)
        self.assertNotIn("counted_iter_ok", names)
        for stem in ("vec_index", "simd_index"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-COUNTED-ITERATOR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-COUNTED-ITERATOR"]
        self.assertFalse(c)

    def test_cxx_promise(self):
        hits = [f for f in run_lints([TD / "promise_lint.cpp"], TD)
                if f.cls == "CXX-PROMISE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("promise_bad", names)
        self.assertNotIn("promise_ok", names)
        fut = [f for f in run_lints([TD / "promise_lint.cpp"], TD)
               if f.cls == "CXX-FUTURE-GET"]
        self.assertFalse(fut)
        for stem in ("future_get", "cxx_async_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-PROMISE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PROMISE"]
        self.assertFalse(c)

    def test_cxx_weak_ptr(self):
        hits = [f for f in run_lints([TD / "weak_ptr_lint.cpp"], TD)
                if f.cls == "CXX-WEAK-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("weak_bad", names)
        self.assertNotIn("weak_ok", names)
        for stem in ("shared_get", "enable_shared"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-WEAK-PTR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-WEAK-PTR"]
        self.assertFalse(c)

    def test_cxx_exception_ptr(self):
        hits = [f for f in run_lints([TD / "exc_ptr_lint.cpp"], TD)
                if f.cls == "CXX-EXCEPTION-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("exc_ptr_bad", names)
        self.assertNotIn("exc_ptr_ok", names)
        for stem in ("promise_lint", "future_get"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-EXCEPTION-PTR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXCEPTION-PTR"]
        self.assertFalse(c)

    def test_cxx_coro_handle(self):
        hits = [f for f in run_lints([TD / "coro_handle_lint.cpp"], TD)
                if f.cls == "CXX-CORO-HANDLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("coro_h_bad", names)
        self.assertNotIn("coro_h_ok", names)
        other = [f for f in run_lints([TD / "coroutine.cpp"], TD)
                 if f.cls == "CXX-CORO-HANDLE"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CORO-HANDLE"]
        self.assertFalse(c)

    def test_cxx_valarray(self):
        hits = [f for f in run_lints([TD / "valarray_lint.cpp"], TD)
                if f.cls == "CXX-VALARRAY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("valarray_bad", names)
        self.assertNotIn("valarray_ok", names)
        for stem in ("vec_index", "simd_index"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-VALARRAY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VALARRAY"]
        self.assertFalse(c)

    def test_cxx_to_underlying(self):
        hits = [f for f in run_lints([TD / "to_under_lint.cpp"], TD)
                if f.cls == "CXX-TO-UNDERLYING"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("to_under_bad", names)
        self.assertNotIn("to_under_ok", names)
        other = [f for f in run_lints([TD / "cxx_casts.cpp"], TD)
                 if f.cls == "CXX-TO-UNDERLYING"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TO-UNDERLYING"]
        self.assertFalse(c)

    def test_cxx_unexpected(self):
        hits = [f for f in run_lints([TD / "unexpect_lint.cpp"], TD)
                if f.cls == "CXX-UNEXPECTED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unexpect_bad", names)
        self.assertNotIn("unexpect_ok", names)
        for stem in ("expected_error", "expected_null"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNEXPECTED"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNEXPECTED"]
        self.assertFalse(c)

    def test_cxx_tuple_get(self):
        hits = [f for f in run_lints([TD / "tuple_lint.cpp"], TD)
                if f.cls == "CXX-TUPLE-GET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tuple_bad", names)
        self.assertNotIn("tuple_ok", names)
        other = [f for f in run_lints([TD / "variant_get.cpp"], TD)
                 if f.cls == "CXX-TUPLE-GET"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TUPLE-GET"]
        self.assertFalse(c)

    def test_cxx_deque_index(self):
        hits = [f for f in run_lints([TD / "deque_lint.cpp"], TD)
                if f.cls == "CXX-DEQUE-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("deque_bad", names)
        self.assertNotIn("deque_ok", names)
        for stem in ("vec_index", "valarray_lint", "simd_index"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-DEQUE-INDEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DEQUE-INDEX"]
        self.assertFalse(c)

    def test_cxx_forward_list(self):
        hits = [f for f in run_lints([TD / "fwd_list_lint.cpp"], TD)
                if f.cls == "CXX-FORWARD-LIST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fwd_bad", names)
        self.assertNotIn("fwd_ok", names)
        for stem in ("list_front_lint", "init_list"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-FORWARD-LIST"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FORWARD-LIST"]
        self.assertFalse(c)

    def test_cxx_list_front(self):
        hits = [f for f in run_lints([TD / "list_front_lint.cpp"], TD)
                if f.cls == "CXX-LIST-FRONT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("list_front_bad", names)
        self.assertNotIn("list_front_ok", names)
        for stem in ("fwd_list_lint", "init_list"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-LIST-FRONT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LIST-FRONT"]
        self.assertFalse(c)

    def test_cxx_map_at(self):
        hits = [f for f in run_lints([TD / "map_at_lint.cpp"], TD)
                if f.cls == "CXX-MAP-AT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("map_at_bad", names)
        self.assertNotIn("map_at_ok", names)
        for stem in ("umap_at_lint", "flat_map_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MAP-AT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MAP-AT"]
        self.assertFalse(c)

    def test_cxx_unordered_at(self):
        hits = [f for f in run_lints([TD / "umap_at_lint.cpp"], TD)
                if f.cls == "CXX-UNORDERED-AT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("umap_at_bad", names)
        self.assertNotIn("umap_at_ok", names)
        for stem in ("map_at_lint", "flat_map_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNORDERED-AT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNORDERED-AT"]
        self.assertFalse(c)

    def test_cxx_set_find(self):
        hits = [f for f in run_lints([TD / "set_find_lint.cpp"], TD)
                if f.cls == "CXX-SET-FIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("set_find_bad", names)
        self.assertNotIn("set_find_ok", names)
        for stem in ("flat_set_lint", "umap_at_lint", "uset_find_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SET-FIND"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SET-FIND"]
        self.assertFalse(c)

    def test_cxx_queue_front(self):
        hits = [f for f in run_lints([TD / "queue_lint.cpp"], TD)
                if f.cls == "CXX-QUEUE-FRONT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("queue_bad", names)
        self.assertNotIn("queue_ok", names)
        other = [f for f in run_lints([TD / "pqueue_lint.cpp"], TD)
                 if f.cls == "CXX-QUEUE-FRONT"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-QUEUE-FRONT"]
        self.assertFalse(c)

    def test_cxx_stack_top(self):
        hits = [f for f in run_lints([TD / "stack_lint.cpp"], TD)
                if f.cls == "CXX-STACK-TOP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stack_bad", names)
        self.assertNotIn("stack_ok", names)
        other = [f for f in run_lints([TD / "stacktrace_lint.cpp"], TD)
                 if f.cls == "CXX-STACK-TOP"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STACK-TOP"]
        self.assertFalse(c)

    def test_cxx_priority_queue(self):
        hits = [f for f in run_lints([TD / "pqueue_lint.cpp"], TD)
                if f.cls == "CXX-PRIORITY-QUEUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pqueue_bad", names)
        self.assertNotIn("pqueue_ok", names)
        other = [f for f in run_lints([TD / "queue_lint.cpp"], TD)
                 if f.cls == "CXX-PRIORITY-QUEUE"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PRIORITY-QUEUE"]
        self.assertFalse(c)

    def test_cxx_array_index(self):
        hits = [f for f in run_lints([TD / "array_index_lint.cpp"], TD)
                if f.cls == "CXX-ARRAY-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("arr_idx_bad", names)
        self.assertNotIn("arr_idx_ok", names)
        for stem in ("vec_index", "valarray_lint", "simd_index",
                     "deque_lint", "bitset_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ARRAY-INDEX"]
            self.assertFalse(other, msg=stem)
        carr = [f for f in run_lints([TD / "unvalidated.c"], TD)
                if f.cls == "CXX-ARRAY-INDEX"]
        self.assertFalse(carr)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ARRAY-INDEX"]
        self.assertFalse(c)

    def test_cxx_unordered_set(self):
        hits = [f for f in run_lints([TD / "uset_find_lint.cpp"], TD)
                if f.cls == "CXX-UNORDERED-SET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uset_find_bad", names)
        self.assertNotIn("uset_find_ok", names)
        for stem in ("set_find_lint", "umap_at_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNORDERED-SET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNORDERED-SET"]
        self.assertFalse(c)

    def test_cxx_wstring_view(self):
        hits = [f for f in run_lints([TD / "wstring_view_lint.cpp"], TD)
                if f.cls == "CXX-WSTRING-VIEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wv_bad", names)
        self.assertNotIn("wv_ok", names)
        dangle = [f for f in run_lints([TD / "wstring_view_lint.cpp"], TD)
                  if f.cls == "CXX-DANGLING-REF"]
        self.assertFalse(dangle)
        for stem in ("dangling_ref", "ranges_dangle", "sstream_view"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-WSTRING-VIEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-WSTRING-VIEW"]
        self.assertFalse(c)

    def test_cxx_multimap_find(self):
        hits = [f for f in run_lints([TD / "multimap_lint.cpp"], TD)
                if f.cls == "CXX-MULTIMAP-FIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mmap_find_bad", names)
        self.assertNotIn("mmap_find_ok", names)
        cross = [f for f in run_lints([TD / "multimap_lint.cpp"], TD)
                 if f.cls in {"CXX-MAP-AT", "CXX-FLAT-MULTIMAP"}]
        self.assertFalse(cross)
        for stem in ("map_at_lint", "flat_multimap_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MULTIMAP-FIND"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MULTIMAP-FIND"]
        self.assertFalse(c)

    def test_cxx_multiset_find(self):
        hits = [f for f in run_lints([TD / "multiset_lint.cpp"], TD)
                if f.cls == "CXX-MULTISET-FIND"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mset_find_bad", names)
        self.assertNotIn("mset_find_ok", names)
        cross = [f for f in run_lints([TD / "multiset_lint.cpp"], TD)
                 if f.cls in {"CXX-SET-FIND", "CXX-FLAT-MULTISET"}]
        self.assertFalse(cross)
        for stem in ("set_find_lint", "flat_multiset_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-MULTISET-FIND"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MULTISET-FIND"]
        self.assertFalse(c)

    def test_cxx_binary_semaphore(self):
        hits = [f for f in run_lints([TD / "binsem_lint.cpp"], TD)
                if f.cls == "CXX-BINARY-SEMAPHORE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("binsem_bad", names)
        self.assertNotIn("binsem_ok", names)
        cross = [f for f in run_lints([TD / "binsem_lint.cpp"], TD)
                 if f.cls in {"CXX-SEMAPHORE", "CXX-LATCH"}]
        self.assertFalse(cross)
        other = [f for f in run_lints([TD / "semaphore_lint.cpp"], TD)
                 if f.cls == "CXX-BINARY-SEMAPHORE"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BINARY-SEMAPHORE"]
        self.assertFalse(c)

    def test_cxx_error_code(self):
        hits = [f for f in run_lints([TD / "error_code_lint.cpp"], TD)
                if f.cls == "CXX-ERROR-CODE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("errc_bad", names)
        self.assertNotIn("errc_ok", names)
        for stem in ("from_chars_lint", "expected_error"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-ERROR-CODE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ERROR-CODE"]
        self.assertFalse(c)

    def test_cxx_byteswap(self):
        hits = [f for f in run_lints([TD / "byteswap_lint.cpp"], TD)
                if f.cls == "CXX-BYTESWAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bswap_bad", names)
        self.assertNotIn("bswap_ok", names)
        other = [f for f in run_lints([TD / "array_index_lint.cpp"], TD)
                 if f.cls == "CXX-BYTESWAP"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BYTESWAP"]
        self.assertFalse(c)

    def test_cxx_pmr(self):
        hits = [f for f in run_lints([TD / "pmr_lint.cpp"], TD)
                if f.cls == "CXX-PMR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pmr_bad", names)
        self.assertNotIn("pmr_ok", names)
        other = [f for f in run_lints([TD / "vec_index.cpp"], TD)
                 if f.cls == "CXX-PMR"]
        self.assertFalse(other)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PMR"]
        self.assertFalse(c)

    def test_cxx_u8string_view(self):
        hits = [f for f in run_lints([TD / "u8string_lint.cpp"], TD)
                if f.cls == "CXX-U8STRING-VIEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("u8_bad", names)
        self.assertNotIn("u8_ok", names)
        dangle = [f for f in run_lints([TD / "u8string_lint.cpp"], TD)
                  if f.cls == "CXX-DANGLING-REF"]
        self.assertFalse(dangle)
        wview = [f for f in run_lints([TD / "u8string_lint.cpp"], TD)
                 if f.cls == "CXX-WSTRING-VIEW"]
        self.assertFalse(wview)
        for stem in ("dangling_ref", "wstring_view_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-U8STRING-VIEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-U8STRING-VIEW"]
        self.assertFalse(c)

    def test_cxx_unordered_multimap(self):
        hits = [f for f in run_lints([TD / "ummap_lint.cpp"], TD)
                if f.cls == "CXX-UNORDERED-MULTIMAP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ummap_find_bad", names)
        self.assertNotIn("ummap_find_ok", names)
        for stem in ("umap_at_lint", "multimap_lint", "flat_multimap_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNORDERED-MULTIMAP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNORDERED-MULTIMAP"]
        self.assertFalse(c)

    def test_cxx_unordered_multiset(self):
        hits = [f for f in run_lints([TD / "umset_lint.cpp"], TD)
                if f.cls == "CXX-UNORDERED-MULTISET"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("umset_find_bad", names)
        self.assertNotIn("umset_find_ok", names)
        for stem in ("uset_find_lint", "multiset_lint", "set_find_lint",
                     "flat_multiset_lint"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-UNORDERED-MULTISET"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNORDERED-MULTISET"]
        self.assertFalse(c)

    def test_cxx_shared_lock(self):
        hits = [f for f in run_lints([TD / "shared_lock_lint.cpp"], TD)
                if f.cls == "CXX-SHARED-LOCK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("slock_bad", names)
        self.assertNotIn("slock_ok", names)
        sm = [f for f in run_lints([TD / "shared_lock_lint.cpp"], TD)
              if f.cls == "CXX-SHARED-MUTEX"]
        self.assertFalse(sm)
        for stem in ("shared_mutex", "cxx_mutex", "cond_wait"):
            other = [f for f in run_lints([TD / f"{stem}.cpp"], TD)
                     if f.cls == "CXX-SHARED-LOCK"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SHARED-LOCK"]
        self.assertFalse(c)

    def test_cxx_atomic_flag(self):
        hits = [f for f in run_lints([TD / "atomic_flag_lint.cpp"], TD)
                if f.cls == "CXX-ATOMIC-FLAG"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("aflag_bad", names)
        self.assertNotIn("aflag_ok", names)
        aref = [f for f in run_lints([TD / "atomic_flag_lint.cpp"], TD)
                if f.cls == "CXX-ATOMIC-REF"]
        self.assertFalse(aref)
        other = [f for f in run_lints([TD / "atomic_ref_lint.cpp"], TD)
                 if f.cls == "CXX-ATOMIC-FLAG"]
        self.assertFalse(other)
        builtin = [f for f in run_lints([TD / "atomic_builtin.c"], TD)
                   if f.cls == "CXX-ATOMIC-FLAG"]
        self.assertFalse(builtin)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ATOMIC-FLAG"]
        self.assertFalse(c)

    def test_cxx_condvar_any(self):
        hits = [f for f in run_lints([TD / "cvany_lint.cpp"], TD)
                if f.cls == "CXX-CONDVAR-ANY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cvany_bad", names)
        self.assertNotIn("cvany_ok", names)
        cw = [f for f in run_lints([TD / "cvany_lint.cpp"], TD)
              if f.cls == "CXX-CONDITION-WAIT"]
        self.assertFalse(cw)
        for stem in ("cond_wait", "condvar", "cxx_mutex"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CONDVAR-ANY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CONDVAR-ANY"]
        self.assertFalse(c)

    def test_cxx_recursive_mutex(self):
        hits = [f for f in run_lints([TD / "rec_mutex_lint.cpp"], TD)
                if f.cls == "CXX-RECURSIVE-MUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rmutex_bad", names)
        self.assertNotIn("rmutex_ok", names)
        sm = [f for f in run_lints([TD / "rec_mutex_lint.cpp"], TD)
              if f.cls == "CXX-SHARED-MUTEX"]
        self.assertFalse(sm)
        for stem in ("cxx_mutex", "shared_mutex"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-RECURSIVE-MUTEX"]
            self.assertFalse(other, msg=stem)
        sl = TD / "shared_lock_lint.cpp"
        if sl.exists():
            other = [f for f in run_lints([sl], TD)
                     if f.cls == "CXX-RECURSIVE-MUTEX"]
            self.assertFalse(other, msg="shared_lock_lint")
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RECURSIVE-MUTEX"]
        self.assertFalse(c)

    def test_cxx_timed_mutex(self):
        hits = [f for f in run_lints([TD / "timed_mutex_lint.cpp"], TD)
                if f.cls == "CXX-TIMED-MUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tmutex_bad", names)
        self.assertNotIn("tmutex_ok", names)
        for stem in ("cxx_mutex", "shared_mutex", "rec_mutex_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TIMED-MUTEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TIMED-MUTEX"]
        self.assertFalse(c)

    def test_cxx_fstream(self):
        hits = [f for f in run_lints([TD / "fstream_lint.cpp"], TD)
                if f.cls == "CXX-FSTREAM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fstream_bad", names)
        self.assertNotIn("fstream_ok", names)
        for p in TD.glob("*sstream*"):
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FSTREAM"]
            self.assertFalse(other, msg=p.name)
        for stem in ("spanstream_lint", "cxx_spanstream", "osync_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FSTREAM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FSTREAM"]
        self.assertFalse(c)

    def test_cxx_this_thread(self):
        hits = [f for f in run_lints([TD / "this_thread_lint.cpp"], TD)
                if f.cls == "CXX-THIS-THREAD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tthread_bad", names)
        self.assertNotIn("tthread_ok", names)
        for pat in ("*jthread*", "*std_thread*", "*thread*"):
            for p in TD.glob(pat):
                if "this_thread" in p.name:
                    continue
                other = [f for f in run_lints([p], TD)
                         if f.cls == "CXX-THIS-THREAD"]
                self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-THIS-THREAD"]
        self.assertFalse(c)

    def test_cxx_call_once(self):
        hits = [f for f in run_lints([TD / "call_once_lint.cpp"], TD)
                if f.cls == "CXX-CALL-ONCE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("call_once_bad", names)
        self.assertNotIn("call_once_ok", names)
        for p in list(TD.glob("*pthread*")) + list(TD.glob("*once*")):
            if "call_once" in p.name:
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CALL-ONCE"]
            self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CALL-ONCE"]
        self.assertFalse(c)

    def test_cxx_shared_timed_mutex(self):
        hits = [f for f in run_lints([TD / "stmutex_lint.cpp"], TD)
                if f.cls == "CXX-SHARED-TIMED-MUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stmutex_bad", names)
        self.assertNotIn("stmutex_ok", names)
        for stem in ("timed_mutex_lint", "shared_lock_lint", "cxx_mutex"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-SHARED-TIMED-MUTEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SHARED-TIMED-MUTEX"]
        self.assertFalse(c)

    def test_cxx_recursive_timed_mutex(self):
        hits = [f for f in run_lints([TD / "rtmutex_lint.cpp"], TD)
                if f.cls == "CXX-RECURSIVE-TIMED-MUTEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rtmutex_bad", names)
        self.assertNotIn("rtmutex_ok", names)
        for stem in ("rec_mutex_lint", "timed_mutex_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-RECURSIVE-TIMED-MUTEX"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RECURSIVE-TIMED-MUTEX"]
        self.assertFalse(c)

    def test_cxx_system_error(self):
        hits = [f for f in run_lints([TD / "syserr_lint.cpp"], TD)
                if f.cls == "CXX-SYSTEM-ERROR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("syserr_bad", names)
        self.assertNotIn("syserr_ok", names)
        for stem in ("error_code_lint", "cxx_errc"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-SYSTEM-ERROR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SYSTEM-ERROR"]
        self.assertFalse(c)

    def test_cxx_chrono_tzdb(self):
        hits = [f for f in run_lints([TD / "tzdb_lint.cpp"], TD)
                if f.cls == "CXX-CHRONO-TZDB"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tzdb_bad", names)
        self.assertNotIn("tzdb_ok", names)
        for p in TD.glob("*chrono*"):
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CHRONO-TZDB"]
            self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CHRONO-TZDB"]
        self.assertFalse(c)

    def test_cxx_ranges_zip(self):
        hits = [f for f in run_lints([TD / "vzip_lint.cpp"], TD)
                if f.cls == "CXX-RANGES-ZIP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vzip_bad", names)
        self.assertNotIn("vzip_ok", names)
        for p in list(TD.glob("*view*")) + list(TD.glob("*ranges*")):
            if "vzip" in p.name or "zip" in p.name:
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-RANGES-ZIP"]
            self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RANGES-ZIP"]
        self.assertFalse(c)

    def test_cxx_format_to(self):
        hits = [f for f in run_lints([TD / "format_to_lint.cpp"], TD)
                if f.cls == "CXX-FORMAT-TO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fmtto_bad", names)
        self.assertNotIn("fmtto_ok", names)
        for p in list(TD.glob("*format*")) + list(TD.glob("*print*")):
            if "format_to" in p.name:
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FORMAT-TO"]
            self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FORMAT-TO"]
        self.assertFalse(c)

    def test_cxx_error_category(self):
        hits = [f for f in run_lints([TD / "errcat_lint.cpp"], TD)
                if f.cls == "CXX-ERROR-CATEGORY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("errcat_bad", names)
        self.assertNotIn("errcat_ok", names)
        for stem in ("error_code_lint", "syserr_lint", "cxx_errc"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ERROR-CATEGORY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ERROR-CATEGORY"]
        self.assertFalse(c)

    def test_cxx_nested_exception(self):
        hits = [f for f in run_lints([TD / "nested_lint.cpp"], TD)
                if f.cls == "CXX-NESTED-EXCEPTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nested_bad", names)
        self.assertNotIn("nested_ok", names)
        p = TD / "exc_ptr_lint.cpp"
        if p.exists():
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-NESTED-EXCEPTION"]
            self.assertFalse(other, msg="exc_ptr_lint")
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-NESTED-EXCEPTION"]
        self.assertFalse(c)

    def test_cxx_atomic_fence(self):
        hits = [f for f in run_lints([TD / "fence_lint.cpp"], TD)
                if f.cls == "CXX-ATOMIC-FENCE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fence_bad", names)
        self.assertNotIn("fence_ok", names)
        for stem, ext in (
            ("atomic_flag_lint", ".cpp"),
            ("atomic_ref_lint", ".cpp"),
            ("atomic_builtin", ".c"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ATOMIC-FENCE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ATOMIC-FENCE"]
        self.assertFalse(c)

    def test_cxx_notify_thread_exit(self):
        hits = [f for f in run_lints([TD / "notify_exit_lint.cpp"], TD)
                if f.cls == "CXX-NOTIFY-THREAD-EXIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nexit_bad", names)
        self.assertNotIn("nexit_ok", names)
        for stem in ("cond_wait", "condvar", "cvany_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-NOTIFY-THREAD-EXIT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-NOTIFY-THREAD-EXIT"]
        self.assertFalse(c)

    def test_cxx_wstring_convert(self):
        hits = [f for f in run_lints([TD / "wconvert_lint.cpp"], TD)
                if f.cls == "CXX-WSTRING-CONVERT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("wconv_bad", names)
        self.assertNotIn("wconv_ok", names)
        for stem in ("wstring_view_lint", "cxx_wstring"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-WSTRING-CONVERT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-WSTRING-CONVERT"]
        self.assertFalse(c)

    def test_cxx_invoke(self):
        hits = [f for f in run_lints([TD / "invoke_lint.cpp"], TD)
                if f.cls == "CXX-INVOKE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("invoke_bad", names)
        self.assertNotIn("invoke_ok", names)
        for p in TD.glob("*function*"):
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-INVOKE"]
            self.assertFalse(other, msg=p.name)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-INVOKE"]
        self.assertFalse(c)

    def test_cxx_apply(self):
        hits = [f for f in run_lints([TD / "apply_lint.cpp"], TD)
                if f.cls == "CXX-APPLY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("apply_bad", names)
        self.assertNotIn("apply_ok", names)
        for stem in ("invoke_lint", "cxx_invoke"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-APPLY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-APPLY"]
        self.assertFalse(c)

    def test_cxx_reference_wrapper(self):
        hits = [f for f in run_lints([TD / "refwrap_lint.cpp"], TD)
                if f.cls == "CXX-REFERENCE-WRAPPER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("refwrap_bad", names)
        self.assertNotIn("refwrap_ok", names)
        for stem in ("function_null", "function_ref_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-REFERENCE-WRAPPER"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REFERENCE-WRAPPER"]
        self.assertFalse(c)

    def test_cxx_endian(self):
        hits = [f for f in run_lints([TD / "endian_lint.cpp"], TD)
                if f.cls == "CXX-ENDIAN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("endian_bad", names)
        self.assertNotIn("endian_ok", names)
        for stem in ("byteswap_lint", "cxx_byteswap"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ENDIAN"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ENDIAN"]
        self.assertFalse(c)

    def test_cxx_bit_ceil(self):
        hits = [f for f in run_lints([TD / "bitceil_lint.cpp"], TD)
                if f.cls == "CXX-BIT-CEIL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bitceil_bad", names)
        self.assertNotIn("bitceil_ok", names)
        for stem, ext in (
            ("byteswap_lint", ".cpp"),
            ("cxx_byteswap", ".cpp"),
            ("atomic_builtin", ".c"),
            ("builtin_clz", ".c"),
            ("clz_zero", ".c"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-BIT-CEIL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BIT-CEIL"]
        self.assertFalse(c)

    def test_cxx_uncaught_exceptions(self):
        hits = [f for f in run_lints([TD / "uncaught_lint.cpp"], TD)
                if f.cls == "CXX-UNCAUGHT-EXCEPTIONS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uncaught_bad", names)
        self.assertNotIn("uncaught_ok", names)
        for stem in ("exc_ptr_lint", "nested_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-UNCAUGHT-EXCEPTIONS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNCAUGHT-EXCEPTIONS"]
        self.assertFalse(c)

    def test_cxx_ranges_join(self):
        hits = [f for f in run_lints([TD / "vjoin_lint.cpp"], TD)
                if f.cls == "CXX-RANGES-JOIN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vjoin_bad", names)
        self.assertNotIn("vjoin_ok", names)
        for stem in ("vzip_lint", "cxx_views_zip"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-RANGES-JOIN"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RANGES-JOIN"]
        self.assertFalse(c)

    def test_cxx_quick_exit(self):
        hits = [f for f in run_lints([TD / "qexit_lint.cpp"], TD)
                if f.cls == "CXX-QUICK-EXIT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("qexit_bad", names)
        self.assertNotIn("qexit_ok", names)
        for stem, ext in (
            ("noreturn_fatal", ".c"),
            ("system_call", ".c"),
            ("notify_exit_lint", ".cpp"),
            ("cxx_notify_exit", ".cpp"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-QUICK-EXIT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-QUICK-EXIT"]
        self.assertFalse(c)

    def test_cxx_to_array(self):
        hits = [f for f in run_lints([TD / "toarr_lint.cpp"], TD)
                if f.cls == "CXX-TO-ARRAY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("toarr_bad", names)
        self.assertNotIn("toarr_ok", names)
        for stem in ("cxx_array", "array_index_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TO-ARRAY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TO-ARRAY"]
        self.assertFalse(c)

    def test_cxx_zoned_time(self):
        hits = [f for f in run_lints([TD / "zoned_lint.cpp"], TD)
                if f.cls == "CXX-ZONED-TIME"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("zoned_bad", names)
        self.assertNotIn("zoned_ok", names)
        for stem in ("tzdb_lint", "cxx_tzdb", "chrono_seed"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ZONED-TIME"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ZONED-TIME"]
        self.assertFalse(c)

    def test_cxx_kill_dependency(self):
        hits = [f for f in run_lints([TD / "killdep_lint.cpp"], TD)
                if f.cls == "CXX-KILL-DEPENDENCY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("killdep_bad", names)
        self.assertNotIn("killdep_ok", names)
        for stem, ext in (
            ("kill_api", ".c"),
            ("atomic_flag_lint", ".cpp"),
            ("fence_lint", ".cpp"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-KILL-DEPENDENCY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-KILL-DEPENDENCY"]
        self.assertFalse(c)

    def test_cxx_rotl(self):
        hits = [f for f in run_lints([TD / "rotl_lint.cpp"], TD)
                if f.cls == "CXX-ROTL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rotl_bad", names)
        self.assertNotIn("rotl_ok", names)
        for stem in ("byteswap_lint", "bitceil_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ROTL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ROTL"]
        self.assertFalse(c)

    def test_cxx_current_exception(self):
        hits = [f for f in run_lints([TD / "curexc_lint.cpp"], TD)
                if f.cls == "CXX-CURRENT-EXCEPTION"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("curexc_bad", names)
        self.assertNotIn("curexc_ok", names)
        for stem in ("exc_ptr_lint", "nested_lint", "uncaught_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CURRENT-EXCEPTION"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CURRENT-EXCEPTION"]
        self.assertFalse(c)

    def test_cxx_bit_width(self):
        hits = [f for f in run_lints([TD / "bitw_lint.cpp"], TD)
                if f.cls == "CXX-BIT-WIDTH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bitw_bad", names)
        self.assertNotIn("bitw_ok", names)
        for stem, ext in (
            ("bitceil_lint", ".cpp"),
            ("cxx_bitceil", ".cpp"),
            ("rotl_lint", ".cpp"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-BIT-WIDTH"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-BIT-WIDTH"]
        self.assertFalse(c)

    def test_cxx_lerp(self):
        hits = [f for f in run_lints([TD / "lerp_lint.cpp"], TD)
                if f.cls == "CXX-LERP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lerp_bad", names)
        self.assertNotIn("lerp_ok", names)
        for stem in ("midpt_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-LERP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LERP"]
        self.assertFalse(c)

    def test_cxx_midpoint(self):
        hits = [f for f in run_lints([TD / "midpt_lint.cpp"], TD)
                if f.cls == "CXX-MIDPOINT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("midpt_bad", names)
        self.assertNotIn("midpt_ok", names)
        for stem in ("lerp_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-MIDPOINT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MIDPOINT"]
        self.assertFalse(c)

    def test_cxx_cmp_less(self):
        hits = [f for f in run_lints([TD / "cmpl_lint.cpp"], TD)
                if f.cls == "CXX-CMP-LESS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cmpl_bad", names)
        self.assertNotIn("cmpl_ok", names)
        for stem, ext in (
            ("spaceship", ".cpp"),
            ("spaceship_ptr", ".cpp"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CMP-LESS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CMP-LESS"]
        self.assertFalse(c)

    def test_cxx_countl_zero(self):
        hits = [f for f in run_lints([TD / "countl_lint.cpp"], TD)
                if f.cls == "CXX-COUNTL-ZERO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("countl_bad", names)
        self.assertNotIn("countl_ok", names)
        for stem in ("rotl_lint", "bitceil_lint", "byteswap_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-COUNTL-ZERO"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-COUNTL-ZERO"]
        self.assertFalse(c)

    def test_cxx_unreachable(self):
        hits = [f for f in run_lints([TD / "unreach_lint.cpp"], TD)
                if f.cls == "CXX-UNREACHABLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("unreach_bad", names)
        self.assertNotIn("unreach_ok", names)
        for stem in ("assume_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-UNREACHABLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNREACHABLE"]
        self.assertFalse(c)

    def test_cxx_gcd(self):
        hits = [f for f in run_lints([TD / "gcd_lint.cpp"], TD)
                if f.cls == "CXX-GCD"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("gcd_bad", names)
        self.assertNotIn("gcd_ok", names)
        for stem in ("lcm_lint", "bitw_lint", "rotl_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-GCD"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-GCD"]
        self.assertFalse(c)

    def test_cxx_lcm(self):
        hits = [f for f in run_lints([TD / "lcm_lint.cpp"], TD)
                if f.cls == "CXX-LCM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("lcm_bad", names)
        self.assertNotIn("lcm_ok", names)
        for stem in ("gcd_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-LCM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LCM"]
        self.assertFalse(c)

    def test_cxx_clamp(self):
        hits = [f for f in run_lints([TD / "clamp_lint.cpp"], TD)
                if f.cls == "CXX-CLAMP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("clamp_bad", names)
        self.assertNotIn("clamp_ok", names)
        for stem in ("midpt_lint", "lerp_lint", "cxx_midpoint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CLAMP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CLAMP"]
        self.assertFalse(c)

    def test_cxx_exchange(self):
        hits = [f for f in run_lints([TD / "exch_lint.cpp"], TD)
                if f.cls == "CXX-EXCHANGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("exch_bad", names)
        self.assertNotIn("exch_ok", names)
        for stem in ("use_after_move",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-EXCHANGE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXCHANGE"]
        self.assertFalse(c)

    def test_cxx_to_address(self):
        hits = [f for f in run_lints([TD / "toaddr_lint.cpp"], TD)
                if f.cls == "CXX-TO-ADDRESS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("toaddr_bad", names)
        self.assertNotIn("toaddr_ok", names)
        for stem in ("unique_release", "unique_reset"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TO-ADDRESS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TO-ADDRESS"]
        self.assertFalse(c)

    def test_cxx_is_constant_evaluated(self):
        hits = [f for f in run_lints([TD / "ice_lint.cpp"], TD)
                if f.cls == "CXX-IS-CONSTANT-EVALUATED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ice_bad", names)
        self.assertNotIn("ice_ok", names)
        for stem in ("assume_lint", "unreach_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-IS-CONSTANT-EVALUATED"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-IS-CONSTANT-EVALUATED"]
        self.assertFalse(c)

    def test_cxx_addressof(self):
        hits = [f for f in run_lints([TD / "addrof_lint.cpp"], TD)
                if f.cls == "CXX-ADDRESSOF"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("addrof_bad", names)
        self.assertNotIn("addrof_ok", names)
        for stem in ("toaddr_lint", "cxx_to_address"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ADDRESSOF"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ADDRESSOF"]
        self.assertFalse(c)

    def test_cxx_assume_aligned(self):
        hits = [f for f in run_lints([TD / "asmalign_lint.cpp"], TD)
                if f.cls == "CXX-ASSUME-ALIGNED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("asmalign_bad", names)
        self.assertNotIn("asmalign_ok", names)
        for stem in ("assume_lint", "unreach_lint", "ice_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ASSUME-ALIGNED"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ASSUME-ALIGNED"]
        self.assertFalse(c)

    def test_cxx_as_const(self):
        hits = [f for f in run_lints([TD / "asconst_lint.cpp"], TD)
                if f.cls == "CXX-AS-CONST"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("asconst_bad", names)
        self.assertNotIn("asconst_ok", names)
        for stem in ("const_cast",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-AS-CONST"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-AS-CONST"]
        self.assertFalse(c)

    def test_cxx_exclusive_scan(self):
        hits = [f for f in run_lints([TD / "exscan_lint.cpp"], TD)
                if f.cls == "CXX-EXCLUSIVE-SCAN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("exscan_bad", names)
        self.assertNotIn("exscan_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-EXCLUSIVE-SCAN"]
        self.assertFalse(c)

    def test_cxx_make_exception_ptr(self):
        hits = [f for f in run_lints([TD / "mkeptr_lint.cpp"], TD)
                if f.cls == "CXX-MAKE-EXCEPTION-PTR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("mkeptr_bad", names)
        self.assertNotIn("mkeptr_ok", names)
        for stem in ("curexc_lint", "exc_ptr_lint", "nested_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-MAKE-EXCEPTION-PTR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-MAKE-EXCEPTION-PTR"]
        self.assertFalse(c)

    def test_cxx_set_terminate(self):
        hits = [f for f in run_lints([TD / "setterm_lint.cpp"], TD)
                if f.cls == "CXX-SET-TERMINATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("setterm_bad", names)
        self.assertNotIn("setterm_ok", names)
        for stem in ("unreach_lint", "assume_lint", "uncaught_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-SET-TERMINATE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SET-TERMINATE"]
        self.assertFalse(c)

    def test_cxx_inclusive_scan(self):
        hits = [f for f in run_lints([TD / "inscan_lint.cpp"], TD)
                if f.cls == "CXX-INCLUSIVE-SCAN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("inscan_bad", names)
        self.assertNotIn("inscan_ok", names)
        for stem in ("exscan_lint", "cxx_exclusive_scan"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-INCLUSIVE-SCAN"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-INCLUSIVE-SCAN"]
        self.assertFalse(c)

    def test_cxx_transform_reduce(self):
        hits = [f for f in run_lints([TD / "tred_lint.cpp"], TD)
                if f.cls == "CXX-TRANSFORM-REDUCE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tred_bad", names)
        self.assertNotIn("tred_ok", names)
        for stem in ("reduce_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TRANSFORM-REDUCE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TRANSFORM-REDUCE"]
        self.assertFalse(c)

    def test_cxx_reduce(self):
        hits = [f for f in run_lints([TD / "reduce_lint.cpp"], TD)
                if f.cls == "CXX-REDUCE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("reduce_bad", names)
        self.assertNotIn("reduce_ok", names)
        for stem in ("tred_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-REDUCE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REDUCE"]
        self.assertFalse(c)

    def test_cxx_uninitialized_copy(self):
        hits = [f for f in run_lints([TD / "uicopy_lint.cpp"], TD)
                if f.cls == "CXX-UNINITIALIZED-COPY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uicopy_bad", names)
        self.assertNotIn("uicopy_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNINITIALIZED-COPY"]
        self.assertFalse(c)

    def test_cxx_construct_at(self):
        hits = [f for f in run_lints([TD / "construct_lint.cpp"], TD)
                if f.cls == "CXX-CONSTRUCT-AT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("construct_bad", names)
        self.assertNotIn("construct_ok", names)
        for stem in ("toaddr_lint", "addrof_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CONSTRUCT-AT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CONSTRUCT-AT"]
        self.assertFalse(c)

    def test_cxx_forward_like(self):
        hits = [f for f in run_lints([TD / "fwdlike_lint.cpp"], TD)
                if f.cls == "CXX-FORWARD-LIKE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("fwdlike_bad", names)
        self.assertNotIn("fwdlike_ok", names)
        for stem in ("use_after_move", "fwd_ref"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FORWARD-LIKE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FORWARD-LIKE"]
        self.assertFalse(c)

    def test_cxx_uninitialized_fill(self):
        hits = [f for f in run_lints([TD / "uifill_lint.cpp"], TD)
                if f.cls == "CXX-UNINITIALIZED-FILL"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uifill_bad", names)
        self.assertNotIn("uifill_ok", names)
        for stem in ("uicopy_lint", "cxx_uninitialized_copy"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-UNINITIALIZED-FILL"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNINITIALIZED-FILL"]
        self.assertFalse(c)

    def test_cxx_destroy_n(self):
        hits = [f for f in run_lints([TD / "destroyn_lint.cpp"], TD)
                if f.cls == "CXX-DESTROY-N"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("destroyn_bad", names)
        self.assertNotIn("destroyn_ok", names)
        for stem in ("construct_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-DESTROY-N"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DESTROY-N"]
        self.assertFalse(c)

    def test_cxx_add_sat(self):
        hits = [f for f in run_lints([TD / "addsat_lint.cpp"], TD)
                if f.cls == "CXX-ADD-SAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("addsat_bad", names)
        self.assertNotIn("addsat_ok", names)
        for stem in ("abs_ok.c", "add_overflow.c"):
            p = TD / stem
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ADD-SAT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ADD-SAT"]
        self.assertFalse(c)

    def test_cxx_transform_scan(self):
        hits = [f for f in run_lints([TD / "tscan_lint.cpp"], TD)
                if f.cls == "CXX-TRANSFORM-SCAN"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tscan_bad", names)
        self.assertNotIn("tscan_ok", names)
        for stem in ("inscan_lint", "exscan_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TRANSFORM-SCAN"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TRANSFORM-SCAN"]
        self.assertFalse(c)

    def test_cxx_type_identity(self):
        hits = [f for f in run_lints([TD / "typeident_lint.cpp"], TD)
                if f.cls == "CXX-TYPE-IDENTITY"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("typeident_bad", names)
        self.assertNotIn("typeident_ok", names)
        for stem in ("cxx_casts",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TYPE-IDENTITY"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TYPE-IDENTITY"]
        self.assertFalse(c)

    def test_cxx_nontype(self):
        hits = [f for f in run_lints([TD / "nontype_lint.cpp"], TD)
                if f.cls == "CXX-NONTYPE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("nontype_bad", names)
        self.assertNotIn("nontype_ok", names)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-NONTYPE"]
        self.assertFalse(c)

    def test_cxx_layout_compatible(self):
        hits = [f for f in run_lints([TD / "layoutc_lint.cpp"], TD)
                if f.cls == "CXX-LAYOUT-COMPATIBLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("layoutc_bad", names)
        self.assertNotIn("layoutc_ok", names)
        for stem in ("pinter_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-LAYOUT-COMPATIBLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-LAYOUT-COMPATIBLE"]
        self.assertFalse(c)

    def test_cxx_ptr_interconvertible(self):
        hits = [f for f in run_lints([TD / "pinter_lint.cpp"], TD)
                if f.cls == "CXX-PTR-INTERCONVERTIBLE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("pinter_bad", names)
        self.assertNotIn("pinter_ok", names)
        for stem in ("layoutc_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-PTR-INTERCONVERTIBLE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-PTR-INTERCONVERTIBLE"]
        self.assertFalse(c)

    def test_cxx_uninitialized_value(self):
        hits = [f for f in run_lints([TD / "uvalue_lint.cpp"], TD)
                if f.cls == "CXX-UNINITIALIZED-VALUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("uvalue_bad", names)
        self.assertNotIn("uvalue_ok", names)
        for stem in ("uifill_lint", "uicopy_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-UNINITIALIZED-VALUE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-UNINITIALIZED-VALUE"]
        self.assertFalse(c)

    def test_cxx_const_iterator(self):
        hits = [f for f in run_lints([TD / "bciter_lint.cpp"], TD)
                if f.cls == "CXX-CONST-ITERATOR"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("bciter_bad", names)
        self.assertNotIn("bciter_ok", names)
        for stem in ("iter_invalid", "counted_iter"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CONST-ITERATOR"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CONST-ITERATOR"]
        self.assertFalse(c)

    def test_cxx_corresponding_member(self):
        hits = [f for f in run_lints([TD / "corrm_lint.cpp"], TD)
                if f.cls == "CXX-CORRESPONDING-MEMBER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("corrm_bad", names)
        self.assertNotIn("corrm_ok", names)
        for stem in ("layoutc_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CORRESPONDING-MEMBER"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CORRESPONDING-MEMBER"]
        self.assertFalse(c)

    def test_cxx_ranges_to(self):
        hits = [f for f in run_lints([TD / "rto_lint.cpp"], TD)
                if f.cls == "CXX-RANGES-TO"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rto_bad", names)
        self.assertNotIn("rto_ok", names)
        for stem in ("toarr_lint", "addrof_lint", "toaddr_lint",
                     "to_chars_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-RANGES-TO"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-RANGES-TO"]
        self.assertFalse(c)

    def test_cxx_enumerate(self):
        hits = [f for f in run_lints([TD / "enumv_lint.cpp"], TD)
                if f.cls == "CXX-ENUMERATE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("enumv_bad", names)
        self.assertNotIn("enumv_ok", names)
        for stem, ext in (
            ("vzip_lint", ".cpp"),
            ("cxx_views_zip", ".cpp"),
            ("rto_lint", ".cpp"),
        ):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ENUMERATE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ENUMERATE"]
        self.assertFalse(c)

    def test_cxx_cartesian_product(self):
        hits = [f for f in run_lints([TD / "cart_lint.cpp"], TD)
                if f.cls == "CXX-CARTESIAN-PRODUCT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("cart_bad", names)
        self.assertNotIn("cart_ok", names)
        for stem in ("vzip_lint", "cxx_views_zip", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CARTESIAN-PRODUCT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CARTESIAN-PRODUCT"]
        self.assertFalse(c)

    def test_cxx_chunk(self):
        hits = [f for f in run_lints([TD / "chunk_lint.cpp"], TD)
                if f.cls == "CXX-CHUNK"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("chunk_bad", names)
        self.assertNotIn("chunk_ok", names)
        for stem in ("ranges_dangle", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-CHUNK"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-CHUNK"]
        self.assertFalse(c)

    def test_cxx_slide(self):
        hits = [f for f in run_lints([TD / "slide_lint.cpp"], TD)
                if f.cls == "CXX-SLIDE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("slide_bad", names)
        self.assertNotIn("slide_ok", names)
        for stem in ("ranges_dangle", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-SLIDE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SLIDE"]
        self.assertFalse(c)

    def test_cxx_adjacent(self):
        hits = [f for f in run_lints([TD / "adjv_lint.cpp"], TD)
                if f.cls == "CXX-ADJACENT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("adjv_bad", names)
        self.assertNotIn("adjv_ok", names)
        for stem, ext in (("adjtime_api", ".c"), ("rto_lint", ".cpp")):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ADJACENT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ADJACENT"]
        self.assertFalse(c)

    def test_cxx_join_with(self):
        hits = [f for f in run_lints([TD / "jwith_lint.cpp"], TD)
                if f.cls == "CXX-JOIN-WITH"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("jwith_bad", names)
        self.assertNotIn("jwith_ok", names)
        for stem in ("vjoin_lint", "cxx_views_join", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-JOIN-WITH"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-JOIN-WITH"]
        self.assertFalse(c)

    def test_cxx_zip_transform(self):
        hits = [f for f in run_lints([TD / "ztrans_lint.cpp"], TD)
                if f.cls == "CXX-ZIP-TRANSFORM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("ztrans_bad", names)
        self.assertNotIn("ztrans_ok", names)
        for stem in ("vzip_lint", "cxx_views_zip", "jwith_lint", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ZIP-TRANSFORM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ZIP-TRANSFORM"]
        self.assertFalse(c)

    def test_cxx_as_rvalue(self):
        hits = [f for f in run_lints([TD / "asrval_lint.cpp"], TD)
                if f.cls == "CXX-AS-RVALUE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("asrval_bad", names)
        self.assertNotIn("asrval_ok", names)
        for stem in ("asconst_lint", "jwith_lint", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-AS-RVALUE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-AS-RVALUE"]
        self.assertFalse(c)

    def test_cxx_from_range(self):
        hits = [f for f in run_lints([TD / "frange_lint.cpp"], TD)
                if f.cls == "CXX-FROM-RANGE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("frange_bad", names)
        self.assertNotIn("frange_ok", names)
        for stem in ("rto_lint", "jwith_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FROM-RANGE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FROM-RANGE"]
        self.assertFalse(c)

    def test_cxx_scoped_enum(self):
        hits = [f for f in run_lints([TD / "scenum_lint.cpp"], TD)
                if f.cls == "CXX-SCOPED-ENUM"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("scenum_bad", names)
        self.assertNotIn("scenum_ok", names)
        for stem, ext in (("enumv_lint", ".cpp"), ("enum_hole", ".c"),
                          ("jwith_lint", ".cpp"), ("rto_lint", ".cpp")):
            p = TD / f"{stem}{ext}"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-SCOPED-ENUM"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-SCOPED-ENUM"]
        self.assertFalse(c)

    def test_cxx_stride(self):
        hits = [f for f in run_lints([TD / "stride_lint.cpp"], TD)
                if f.cls == "CXX-STRIDE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("stride_bad", names)
        self.assertNotIn("stride_ok", names)
        for stem in ("chunk_lint", "slide_lint", "jwith_lint", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-STRIDE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-STRIDE"]
        self.assertFalse(c)

    def test_cxx_repeat(self):
        hits = [f for f in run_lints([TD / "repeat_lint.cpp"], TD)
                if f.cls == "CXX-REPEAT"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("repeat_bad", names)
        self.assertNotIn("repeat_ok", names)
        for stem in ("jwith_lint", "rto_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-REPEAT"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REPEAT"]
        self.assertFalse(c)

    def test_cxx_take(self):
        hits = [f for f in run_lints([TD / "takev_lint.cpp"], TD)
                if f.cls == "CXX-TAKE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("takev_bad", names)
        self.assertNotIn("takev_ok", names)
        for stem in ("chunk_lint", "slide_lint", "repeat_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TAKE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TAKE"]
        self.assertFalse(c)

    def test_cxx_drop(self):
        hits = [f for f in run_lints([TD / "dropv_lint.cpp"], TD)
                if f.cls == "CXX-DROP"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dropv_bad", names)
        self.assertNotIn("dropv_ok", names)
        for stem in ("chunk_lint", "slide_lint", "repeat_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-DROP"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DROP"]
        self.assertFalse(c)

    def test_cxx_filter(self):
        hits = [f for f in run_lints([TD / "filterv_lint.cpp"], TD)
                if f.cls == "CXX-FILTER"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("filterv_bad", names)
        self.assertNotIn("filterv_ok", names)
        for stem in ("ranges_dangle",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-FILTER"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-FILTER"]
        self.assertFalse(c)

    def test_cxx_transform_view(self):
        hits = [f for f in run_lints([TD / "tview_lint.cpp"], TD)
                if f.cls == "CXX-TRANSFORM-VIEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("tview_bad", names)
        self.assertNotIn("tview_ok", names)
        for stem in ("cxx_transform_reduce", "cxx_transform_scan",
                     "tscan_lint", "tred_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TRANSFORM-VIEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TRANSFORM-VIEW"]
        self.assertFalse(c)

    def test_cxx_elements(self):
        hits = [f for f in run_lints([TD / "elems_lint.cpp"], TD)
                if f.cls == "CXX-ELEMENTS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("elems_bad", names)
        self.assertNotIn("elems_ok", names)
        for stem in ("rto_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-ELEMENTS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-ELEMENTS"]
        self.assertFalse(c)

    def test_cxx_iota(self):
        hits = [f for f in run_lints([TD / "iota_lint.cpp"], TD)
                if f.cls == "CXX-IOTA"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("iota_bad", names)
        self.assertNotIn("iota_ok", names)
        for stem in ("repeat_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-IOTA"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-IOTA"]
        self.assertFalse(c)

    def test_cxx_take_while(self):
        hits = [f for f in run_lints([TD / "twhile_lint.cpp"], TD)
                if f.cls == "CXX-TAKE-WHILE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("twhile_bad", names)
        self.assertNotIn("twhile_ok", names)
        for stem in ("takev_lint", "cxx_take"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-TAKE-WHILE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-TAKE-WHILE"]
        self.assertFalse(c)

    def test_cxx_drop_while(self):
        hits = [f for f in run_lints([TD / "dwhile_lint.cpp"], TD)
                if f.cls == "CXX-DROP-WHILE"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("dwhile_bad", names)
        self.assertNotIn("dwhile_ok", names)
        for stem in ("dropv_lint", "cxx_drop"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-DROP-WHILE"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-DROP-WHILE"]
        self.assertFalse(c)

    def test_cxx_keys(self):
        hits = [f for f in run_lints([TD / "keys_lint.cpp"], TD)
                if f.cls == "CXX-KEYS"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("keys_bad", names)
        self.assertNotIn("keys_ok", names)
        for stem in ("map_at_lint", "cxx_map", "flat_map_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-KEYS"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-KEYS"]
        self.assertFalse(c)

    def test_cxx_values(self):
        hits = [f for f in run_lints([TD / "vals_lint.cpp"], TD)
                if f.cls == "CXX-VALUES"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("vals_bad", names)
        self.assertNotIn("vals_ok", names)
        for stem in ("map_at_lint", "cxx_map", "flat_map_lint"):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-VALUES"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-VALUES"]
        self.assertFalse(c)

    def test_cxx_reverse_view(self):
        hits = [f for f in run_lints([TD / "rview_lint.cpp"], TD)
                if f.cls == "CXX-REVERSE-VIEW"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("rview_bad", names)
        self.assertNotIn("rview_ok", names)
        for stem in ("iota_lint",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-REVERSE-VIEW"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-REVERSE-VIEW"]
        self.assertFalse(c)

    def test_cxx_counted(self):
        hits = [f for f in run_lints([TD / "countv_lint.cpp"], TD)
                if f.cls == "CXX-COUNTED"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("countv_bad", names)
        self.assertNotIn("countv_ok", names)
        for stem in ("counted_iter",):
            p = TD / f"{stem}.cpp"
            if not p.exists():
                continue
            other = [f for f in run_lints([p], TD)
                     if f.cls == "CXX-COUNTED"]
            self.assertFalse(other, msg=stem)
        c = [f for f in run_lints([TD / "abs_ok.c"], TD)
             if f.cls == "CXX-COUNTED"]
        self.assertFalse(c)


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestBMC(unittest.TestCase):
    def test_overflow_failed(self):
        f, _ = fn("add_overflow")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")
        self.assertTrue(r.counterexample)

    def test_div0_failed(self):
        f, _ = fn("div_param")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-DIV-ZERO")

    def test_oob_failed(self):
        f, _ = fn("oob_write")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertIn(r.cls, {"MEM-OOB-WRITE", "MEM-OOB-READ"})

    def test_abs_proved(self):
        f, _ = fn("abs_ok")
        r = bmc_function(f, 8)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_pointer_not_unguarded(self):
        f, _ = fn("null_branch")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS)

    def test_saturate_proved(self):
        f, _ = fn("saturate")
        r = bmc_function(f, 8)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})


if __name__ == "__main__":
    unittest.main()
