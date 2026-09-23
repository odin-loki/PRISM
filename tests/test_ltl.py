"""Strix-style LTL safety monitors and known liveness approximations.

python -m unittest tests.test_ltl -v
"""

from __future__ import annotations

import inspect
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.cparse import extract_functions
from prism.ltl import (
    F_BOUND,
    check_safety,
    extract_fsm,
    parse_ltl_file,
    run_ltl,
    strix_available,
)

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


_LTL_PLANTS = (TD / "fsm.c", TD / "fsm_recur.c")


def fn(name: str):
    for p in _LTL_PLANTS:
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestLTLSafetyApprox(unittest.TestCase):
    def setUp(self):
        rec, _ = fn("fsm_recur")
        self.recur_fn = rec
        self.recur = extract_fsm(rec.body)
        self.assertIsNotNone(self.recur)
        sink, _ = fn("fsm_step")
        self.sink_fn = sink
        self.sink = extract_fsm(sink.body)
        self.assertIsNotNone(self.sink)
        settle, _ = fn("fsm_settle")
        self.settle_fn = settle
        self.settle = extract_fsm(settle.body)
        self.assertIsNotNone(self.settle)

    def test_gf_approx_holds_is_bounded_not_proved(self):
        f = check_safety("GF (state == LIVE_IDLE)", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertIn("safety_approx", f.extra)
        self.assertEqual(f.extra.get("approx_kind"), "GF")
        self.assertTrue(f.extra.get("strix_not_proved"))
        self.assertIn("not a proof", f.message)

    def test_g_f_unbounded_is_same_approx(self):
        f = check_safety("G F (state == LIVE_IDLE)", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)

    def test_g_paren_f_unbounded_is_approx(self):
        f = check_safety("G (F (state == LIVE_IDLE))", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)

    def test_g_f_k_is_safety_fragment(self):
        """Explicit F_k is the fragment we decide — PROVED is allowed."""
        f = check_safety("G (F_8 (state == LIVE_IDLE))", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.PROVED)
        self.assertEqual(F_BOUND, 8)

    def test_gf_approx_sink_fails(self):
        f = check_safety("GF (state == ST_IDLE)", self.sink)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertEqual(f.extra.get("approx_kind"), "GF")

    def test_gf_plant_run_ltl_not_proved(self):
        recs = run_ltl([self.recur_fn], [TD / "gf_recur.ltl"])
        self.assertTrue(recs)
        self.assertEqual(recs[0].status, laws.BOUNDED)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_fg_approx_holds_is_bounded_not_proved(self):
        f = check_safety("FG (state == LIVE_IDLE)", self.settle)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertEqual(f.extra.get("approx_kind"), "FG")

    def test_fg_approx_cycle_fails(self):
        f = check_safety("F G (state == LIVE_IDLE)", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertNotEqual(f.status, laws.PROVED)

    def test_until_top_level_approx_holds(self):
        f = check_safety("state == LIVE_ACK U state == LIVE_IDLE", self.recur)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertEqual(f.extra.get("approx_kind"), "UNTIL")

    def test_until_real_violation_failed(self):
        f = check_safety("state == ST_WORK U state == ST_IDLE", self.sink)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertIn("¬p ∧ ¬q", f.message)

    def test_g_until_stays_notrun(self):
        """G (p U q) is not a known atomic pattern — keep NOTRUN."""
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "until.ltl"
            spec.write_text("G (state == ST_WORK U state == ST_IDLE)\n", encoding="utf-8")
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_bare_f_stays_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "live.ltl"
            spec.write_text("F (state == ST_IDLE)\n", encoding="utf-8")
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertIn("Strix", recs[0].message)
        self.assertTrue(recs[0].extra.get("strix_not_proved"))
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_nested_gf_until_stays_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "nested.ltl"
            spec.write_text("G F (state == ST_IDLE U state == ST_WORK)\n", encoding="utf-8")
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)

    def test_nested_until_stays_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "nested_u.ltl"
            spec.write_text(
                "state == ST_IDLE U (state == ST_WORK U state == ST_BAD)\n",
                encoding="utf-8",
            )
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.BOUNDED)
        self.assertIsNone(check_safety(
            "state == ST_IDLE U (state == ST_WORK U state == ST_BAD)",
            self.sink,
        ))

    def test_strix_success_is_not_proved(self):
        """Even if a strix binary exists, leftover formulas stay NOTRUN."""
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "live.ltl"
            spec.write_text("F (state == ST_IDLE)\n", encoding="utf-8")
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertTrue(recs[0].extra.get("strix_not_proved"))
        self.assertIn("strix_note", recs[0].extra)
        exe = strix_available()
        if exe:
            self.assertEqual(recs[0].extra.get("strix"), exe)
            self.assertNotEqual(recs[0].status, laws.PROVED)
        else:
            self.assertEqual(recs[0].extra.get("strix"), "")

    def test_strix_binary_is_never_invoked_for_a_proof(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "live.ltl"
            spec.write_text("F (state == ST_IDLE)\n", encoding="utf-8")
            with mock.patch("prism.ltl.strix_available", return_value="/fake/strix"), \
                 mock.patch("prism.ltl.subprocess.run") as run:
                recs = run_ltl([self.sink_fn], [spec])
        run.assert_not_called()
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertTrue(recs[0].extra.get("strix_not_proved"))
        self.assertEqual(recs[0].extra.get("strix"), "/fake/strix")
        self.assertIn("does not treat strix output as PROVED", recs[0].message)

    def test_missing_strix_is_notrun_not_clean(self):
        recs = run_ltl([self.sink_fn], [])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertNotEqual(recs[0].status, laws.PROVED)

    def test_comment_only_spec_is_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "empty.ltl"
            spec.write_text("# G (state != BAD)\n\n", encoding="utf-8")
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertIn("no .ltl spec", recs[0].message)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_comments_only_testdata_is_notrun_never_proved(self):
        spec = TD / "comments_only.ltl"
        self.assertTrue(spec.is_file(), msg="testdata/comments_only.ltl is required")
        text = spec.read_text(encoding="utf-8")
        live = [
            ln.strip() for ln in text.splitlines()
            if ln.strip() and not ln.strip().startswith("#") and not ln.strip().startswith("//")
        ]
        self.assertEqual(live, [], msg="comments_only.ltl must contain only # / // lines")
        self.assertIn("#", text)
        self.assertIn("//", text)
        self.assertEqual(parse_ltl_file(spec), [])
        recs = run_ltl([self.sink_fn], [spec])
        self.assertTrue(recs)
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertIn("no .ltl spec", recs[0].message)
        for r in recs:
            self.assertEqual(r.status, laws.NOTRUN, r.message)
            self.assertNotEqual(r.status, laws.PROVED, r.message)
            self.assertNotEqual(r.status, laws.CLEAN, r.message)
            self.assertFalse(laws.is_proof(r.status), r.status)

    def test_slash_slash_comment_only_spec_is_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "slash.ltl"
            spec.write_text("// G (state != BAD)\n\n", encoding="utf-8")
            self.assertEqual(parse_ltl_file(spec), [])
            recs = run_ltl([self.sink_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_g_implies_state_pred_is_invariant(self):
        f = check_safety(
            "G (state == ST_IDLE -> (state == ST_IDLE || state == ST_WORK))",
            self.sink,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.PROVED)

    def test_g_implies_state_pred_fails(self):
        f = check_safety("G (state == ST_IDLE -> state == ST_WORK)", self.sink)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertNotEqual(f.status, laws.PROVED)

    def test_unbounded_req_f_is_bounded_not_proved(self):
        f = check_safety(
            "G (state == LIVE_IDLE -> F (state == LIVE_ACK))",
            self.recur,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.BOUNDED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertEqual(f.extra.get("approx_kind"), "F")

    def test_unbounded_req_f_sink_fails(self):
        f = check_safety(
            "G (state == ST_WORK -> F (state == ST_IDLE))",
            self.sink,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertEqual(f.extra.get("approx_kind"), "F")

    def test_safety_without_fsm_is_notrun(self):
        ev_fn, _ = fn("fsm_ev_only")
        self.assertIsNone(extract_fsm(ev_fn.body))
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "g.ltl"
            spec.write_text("G (state != BAD)\n", encoding="utf-8")
            recs = run_ltl([ev_fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertIn("switch(state)", recs[0].message)


class TestExtractFsm(unittest.TestCase):
    def test_switch_state_not_switch_ev(self):
        ev_fn, _ = fn("fsm_ev_only")
        self.assertIsNone(extract_fsm(ev_fn.body))
        rec, _ = fn("fsm_recur")
        self.assertIsNotNone(extract_fsm(rec.body))

    def test_fallthrough_shares_dest(self):
        fall, _ = fn("fsm_fall")
        fsm = extract_fsm(fall.body)
        self.assertIsNotNone(fsm)
        trans = set(map(tuple, fsm["transitions"]))
        self.assertIn(("FT_REQ", "FT_ACK"), trans)
        self.assertIn(("FT_RETRY", "FT_ACK"), trans)
        self.assertNotIn(("FT_REQ", "FT_REQ"), trans)
        f = check_safety(
            "G (state == FT_REQ -> X (state == FT_ACK))",
            fsm,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.PROVED)

    def test_parenthesized_dest(self):
        paren, _ = fn("fsm_paren")
        fsm = extract_fsm(paren.body)
        self.assertIsNotNone(fsm)
        trans = set(map(tuple, fsm["transitions"]))
        self.assertIn(("LIVE_IDLE", "LIVE_ACK"), trans)
        self.assertIn(("LIVE_ACK", "LIVE_IDLE"), trans)
        f = check_safety(
            "G (state == LIVE_IDLE -> X (state == LIVE_ACK))",
            fsm,
        )
        self.assertEqual(f.status, laws.PROVED)

    def test_one_case_plus_default(self):
        dfn, _ = fn("fsm_default")
        fsm = extract_fsm(dfn.body)
        self.assertIsNotNone(fsm)
        trans = set(map(tuple, fsm["transitions"]))
        self.assertIn(("LIVE_IDLE", "LIVE_ACK"), trans)
        self.assertIn(("LIVE_ACK", "LIVE_IDLE"), trans)
        f = check_safety(
            "G (state == LIVE_ACK -> X (state == LIVE_IDLE))",
            fsm,
        )
        self.assertEqual(f.status, laws.PROVED)


class TestCppParseLtlFileSourceContract(unittest.TestCase):
    """C++ parse_ltl_file skips # and // the same way the Python engine does."""

    def test_cpp_parse_ltl_file_skips_hash_and_slash_slash(self):
        src = (ROOT / "src" / "prism" / "stages" / "ltl.cpp").read_text(encoding="utf-8")
        sig = "std::vector<std::string> parse_ltl_file("
        i = src.find(sig)
        self.assertNotEqual(i, -1, msg="parse_ltl_file missing from stages/ltl.cpp")
        brace = src.find("{", i)
        self.assertGreater(brace, i)
        depth = 0
        end = None
        for j, ch in enumerate(src[brace:], brace):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = j
                    break
        self.assertIsNotNone(end)
        body = src[i : end + 1]
        self.assertIn('ln.starts_with("#")', body)
        self.assertIn('ln.starts_with("//")', body)
        self.assertIn("continue", body)
        self.assertNotIn("PROVED", body)

    def test_prism_parse_ltl_file_skips_hash_and_slash_slash(self):
        py = inspect.getsource(parse_ltl_file)
        self.assertIn('startswith("#")', py)
        self.assertIn('startswith("//")', py)


if __name__ == "__main__":
    unittest.main()
