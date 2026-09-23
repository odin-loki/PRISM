"""Concrete UB oracle + fuzzer crash tests. python -m unittest tests.test_concrete"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest.mock import patch

from prism import laws
from prism.bmc import INT_MAX, INT_MIN
from prism.concrete import decode_args, execute, interesting_seeds, pack_args
from prism.cparse import extract_functions
from prism.fuse import run_fuse
from prism.fuzz import fuzz_function
from prism.models import Finding, FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestExecute(unittest.TestCase):
    def test_add_overflow(self):
        f, _ = fn("add_overflow")
        hit = execute(f, {"x": INT_MAX})
        self.assertEqual(hit.ub, "INT-SIGNED-OVF")
        ok = execute(f, {"x": 0})
        self.assertIsNone(ok.ub)
        self.assertEqual(ok.value, 100)

    def test_div_param(self):
        f, _ = fn("div_param")
        hit = execute(f, {"x": 1, "y": 0})
        self.assertEqual(hit.ub, "INT-DIV-ZERO")
        ok = execute(f, {"x": 8, "y": 2})
        self.assertIsNone(ok.ub)
        self.assertEqual(ok.value, 4)

    def test_div_min_neg1(self):
        f, _ = fn("div_param")
        hit = execute(f, {"x": INT_MIN, "y": -1})
        self.assertEqual(hit.ub, "INT-SIGNED-OVF")

    def test_oob_write(self):
        f, _ = fn("oob_write")
        hit = execute(f, {"i": 4})
        self.assertEqual(hit.ub, "MEM-OOB-WRITE")
        neg = execute(f, {"i": -1})
        self.assertEqual(neg.ub, "MEM-OOB-WRITE")
        ok = execute(f, {"i": 0})
        self.assertIsNone(ok.ub)
        self.assertEqual(ok.value, 1)

    def test_shift_ub(self):
        f, _ = fn("shift_ub")
        hit = execute(f, {"x": 0})
        self.assertEqual(hit.ub, "INT-SHIFT-UB")

    def test_shift_count(self):
        f = FunctionInfo(
            file="synthetic.c", name="sh", kind="SCALAR", line=1,
            signature="int sh(int x)", params=[("int", "x")],
            body="return x << 32;",
        )
        self.assertEqual(execute(f, {"x": 1}).ub, "INT-SHIFT-UB")

    def test_pointer_skip_not_crash(self):
        f, _ = fn("null_branch")
        rec = execute(f, {})
        self.assertIsNone(rec.ub)
        self.assertEqual(rec.error, "skip-pointer")

    def test_saturate_no_ub(self):
        f, _ = fn("saturate")
        rec = execute(f, {"x": INT_MAX})
        self.assertIsNone(rec.ub)
        self.assertIsNone(rec.error)
        for x in (0, -1, 1, 100, 101, INT_MAX, INT_MIN):
            rec = execute(f, {"x": x})
            self.assertIsNone(rec.ub, f"x={x} {rec.ub}")

    def test_const_local_execute_named_not_trailing(self):
        f, _ = fn("const_local_bad")
        rec = execute(f, {"n": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("const unencoded", rec.error or "")

    def test_struct_local_execute_named_not_trailing(self):
        f, _ = fn("struct_local_bad")
        rec = execute(f, {"x": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("struct unencoded", rec.error or "")
        f2, _ = fn("typedef_local_bad")
        rec2 = execute(f2, {"x": 1})
        self.assertIn("typedef local unencoded", rec2.error or "")

    def test_storage_class_execute_named_not_trailing(self):
        f, _ = fn("register_local_bad")
        rec = execute(f, {"n": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("storage-class unencoded", rec.error or "")
        f2, _ = fn("auto_type_bad")
        rec2 = execute(f2, {"n": 1})
        self.assertIn("storage-class unencoded", rec2.error or "")
        f3, _ = fn("auto_type_gnu_bad")
        rec3 = execute(f3, {"n": 1})
        self.assertIn("storage-class unencoded", rec3.error or "")
        f4, _ = fn("static_local_bad")
        rec4 = execute(f4, {"n": 1})
        self.assertIn("storage-duration unencoded", rec4.error or "")
        f5, _ = fn("extern_local_bad")
        rec5 = execute(f5, {"n": 1})
        self.assertIn("storage-duration unencoded", rec5.error or "")

    def test_layout_syntax_execute_named_not_trailing(self):
        f, _ = fn("anon_enum_bad")
        rec = execute(f, {"n": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("anon enum unencoded", rec.error or "")
        f2, _ = fn("alignas_bad")
        rec2 = execute(f2, {"n": 1})
        self.assertIn("alignas unencoded", rec2.error or "")
        f3, _ = fn("compound_bad")
        rec3 = execute(f3, {"n": 1})
        self.assertIn("compound-lit unencoded", rec3.error or "")
        f4, _ = fn("enum_const_ok")
        rec4 = execute(f4, {"n": 1})
        self.assertIsNone(rec4.error)
        self.assertIsNone(rec4.ub)

    def test_const_for_execute_named_not_trailing(self):
        f, _ = fn("const_for_bad")
        rec = execute(f, {"n": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("const unencoded", rec.error or "")

    def test_anon_struct_execute_named_not_trailing(self):
        f, _ = fn("anon_struct_bad")
        rec = execute(f, {"x": 1})
        self.assertIsNone(rec.ub)
        self.assertIn("struct unencoded", rec.error or "")

    def test_abs_ok_int_min(self):
        f, _ = fn("abs_ok")
        rec = execute(f, {"x": INT_MIN})
        self.assertIsNone(rec.ub, rec.error)
        self.assertEqual(rec.value, INT_MAX)

    def test_loop_prove(self):
        f, _ = fn("loop_prove")
        rec = execute(f, {"x": 10})
        self.assertIsNone(rec.ub, rec.error)
        self.assertEqual(rec.value, 3)

    def test_switch(self):
        f, _ = fn("fsm_step")
        rec = execute(f, {"state": 0, "ev": 1})
        self.assertIsNone(rec.ub, rec.error)
        self.assertEqual(rec.value, 1)

    def test_abs_ter_int_min(self):
        f, _ = fn("abs_ter")
        hit = execute(f, {"x": INT_MIN})
        self.assertEqual(hit.ub, "INT-SIGNED-OVF")

    def test_do_overflow(self):
        f, _ = fn("do_overflow")
        hit = execute(f, {"n": 1073741824})
        self.assertEqual(hit.ub, "INT-SIGNED-OVF")

    def test_pick(self):
        f, _ = fn("pick")
        ok = execute(f, {"x": 1})
        self.assertIsNone(ok.ub, ok.error)
        self.assertEqual(ok.value, 1)

    def test_comma_ovf(self):
        f, _ = fn("comma_ovf")
        hit = execute(f, {"x": 1073741824})
        self.assertEqual(hit.ub, "INT-SIGNED-OVF")

    def test_comma_sum(self):
        f, _ = fn("comma_sum")
        ok = execute(f, {"x": 1, "y": 2})
        self.assertIsNone(ok.ub, ok.error)
        self.assertEqual(ok.value, 0)

    def test_goto_unencoded_not_silent_ok(self):
        f, _ = fn("with_goto")
        rec = execute(f, {"x": 0})
        self.assertIsNone(rec.ub)
        self.assertNotEqual(rec.value, 1, "goto must not prove x+1 path")
        if rec.error is None:
            self.assertNotEqual(rec.value, 1)

    def test_bare_return_is_not_parse_error(self):
        f, _ = fn("taut_bound_bad")
        rec = execute(f, {"idx": 0})
        self.assertIsNone(rec.error, rec.error)
        self.assertIsNone(rec.ub)

    def test_char_literal_is_not_parse_error(self):
        f, _ = fn("trunc_ok")
        rec = execute(f, {})
        self.assertIsNone(rec.error, rec.error)
        self.assertIsNone(rec.ub)
        self.assertEqual(rec.value, 1 + ord("A") + 42)

    def test_throw_is_not_silent_ok(self):
        path = TD / "throw_dtor.cpp"
        hits = [f for f in extract_functions(path, str(path))
                if f.name == "throws_not_dtor"]
        self.assertTrue(hits)
        rec = execute(hits[0], {})
        self.assertIsNone(rec.ub)
        self.assertIsNotNone(rec.error)
        self.assertIn("throw", rec.error.lower())

    def test_strcpy_overflow_is_oob_write(self):
        f, _ = fn("copy_bad")
        self.assertIn("overflow", f.body)
        rec = execute(f, {})
        self.assertIsNone(rec.error, rec.error)
        self.assertEqual(rec.ub, "MEM-OOB-WRITE")

    def test_snprintf_bounded_is_not_crash(self):
        f, _ = fn("copy_ok")
        rec = execute(f, {})
        self.assertIsNone(rec.error, rec.error)
        self.assertIsNone(rec.ub)

    def test_postfix_dec_overflow(self):
        f, _ = fn("empty_inf_ok")
        rec = execute(f, {"n": INT_MIN})
        self.assertIsNone(rec.error, rec.error)
        self.assertEqual(rec.ub, "INT-SIGNED-OVF")

    def test_postfix_dec_zero_exits(self):
        f, _ = fn("empty_inf_ok")
        rec = execute(f, {"n": 0})
        self.assertIsNone(rec.error, rec.error)
        self.assertIsNone(rec.ub)
        f, _ = fn("add_u")
        rec = execute(f, {"a": INT_MAX, "b": INT_MAX})
        self.assertIsNone(rec.ub, rec.error)
        self.assertIsNone(rec.error)

    def test_unsigned_index_guard_blocks_huge_index(self):
        f, _ = fn("idx_u_ok")
        rec = execute(f, {"i": -1})
        self.assertIsNone(rec.ub, rec.error)
        self.assertEqual(rec.value, 0)

    def test_unsigned_index_oob(self):
        f, _ = fn("idx_u_bad")
        rec = execute(f, {"i": 4})
        self.assertEqual(rec.ub, "MEM-OOB-READ")

    def test_long_long_int_max_plus_one_is_defined(self):
        f, _ = fn("add_ll")
        rec = execute(f, {"a": 1, "b": INT_MAX})
        self.assertIsNone(rec.ub, rec.error)

    def test_long_long_overflows_at_2_63(self):
        f, _ = fn("add_ll")
        rec = execute(f, {"a": 1 << 62, "b": 1 << 62})
        self.assertEqual(rec.ub, "INT-SIGNED-OVF")


class TestDecode(unittest.TestCase):
    def test_little_endian_int(self):
        f, _ = fn("add_overflow")
        data = (INT_MAX).to_bytes(4, "little", signed=True)
        args = decode_args(f, data)
        self.assertEqual(args["x"], INT_MAX)
        self.assertEqual(pack_args(f, args), data)

    def test_interesting_seeds_include_int_max_and_zero(self):
        f, _ = fn("div_param")
        seeds = interesting_seeds(f)
        decoded = [decode_args(f, s) for s in seeds]
        self.assertTrue(any(a["y"] == 0 for a in decoded))
        f2, _ = fn("add_overflow")
        decoded2 = [decode_args(f2, s) for s in interesting_seeds(f2)]
        self.assertTrue(any(a["x"] == INT_MAX for a in decoded2))


class TestFuzzOracle(unittest.TestCase):
    def test_planted_bugs_crash(self):
        cases = [
            ("add_overflow", "INT-SIGNED-OVF"),
            ("div_param", "INT-DIV-ZERO"),
            ("oob_write", "MEM-OOB-WRITE"),
            ("shift_ub", "INT-SHIFT-UB"),
        ]
        for name, cls in cases:
            with self.subTest(name=name):
                f, p = fn(name)
                r = fuzz_function(f, p, budget=0.5, iters=8)
                self.assertEqual(r.status, laws.CRASH, r.message)
                self.assertEqual(r.cls, cls)
                self.assertTrue(r.counterexample)
                self.assertFalse(laws.is_proof(r.status))

    def test_saturate_clean_not_proof(self):
        f, p = fn("saturate")
        r = fuzz_function(f, p, budget=0.3, iters=8)
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING})
        self.assertIn("not a proof", r.message)

    def test_pointer_needs_harness_not_error(self):
        f, p = fn("null_branch")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertIn("POINTER", r.message)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs[0].status, laws.ERROR)
        self.assertIn("POINTER", recs[0].message)

    def test_array_decay_fuzz_needs_harness_not_clean(self):
        f, p = fn("arr_esc_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_try_catch_needs_harness_not_error(self):
        path = TD / "try_catch.cpp"
        hits = [f for f in extract_functions(path, str(path))
                if f.name == "try_ok"]
        self.assertTrue(hits)
        f = hits[0]
        r = fuzz_function(f, path, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], path.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_new_without_ptr_decl_needs_harness_not_error(self):
        path = TD / "cxx_newdel.cpp"
        hits = [f for f in extract_functions(path, str(path))
                if f.name == "new_no_ptr"]
        self.assertTrue(hits)
        f = hits[0]
        r = fuzz_function(f, path, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], path.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_asm_needs_harness_not_error(self):
        f, p = fn("asm_vol")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_atomic_qual_needs_harness_not_error(self):
        f, p = fn("atom_qual_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_const_local_needs_harness_not_error(self):
        f, p = fn("const_local_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_struct_local_needs_harness_not_error(self):
        f, p = fn("struct_local_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)
        f2, p2 = fn("typedef_local_bad")
        r2 = fuzz_function(f2, p2, budget=0.1, iters=1)
        self.assertEqual(r2.status, laws.NEEDS_HARNESS, r2.message)
        self.assertNotEqual(r2.status, laws.ERROR)

    def test_return_paren_buf_fuzz_needs_harness_not_clean(self):
        f, p = fn("arr_esc_paren")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_return_paren_addr_fuzz_needs_harness_not_clean(self):
        f, p = fn("esc_paren_addr")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_storage_class_fuzz_needs_harness_not_error(self):
        f, p = fn("register_local_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        f2, p2 = fn("auto_type_bad")
        r2 = fuzz_function(f2, p2, budget=0.1, iters=1)
        self.assertEqual(r2.status, laws.NEEDS_HARNESS, r2.message)
        self.assertNotEqual(r2.status, laws.ERROR)
        f3, p3 = fn("static_local_bad")
        r3 = fuzz_function(f3, p3, budget=0.1, iters=1)
        self.assertEqual(r3.status, laws.NEEDS_HARNESS, r3.message)
        self.assertNotEqual(r3.status, laws.ERROR)

    def test_layout_syntax_fuzz_needs_harness_not_error(self):
        for name in ("anon_enum_bad", "alignas_bad", "compound_bad"):
            f, p = fn(name)
            r = fuzz_function(f, p, budget=0.1, iters=1)
            self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
            self.assertNotEqual(r.status, laws.ERROR)
            self.assertNotEqual(r.status, laws.CLEAN)

    def test_const_for_fuzz_needs_harness_not_error(self):
        f, p = fn("const_for_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)

    def test_anon_struct_fuzz_needs_harness_not_error(self):
        f, p = fn("anon_struct_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_memcpy_fuzz_needs_harness_not_clean(self):
        f, p = fn("missing_nul_bad")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_mkstemp_fuzz_needs_harness_not_error(self):
        f, p = fn("mkstemp_ok")
        r = fuzz_function(f, p, budget=0.1, iters=1)
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)


class TestFuseEngine(unittest.TestCase):
    def test_engine_none_unchanged(self):
        f, p = fn("null_branch")
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1, engine=None)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs[0].status, laws.ERROR)

    def test_fuzz4all_seeds_before_fuzz(self):
        f, p = fn("saturate")
        passed_seeds: list[list[bytes] | None] = []

        class Eng:
            def available(self):
                return True

        def fake_fuzz4all(engine, fn_, nbytes):
            return [b"\x01\x00\x00\x00"]

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )

        def fake_fuzz(fn_, src, budget=0, iters=0, seeds=None):
            passed_seeds.append(seeds)
            return clean

        with patch("prism.agent.fuzz4all_seeds", side_effect=fake_fuzz4all):
            with patch("prism.fuse.fuzz_function", side_effect=fake_fuzz):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertTrue(passed_seeds)
        self.assertIsNotNone(passed_seeds[0])
        self.assertIn(b"\x01\x00\x00\x00", passed_seeds[0])
        self.assertEqual(recs[0].extra.get("fuzz4all"), 1)
        self.assertEqual(recs[0].status, laws.CLEAN)

    def test_stall_calls_chatfuzz_once(self):
        f, p = fn("saturate")
        called = []

        class Eng:
            def available(self):
                return True

        def fake_mutants(engine, fn_, seed_hex):
            called.append((fn_.name, seed_hex))
            return [b"\x02\x00\x00\x00"]

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.fuzz_function", return_value=clean):
            with patch("prism.agent.chatfuzz_mutants", side_effect=fake_mutants):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertEqual(len(called), 1, called)
        self.assertEqual(called[0][0], "saturate")
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))


class TestCppFuzzBinarySource(unittest.TestCase):
    """C++ fuzz_function must match the Python engine _binary_fuzz after concrete."""

    def test_binary_fuzz_after_concrete_before_clean(self):
        src = (ROOT / "src" / "prism" / "stages" / "fuse.cpp").read_text(encoding="utf-8")
        start = src.find("Finding fuzz_function(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 16000]
        self.assertIn("compile_afl_harness", body)
        self.assertIn("oracle\"] = \"binary\"", body)
        self.assertIn("binary\"] = \"compile-failed\"", body)
        self.assertIn("which({\"gcc\", \"clang\"})", body)
        self.assertNotIn("(void)src;", body)


if __name__ == "__main__":
    unittest.main()
