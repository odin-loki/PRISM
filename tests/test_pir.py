"""The pir stage (Clang -> LLVM IR -> PIR -> Z3, roadmap Part 2; docs/PIR.md).

The C++ engine runs it on tests/pir (true and false variants per construct)
when PRISM_BIN is set; the Python engine is frozen (roadmap D8) and records a
single NOTRUN row for the stage.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.pipeline import EXEC_STAGES, STAGE_ORDER, run_pir_notrun

ROOT = Path(__file__).resolve().parents[1]
TASKS = ROOT / "tests" / "pir"
PIPELINE_CPP = (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8")

# (file, function) -> expected status. *_bad = a real defect (FAILED with a
# counterexample), *_ok = PROVED, loops that do not close = BOUNDED, closed
# k-induction = PROVED-UNBOUNDED, unmodelled constructs = NEEDS-HARNESS.
EXPECTED: dict[tuple[str, str], str] = {
    ("overflow.c", "add_bad"): laws.FAILED,
    ("overflow.c", "add_ok"): laws.PROVED,
    ("overflow.c", "add_unsigned_ok"): laws.PROVED,
    ("overflow.c", "mul_bad"): laws.FAILED,
    ("overflow.c", "neg_bad"): laws.FAILED,
    ("div0.c", "div_bad"): laws.FAILED,
    ("div0.c", "div_ok"): laws.PROVED,
    ("div0.c", "mod_min_bad"): laws.FAILED,
    ("div0.c", "urem_bad"): laws.FAILED,
    ("shift.c", "shl_amount_bad"): laws.FAILED,
    ("shift.c", "shl_masked_ok"): laws.PROVED,
    ("shift.c", "shl_sign_bad"): laws.FAILED,
    ("shift.c", "shl_sign_ok"): laws.PROVED,
    ("shift.c", "ashr_ok"): laws.PROVED,
    ("loops.c", "count4_ok"): laws.PROVED,
    ("loops.c", "loop_ovf_bad"): laws.FAILED,
    ("loops.c", "loop_long_bounded"): laws.BOUNDED,
    ("loops.c", "nested_ok"): laws.PROVED,
    ("kinduct.c", "kind_closed"): laws.PROVED_UNBOUNDED,
    ("kinduct.c", "kind_countdown"): laws.PROVED_UNBOUNDED,
    ("kinduct.c", "kind_open"): laws.BOUNDED,
    ("switch.c", "sw_bad"): laws.FAILED,
    ("switch.c", "sw_ok"): laws.PROVED,
    ("select.c", "max_ok"): laws.PROVED,
    ("select.c", "sel_div_bad"): laws.FAILED,
    ("select.c", "abs_bad"): laws.FAILED,
    ("select.c", "abs_ok"): laws.PROVED,
    ("casts.c", "trunc_ok"): laws.PROVED,
    ("casts.c", "widen_bad"): laws.FAILED,
    ("casts.c", "widen_ok"): laws.PROVED,
    ("casts.c", "uchar_ok"): laws.PROVED,
    ("casts.c", "sext_mul_ok"): laws.PROVED,
    ("uninit.c", "uninit_bad"): laws.FAILED,
    ("uninit.c", "uninit_ok"): laws.PROVED,
    ("assert.c", "assert_bad"): laws.FAILED,
    ("assert.c", "assert_ok"): laws.PROVED,
    ("unencoded.c", "deref_ptr"): laws.NEEDS_HARNESS,
    ("unencoded.c", "local_array"): laws.PROVED,  # memory model (docs/PIR.md): a[i & 3] is in bounds
    ("unencoded.c", "twice_double"): laws.PROVED,  # IEEE floating point is encoded (docs/PIR.md)
    ("unencoded.c", "recurse"): laws.NEEDS_HARNESS,
    ("calls.c", "helper"): laws.FAILED,
    ("calls.c", "caller_ok"): laws.PROVED,
    ("calls.c", "caller_bad"): laws.FAILED,
    ("folded.c", "fold_shift_bad"): laws.FAILED,
    ("folded.c", "fold_div_bad"): laws.FAILED,
    ("folded.c", "fold_ok"): laws.PROVED,
    ("cxx.cpp", "cxx_add_bad"): laws.FAILED,
    ("cxx.cpp", "use_twice_ok"): laws.PROVED,
    ("cxx.cpp", "get"): laws.NEEDS_HARNESS,  # `this` is a pointer (Law 6)
    ("cxx.cpp", "cxx_shl_ok"): laws.PROVED,  # C++20+: signed << is defined
    ("cxx.cpp", "constexpr_ok"): laws.PROVED,
}

# Memory model, library models, pointer contracts (docs/PIR.md "Memory
# model", "Library models", "Pointer parameters"): tests/pir/mem_*.
MEM_EXPECTED: dict[tuple[str, str], tuple[str, str]] = {
    ("mem_array.c", "arr_read_ok"): (laws.PROVED, ""),
    ("mem_array.c", "arr_read_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_array.c", "arr_write_ok"): (laws.PROVED, ""),
    ("mem_array.c", "arr_write_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_array.c", "arr_2d_ok"): (laws.PROVED, ""),
    ("mem_array.c", "global_ok"): (laws.PROVED, ""),
    ("mem_array.c", "global_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_array.c", "struct_ok"): (laws.PROVED, ""),
    ("mem_array.c", "uninit_mem_bad"): (laws.FAILED, "UNINIT-READ"),
    ("mem_array.c", "uninit_mem_ok"): (laws.PROVED, ""),
    ("mem_heap.c", "heap_ok"): (laws.PROVED, ""),
    ("mem_heap.c", "heap_oob_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_heap.c", "null_bad"): (laws.FAILED, "PTR-NULL-DEREF"),
    ("mem_heap.c", "uaf_bad"): (laws.FAILED, "MEM-UAF"),
    ("mem_heap.c", "double_free_bad"): (laws.FAILED, "MEM-DOUBLE-FREE"),
    ("mem_heap.c", "invalid_free_bad"): (laws.FAILED, "MEM-INVALID-FREE"),
    ("mem_heap.c", "free_offset_bad"): (laws.FAILED, "MEM-INVALID-FREE"),
    ("mem_heap.c", "free_null_ok"): (laws.PROVED, ""),
    ("mem_heap.c", "heap_uninit_bad"): (laws.FAILED, "UNINIT-READ"),
    ("mem_heap.c", "calloc_ok"): (laws.PROVED, ""),
    ("mem_heap.c", "realloc_ok"): (laws.PROVED, ""),
    ("mem_heap.c", "realloc_stale_bad"): (laws.FAILED, "MEM-UAF"),
    ("mem_ptr.c", "one_past_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "arith_bad"): (laws.FAILED, "MEM-PTR-ARITH"),
    ("mem_ptr.c", "arith_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "cmp_bad"): (laws.FAILED, "PTR-COMPARE"),
    ("mem_ptr.c", "cmp_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "diff_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "escape_bad"): (laws.FAILED, "MEM-STACK-ESCAPE"),
    ("mem_ptr.c", "escape_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "null_deref_bad"): (laws.FAILED, "PTR-NULL-DEREF"),
    ("mem_ptr.c", "null_deref_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "misaligned_bad"): (laws.FAILED, "MEM-MISALIGNED"),
    ("mem_ptr.c", "aligned_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "literal_write_bad"): (laws.FAILED, "MEM-WRITE-CONST"),
    ("mem_ptr.c", "literal_read_ok"): (laws.PROVED, ""),
    ("mem_ptr.c", "lifetime_bad"): (laws.FAILED, "MEM-UAF"),
    ("mem_ptr.c", "byval_ok"): (laws.PROVED, ""),
    ("mem_str.c", "strlen_ok"): (laws.PROVED, ""),
    ("mem_str.c", "strcpy_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_str.c", "strcpy_ok"): (laws.PROVED, ""),
    ("mem_str.c", "unterminated_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_str.c", "memcpy_ok"): (laws.PROVED, ""),
    ("mem_str.c", "memcpy_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_str.c", "overlap_bad"): (laws.FAILED, "MEM-OVERLAP"),
    ("mem_str.c", "memmove_ok"): (laws.PROVED, ""),
    ("mem_str.c", "memset_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_str.c", "strcat_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_libc.c", "abs_bad"): (laws.FAILED, "INT-SIGNED-OVF"),
    ("mem_libc.c", "abs_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "getenv_bad"): (laws.FAILED, "PTR-NULL-DEREF"),
    ("mem_libc.c", "getenv_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "atoi_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "printf_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "printf_n_bad"): (laws.FAILED, "FMT-PERCENT-N"),
    ("mem_libc.c", "printf_args_bad"): (laws.FAILED, "FMT-ARGS"),
    ("mem_libc.c", "printf_type_bad"): (laws.FAILED, "FMT-ARGS"),
    ("mem_libc.c", "snprintf_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "sprintf_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_libc.c", "fgets_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "fgets_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_libc.c", "fopen_bad"): (laws.FAILED, "PTR-NULL-DEREF"),
    ("mem_libc.c", "fclose_twice_bad"): (laws.FAILED, "MEM-DOUBLE-FREE"),
    ("mem_libc.c", "generic_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "vla_ok"): (laws.PROVED, ""),
    ("mem_libc.c", "vla_size_bad"): (laws.FAILED, "MEM-VLA-SIZE"),
    ("mem_libc.c", "vla_oob_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_libc.c", "setjmp_unenc"): (laws.PROVED, ""),  # setjmp/longjmp are exception-like edges now
    ("mem_contract.c", "no_contract"): (laws.NEEDS_HARNESS, ""),
    ("mem_contract.c", "first_last_ok"): (laws.PROVED_ASSUMING, ""),
    ("mem_contract.c", "past_end_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_contract.c", "count_pos_ok"): (laws.PROVED_ASSUMING, ""),
    ("mem_contract.c", "off_by_one_bad"): (laws.FAILED, "MEM-OOB-WRITE"),
    ("mem_contract.c", "readonly_write_bad"): (laws.FAILED, "MEM-WRITE-CONST"),
    ("mem_contract.c", "draft_count"): (laws.NEEDS_HARNESS, ""),  # drafts are opt-in (--pir-drafts)
    ("mem_cxx.cpp", "new_delete_ok"): (laws.PROVED, ""),
    ("mem_cxx.cpp", "new_array_ok"): (laws.PROVED, ""),
    ("mem_cxx.cpp", "mismatch_bad"): (laws.FAILED, "MEM-MISMATCHED-FREE"),
    ("mem_cxx.cpp", "new_oob_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_cxx.cpp", "delete_twice_bad"): (laws.FAILED, "MEM-DOUBLE-FREE"),
    ("mem_stl.cpp", "span_ok"): (laws.PROVED, ""),
    ("mem_stl.cpp", "span_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_stl.cpp", "array_ok"): (laws.PROVED, ""),
    ("mem_stl.cpp", "array_bad"): (laws.FAILED, "MEM-OOB-READ"),
    ("mem_stl.cpp", "optional_ok"): (laws.PROVED, ""),
    ("mem_stl.cpp", "optional_bad"): (laws.FAILED, "CXX-OPTIONAL-NULL"),
    ("mem_stl.cpp", "unique_ok"): (laws.PROVED, ""),
    ("mem_stl.cpp", "unique_bad"): (laws.FAILED, "PTR-NULL-DEREF"),
    ("mem_stl.cpp", "vector_ok"): (laws.PROVED, ""),
    ("mem_stl.cpp", "vector_bad"): (laws.FAILED, "MEM-OOB-READ"),
    # at(3) throws std::out_of_range: the cleanup runs and the exception leaves
    # the function (not main, not noexcept): no violation (docs/PIR.md "Exceptions")
    ("mem_stl.cpp", "vector_at_throws"): (laws.PROVED, ""),
}

# Roadmap 2.6 (docs/PIR.md "Floating point", "Exceptions", "Coroutines",
# "setjmp/longjmp", "Indirect calls", "Inline assembly"): tests/pir/{fp,eh,
# coro,sjlj,virt,asm}_*.
PIR3_EXPECTED: dict[tuple[str, str], tuple[str, str]] = {
    ("fp_arith.c", "fp_cast_bad"): (laws.FAILED, "FLOAT-CAST-OVF"),
    ("fp_arith.c", "fp_cast_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_nan_guard_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_exact_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_float_round_bad"): (laws.FAILED, "FUNC-CONTRACT"),
    ("fp_arith.c", "fp_add_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_add_bad"): (laws.FAILED, "FUNC-CONTRACT"),
    ("fp_arith.c", "twice_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_to_unsigned_bad"): (laws.FAILED, "FLOAT-CAST-OVF"),
    ("fp_arith.c", "fp_half_way_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_sin_ok"): (laws.PROVED, ""),
    ("fp_arith.c", "fp_sin_bad"): (laws.FAILED, "FLOAT-CAST-OVF"),
    ("fp_arith.c", "fp_div"): (laws.PROVED, ""),  # FLOAT-DIV-ZERO etc. only with --fp-checks
    ("fp_arith.c", "fp_div_guard"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_catch_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_catch_bad"): (laws.FAILED, "INT-DIV-ZERO"),
    ("eh_catch.cpp", "eh_noexcept_bad"): (laws.FAILED, "CXX-THROW-NOEXCEPT"),
    ("eh_catch.cpp", "eh_noexcept_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_wrong_type_bad"): (laws.FAILED, "CXX-THROW-NOEXCEPT"),
    ("eh_catch.cpp", "eh_base_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_cleanup_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_cleanup_bad"): (laws.FAILED, "FUNC-CONTRACT"),
    ("eh_catch.cpp", "eh_rethrow_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_std_ok"): (laws.PROVED, ""),
    ("eh_catch.cpp", "eh_std_bad"): (laws.FAILED, "CXX-THROW-NOEXCEPT"),
    ("eh_uncaught.cpp", "main"): (laws.FAILED, "CXX-UNCAUGHT"),
    ("coro_gen.cpp", "coro_ok"): (laws.PROVED, ""),
    ("coro_gen.cpp", "coro_bad"): (laws.FAILED, "INT-DIV-ZERO"),
    ("sjlj_basic.c", "sjlj_ok"): (laws.PROVED, ""),
    ("sjlj_basic.c", "sjlj_zero_ok"): (laws.PROVED, ""),
    ("sjlj_basic.c", "sjlj_bad"): (laws.FAILED, "INT-DIV-ZERO"),
    ("sjlj_basic.c", "sjlj_dead_frame_bad"): (laws.FAILED, "CTRL-LONGJMP-INVALID"),
    ("virt_dispatch.cpp", "virt_ok"): (laws.PROVED, ""),
    ("virt_dispatch.cpp", "virt_bad"): (laws.FAILED, "INT-DIV-ZERO"),
    ("virt_dispatch.cpp", "fnptr_ok"): (laws.PROVED, ""),
    ("virt_dispatch.cpp", "fnptr_bad"): (laws.FAILED, "INT-SIGNED-OVF"),
    ("asm_contract.c", "asm_nocontract"): (laws.NEEDS_HARNESS, ""),
    ("asm_contract.c", "asm_ok"): (laws.PROVED_ASSUMING, ""),
    ("asm_contract.c", "asm_bad"): (laws.FAILED, "MEM-OOB-READ"),
    # k-induction for read-only loops of functions with memory (docs/PIR.md)
    ("kind_mem.c", "kind_mem_read_closed"): (laws.PROVED_UNBOUNDED, ""),
    ("kind_mem.c", "kind_mem_write_bounded"): (laws.BOUNDED, ""),
    ("kind_mem.c", "kind_mem_read_bad"): (laws.FAILED, "MEM-OOB-READ"),
}

CLASSES = {
    ("overflow.c", "add_bad"): "INT-SIGNED-OVF",
    ("div0.c", "div_bad"): "INT-DIV-ZERO",
    ("shift.c", "shl_amount_bad"): "INT-SHIFT-UB",
    ("shift.c", "shl_sign_bad"): "INT-SHIFT-UB",
    ("uninit.c", "uninit_bad"): "UNINIT-READ",
    ("assert.c", "assert_bad"): "FUNC-CONTRACT",
    ("switch.c", "sw_bad"): "INT-DIV-ZERO",
}


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return Path(env)
    return None


def _clang() -> bool:
    return all(shutil.which(t) for t in ("clang", "clang++", "opt"))


class TestPythonEngineFrozen(unittest.TestCase):
    """D8: the Python engine lists the stage and says it did not run it."""

    def test_stage_after_bmc_in_both_engines(self):
        self.assertEqual(STAGE_ORDER[STAGE_ORDER.index("bmc") + 1], "pir")
        self.assertIn('stage("pir"', PIPELINE_CPP)

    def test_notrun_row(self):
        rows = run_pir_notrun()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].stage, "pir")
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertIn("roadmap D8", rows[0].message)

    def test_exec_table(self):
        # Law 9: the lli translation-validation half is the executing part.
        self.assertEqual(EXEC_STAGES["pir"], "part")


@unittest.skipUnless(_cpp_prism() and _clang(), "needs PRISM_BIN and clang/clang++/opt on PATH")
class TestPirStage(unittest.TestCase):
    report: dict = {}
    report_exec: dict = {}

    @classmethod
    def _run(cls, *extra: str) -> dict:
        with tempfile.TemporaryDirectory(prefix="prism_pir_") as td:
            out = Path(td) / "out"
            subprocess.run(
                [str(_cpp_prism()), str(TASKS), "--no-llm", "--stage", "pir", "--out", str(out),
                 "--jobs", "2", *extra],
                cwd=ROOT, capture_output=True, text=True, timeout=1800,
            )
            return json.loads((out / "report.json").read_text(encoding="utf-8"))

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = cls._run()

    def _pir(self, report: dict) -> list[dict]:
        st = next(s for s in report["stages"] if s["name"] == "pir")
        return st["findings"]

    def _verdicts(self, report: dict) -> dict[tuple[str, str], dict]:
        out: dict[tuple[str, str], dict] = {}
        for f in self._pir(report):
            if f.get("function"):
                out.setdefault((Path(f["file"]).name, f["function"]), f)
        return out

    def test_expected_verdicts(self):
        got = self._verdicts(self.report)
        wrong = {k: (v, got.get(k, {}).get("status")) for k, v in EXPECTED.items()
                 if got.get(k, {}).get("status") != v}
        self.assertEqual(wrong, {})

    def test_failed_carries_counterexample_and_class(self):
        got = self._verdicts(self.report)
        for k, v in EXPECTED.items():
            if v != laws.FAILED:
                continue
            f = got[k]
            self.assertTrue(f["counterexample"], k)
            self.assertEqual(f["extra"]["cex"], f["counterexample"])
        for k, cls in CLASSES.items():
            self.assertEqual(got[k]["cls"], cls, k)

    def test_frontend_and_pir_hash(self):
        got = self._verdicts(self.report)
        f = got[("overflow.c", "add_ok")]
        self.assertTrue(f["extra"]["frontend"].startswith("clang-"))
        self.assertRegex(f["extra"]["pir_hash"], r"^[0-9a-f]{64}$")

    def test_unencoded_is_named(self):
        got = self._verdicts(self.report)
        self.assertIn("Law 6", got[("unencoded.c", "deref_ptr")]["message"])
        # local arrays, floating point and setjmp are in the model now; inline
        # assembly without a contract is named
        self.assertIn("inline assembly", got[("asm_contract.c", "asm_nocontract")]["message"])
        self.assertEqual(got[("unencoded.c", "twice_double")]["status"], laws.PROVED)

    def test_memory_verdicts_and_classes(self):
        got = self._verdicts(self.report)
        wrong = {}
        for k, (st, cls) in MEM_EXPECTED.items():
            f = got.get(k, {})
            if f.get("status") != st or (cls and f.get("cls") != cls):
                wrong[k] = ((st, cls), (f.get("status"), f.get("cls"), f.get("message")))
        self.assertEqual(wrong, {})

    def test_roadmap_26_verdicts_and_classes(self):
        got = self._verdicts(self.report)
        wrong = {}
        for k, (st, cls) in PIR3_EXPECTED.items():
            f = got.get(k, {})
            if f.get("status") != st or (cls and f.get("cls") != cls):
                wrong[k] = ((st, cls), (f.get("status"), f.get("cls"), f.get("message")))
        self.assertEqual(wrong, {})
        # the asm contract is an assumption, listed like a pointer contract
        f = got[("asm_contract.c", "asm_ok")]
        self.assertIn("asm contract", f["extra"]["assumptions"])
        self.assertEqual(f["extra"]["verdict_before_assumptions"], laws.PROVED)
        self.assertIn("sin", got[("fp_arith.c", "fp_sin_ok")]["extra"]["libm_unconstrained"])
        self.assertRegex(got[("fp_arith.c", "fp_cast_bad")]["counterexample"], r"^d=")
        self.assertEqual(got[("kind_mem.c", "kind_mem_read_closed")]["extra"]["k_induction_memory"], "read-only loop")
        self.assertEqual(got[("kind_mem.c", "kind_mem_write_bounded")]["extra"]["k_induction"],
                         "not-attempted (memory written in the loop)")

    def test_fp_checks_are_opt_in(self):
        on = self._verdicts(self._run("--fp-checks"))
        f = on[("fp_arith.c", "fp_div")]
        self.assertEqual(f["status"], laws.FAILED)
        self.assertIn(f["cls"], {"FLOAT-DIV-ZERO", "FLOAT-INVALID", "FLOAT-OVERFLOW"})
        self.assertEqual(on[("fp_arith.c", "fp_div_guard")]["status"], laws.PROVED)
        self.assertEqual(on[("fp_arith.c", "fp_cast_ok")]["status"], laws.PROVED)

    def test_memory_failed_has_line_and_prop(self):
        got = self._verdicts(self.report)
        for k, (st, _) in MEM_EXPECTED.items():
            if st == laws.FAILED:
                self.assertTrue(got[k]["extra"].get("prop"), k)
                self.assertTrue(got[k].get("line"), k)

    def test_contract_proof_lists_assumptions(self):
        got = self._verdicts(self.report)
        f = got[("mem_contract.c", "first_last_ok")]
        self.assertIn("assumptions", f["extra"])
        self.assertIn("\\valid(p + (0..3))", f["extra"]["assumptions"])
        self.assertEqual(f["extra"]["verdict_before_assumptions"], laws.PROVED)
        self.assertIn("Law 6", got[("mem_contract.c", "no_contract")]["message"])
        self.assertIn("Law 6", got[("mem_contract.c", "draft_count")]["message"])

    def test_harness_drafts_are_opt_in(self):
        on = self._verdicts(self._run("--pir-drafts"))
        f = on[("mem_contract.c", "draft_count")]
        self.assertEqual(f["status"], laws.PROVED_ASSUMING)
        self.assertIn("harness (template draft)", f["extra"]["assumptions"])
        self.assertIn("(drafted size range)", f["extra"]["assumptions"])
        # a user-stated contract still decides; unguarded literal-index sizes are not drafted
        self.assertEqual(on[("mem_contract.c", "first_last_ok")]["status"], laws.PROVED_ASSUMING)
        self.assertEqual(on[("mem_contract.c", "no_contract")]["status"], laws.NEEDS_HARNESS)

    def test_library_models_and_memory_notes(self):
        got = self._verdicts(self.report)
        f = got[("mem_str.c", "strcpy_bad")]
        self.assertIn("strcpy", f["extra"].get("inlined", ""))
        self.assertIn("strcpy library model", f["message"])
        self.assertTrue(f["extra"]["strict_aliasing"].startswith("off"))
        self.assertIn(f["extra"]["memory"], ("array", "bv"))

    def test_strict_aliasing_is_opt_in(self):
        on = self._run("--strict-aliasing")
        got = self._verdicts(on)
        f = got[("mem_alias.c", "pun_bad")]
        self.assertEqual((f["status"], f["cls"]), (laws.FAILED, "MEM-STRICT-ALIAS"))
        self.assertEqual(got[("mem_alias.c", "char_ok")]["status"], laws.PROVED)
        off = self._verdicts(self.report)
        self.assertEqual(off[("mem_alias.c", "pun_bad")]["status"], laws.PROVED)

    def test_never_merged(self):
        # Law 2: a loop cut at the bound is BOUNDED, never PROVED.
        got = self._verdicts(self.report)
        f = got[("loops.c", "loop_long_bounded")]
        self.assertEqual(f["extra"]["unwind_closed"], "false")
        self.assertEqual(f["extra"]["k_induction"], "step-open")
        self.assertEqual(got[("kinduct.c", "kind_closed")]["extra"]["k_induction"], "closed")

    def test_law9_validation_held_back(self):
        rows = self._pir(self.report)
        got = self._verdicts(self.report)
        self.assertEqual(got[("overflow.c", "add_ok")]["extra"]["tv"], "NOTRUN (needs --allow-exec)")
        notes = [r for r in rows if r["status"] == laws.NOTRUN]
        self.assertEqual(len(notes), 1)
        self.assertEqual(notes[0]["extra"]["reason"], "executes-scanned-code")

    @unittest.skipUnless(shutil.which("lli"), "lli not on PATH")
    def test_translation_validation_with_allow_exec(self):
        rep = self._run("--allow-exec")
        got = self._verdicts(rep)
        for k, v in EXPECTED.items():
            f = got[k]
            self.assertEqual(f["status"], v, (k, f.get("message")))
            if v in (laws.NEEDS_HARNESS,):
                continue
            # PASS, or NONE when every input hits the UB (e.g. fold_shift_bad
            # takes no parameters and always shifts into the sign bit).
            tv = f["extra"]["tv"]
            self.assertTrue(tv.startswith("PASS") or tv.startswith("NONE"), (k, tv))
            if v != laws.FAILED:
                self.assertTrue(tv.startswith("PASS"), (k, tv))
        # memory functions: validated against lli, or PARTIAL with the reason
        # (nondet library results, contracts, pointer results, globals); never diverged
        for k, (st, _) in MEM_EXPECTED.items():
            f = got[k]
            self.assertEqual(f["status"], st, (k, f.get("message")))
            if st in (laws.NEEDS_HARNESS,):
                continue
            tv = f["extra"]["tv"]
            self.assertTrue(tv.startswith(("PASS", "NONE", "PARTIAL")), (k, tv))
        # roadmap 2.6 functions: validated or PARTIAL/NONE with the reason; never diverged
        for k, (st, _) in PIR3_EXPECTED.items():
            f = got[k]
            self.assertEqual(f["status"], st, (k, f.get("message")))
            if st == laws.NEEDS_HARNESS:
                continue
            self.assertTrue(f["extra"]["tv"].startswith(("PASS", "NONE", "PARTIAL")), (k, f["extra"]["tv"]))
        for k in [("fp_arith.c", "fp_add_ok"), ("eh_catch.cpp", "eh_cleanup_ok"), ("sjlj_basic.c", "sjlj_ok"),
                  ("virt_dispatch.cpp", "virt_ok"), ("coro_gen.cpp", "coro_ok")]:
            self.assertTrue(got[k]["extra"]["tv"].startswith("PASS"), (k, got[k]["extra"]["tv"]))
        for k in [("mem_array.c", "arr_read_ok"), ("mem_array.c", "uninit_mem_ok"), ("mem_ptr.c", "one_past_ok"),
                  ("mem_str.c", "memcpy_ok")]:
            self.assertTrue(got[k]["extra"]["tv"].startswith("PASS"), (k, got[k]["extra"]["tv"]))


if __name__ == "__main__":
    unittest.main()
