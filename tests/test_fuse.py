"""Mined FuSeBMC goals + Fuzz4All autoprompt / mutate. python -m unittest tests.test_fuse"""

from __future__ import annotations

import os
import unittest
from pathlib import Path
from unittest.mock import patch

from prism import laws
from prism.ai import (
    LLM_SKIP_FUSE_MSG,
    create_prompt_from_source,
    documentation_from_comments,
    fuzz4all_update_strategy,
    pick_best_prompt,
    score_prompt_seeds,
)
from prism.cparse import extract_functions
from prism.fuse import branch_goals, numbered_goals, run_fuse
from prism.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestFuseGoals(unittest.TestCase):
    def test_implicit_else_loop_exit_switch_cases(self):
        f, _ = fn("fuse_goals")
        goals = branch_goals(f)
        self.assertIn("x > 0", goals)
        self.assertIn("!(x > 0)", goals)
        self.assertIn("n > 0", goals)
        self.assertIn("!(n > 0)", goals)
        self.assertIn("(k) == (1)", goals)
        self.assertIn("(k) == (2)", goals)
        self.assertGreaterEqual(len(goals), 6)
        self.assertEqual(len(goals), len(set(goals)))

    def test_goal_counter_names(self):
        f, p = fn("fuse_goals")
        goals = branch_goals(f)
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=clean):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        self.assertEqual(recs[0].status, laws.CLEAN)
        ids = recs[0].extra.get("goal_ids")
        gmap = recs[0].extra.get("goal_map")
        self.assertEqual(ids[0], "GOAL_1")
        self.assertEqual(len(ids), len(goals))
        self.assertEqual(gmap["GOAL_1"], goals[0])
        self.assertIn("not a proof", recs[0].message)

    def test_saturate_else_polarity(self):
        f, _ = fn("saturate")
        goals = branch_goals(f)
        self.assertIn("x > 100", goals)
        self.assertIn("!(x > 100)", goals)
        self.assertIn("x < 0", goals)
        self.assertIn("!(x < 0)", goals)

    def test_goal_ids_match_fusebmc_numbering(self):
        f, _ = fn("fuse_goals")
        goals = branch_goals(f)
        labeled = numbered_goals(f)
        ids = [lab for lab, _ in labeled]
        self.assertEqual(ids, [f"GOAL_{i}" for i in range(1, len(goals) + 1)])
        self.assertEqual(labeled[0][1], goals[0])
        self.assertEqual(len(labeled), len(goals))


class TestFuzz4AllScoring(unittest.TestCase):
    def test_score_unique_valid(self):
        nbytes = 4
        dup = [b"\x00\x00\x00\x00", b"\x00\x00\x00\x00"]
        self.assertEqual(score_prompt_seeds(dup, nbytes), 1)
        two = [b"\x01\x00\x00\x00", b"\x02\x00\x00\x00"]
        self.assertEqual(score_prompt_seeds(two, nbytes), 2)
        self.assertEqual(score_prompt_seeds([b""], nbytes), 0)

    def test_pick_best_prompt(self):
        nbytes = 4
        a = [b"\x00\x00\x00\x00", b"\x00\x00\x00\x00"]
        b = [b"\x01\x00\x00\x00", b"\x02\x00\x00\x00"]
        prompt, seeds, score = pick_best_prompt([("p1", a), ("p2", b)], nbytes)
        self.assertEqual(prompt, "p2")
        self.assertEqual(score, 2)
        self.assertEqual(len(seeds), 2)

    def test_pick_best_empty_is_zero(self):
        prompt, seeds, score = pick_best_prompt([], 4)
        self.assertEqual(prompt, "")
        self.assertEqual(seeds, [])
        self.assertEqual(score, 0)

    def test_update_strategy(self):
        self.assertIn("generate a new encoding", fuzz4all_update_strategy("aa", None, 0))
        self.assertIn("mutate the previous generation", fuzz4all_update_strategy("aa", None, 1))
        self.assertIn("semantically equivalent", fuzz4all_update_strategy("aa", None, 2))
        comb = fuzz4all_update_strategy("aa", "bb", 3)
        self.assertIn("prev=bb", comb)
        self.assertIn("combine", comb)
        self.assertIn("mutate the previous generation", fuzz4all_update_strategy("aa", None, 3))

    def test_documentation_from_comments(self):
        f, p = fn("fuzz4all_docs")
        src = p.read_text(encoding="utf-8")
        docs = documentation_from_comments(src)
        self.assertIn("clamp x into [0, 100]", docs)
        self.assertIn("signed integer", docs)
        prompt = create_prompt_from_source(name=f.name, body=f.body or "", source=src)
        self.assertEqual(prompt["target_api"], "fuzz4all_docs")
        self.assertIn("clamp", prompt["docstring"])
        self.assertTrue(prompt["hw_prompt"])


class TestFuseLlm(unittest.TestCase):
    def test_llm_down_writes_notrun_never_clean(self):
        f, p = fn("saturate")

        class Eng:
            def available(self):
                return False

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=clean):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        notrun = [r for r in recs if r.status == laws.NOTRUN]
        self.assertTrue(notrun)
        self.assertEqual(notrun[0].message, LLM_SKIP_FUSE_MSG)
        self.assertEqual(notrun[0].extra.get("autoprompt"), "NOTRUN")
        self.assertEqual(notrun[0].extra.get("chatfuzz"), "NOTRUN")
        self.assertIn(notrun[0].status, {laws.NOTRUN, laws.HYPOTHESIS})
        self.assertNotEqual(notrun[0].status, laws.CLEAN)
        self.assertNotEqual(notrun[0].status, laws.ERROR)
        self.assertEqual(notrun[0].strength, laws.STRENGTH_READS)
        self.assertFalse(laws.is_proof(notrun[0].status))
        self.assertNotIn(notrun[0].status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED})
        self.assertFalse(any(laws.is_proof(r.status) for r in recs))

    def test_mutate_interesting_on_new_cov(self):
        f, p = fn("saturate")
        called = []

        class Eng:
            def available(self):
                return True

        def fake_mutate(engine, fn_, seed_hex, prev_hex=None, strategy=1, **_kw):
            if strategy == 1:
                called.append(seed_hex)
            return [b"\x03\x00\x00\x00"]

        hit = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 2, "iters": 4, "corpus": 3},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=hit):
                with patch("prism.agent.fuzz4all_seeds", return_value=[]):
                    with patch("prism.agent.fuzz4all_mutate_interesting", side_effect=fake_mutate):
                        with patch("prism.agent.fuzz4all_combine", return_value=[]):
                            recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertEqual(len(called), 1, called)
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertTrue(recs[0].extra.get("fuzz4all_mutate"))
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_engine_none_no_notrun_finding(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=clean):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertNotEqual(recs[0].status, laws.NOTRUN)
        self.assertIn("not a proof", recs[0].message)

    def test_extra_goals_are_goal_n_labels(self):
        f, p = fn("fuse_goals")
        labeled = numbered_goals(f)
        self.assertEqual(labeled[0][0], "GOAL_1")
        self.assertEqual([lab for lab, _ in labeled], [f"GOAL_{i}" for i in range(1, len(labeled) + 1)])
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=clean):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        self.assertEqual(recs[0].extra.get("goals"), [lab for lab, _ in labeled])
        self.assertTrue(all(str(g).startswith("GOAL_") for g in recs[0].extra.get("goals") or []))

    def test_fuzzer_hit_records_new_goal_label(self):
        f, p = fn("saturate")
        # x=101 covers GOAL_1 (x > 100)
        seed = (101).to_bytes(4, "little")
        hit = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 1, "iters": 4, "corpus": 2},
        )

        class Eng:
            def available(self):
                return True

        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=hit):
                with patch("prism.agent.fuzz4all_seeds", return_value=[seed]):
                    with patch("prism.agent.fuzz4all_mutate_interesting", return_value=[]):
                        with patch("prism.agent.fuzz4all_combine", return_value=[]):
                            recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        fuse = recs[0]
        self.assertEqual(fuse.status, laws.CLEAN)
        self.assertIn("GOAL_1", fuse.extra.get("goals") or [])
        self.assertIn("GOAL_1", fuse.extra.get("new_goals") or fuse.extra.get("covered_goals") or [])
        hyps = [r for r in recs if r.status == laws.HYPOTHESIS]
        self.assertTrue(hyps)
        self.assertEqual(hyps[0].strength, laws.STRENGTH_READS)
        self.assertNotEqual(hyps[0].status, laws.CLEAN)

    def test_c_prompt_combine_on_interesting(self):
        f, p = fn("saturate")
        combined = []

        class Eng:
            def available(self):
                return True

        def fake_combine(engine, fn_, seed_hex, prev_hex):
            combined.append((seed_hex, prev_hex))
            return [b"\x04\x00\x00\x00"]

        hit = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 2, "iters": 4, "corpus": 3},
        )
        two = [b"\x01\x00\x00\x00", b"\x02\x00\x00\x00"]
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=hit):
                with patch("prism.agent.fuzz4all_seeds", return_value=two):
                    with patch("prism.agent.fuzz4all_mutate_interesting", return_value=[]):
                        with patch("prism.agent.fuzz4all_combine", side_effect=fake_combine):
                            recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertEqual(len(combined), 1, combined)
        self.assertTrue(recs[0].extra.get("fuzz4all_combine"))
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_distilled_prompt_is_hypothesis_not_clean(self):
        f, p = fn("fuzz4all_docs")

        class Eng:
            def available(self):
                return True

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.HAS_Z3", False):
            with patch("prism.fuse.fuzz_function", return_value=clean):
                with patch("prism.agent.fuzz4all_seeds", return_value=[]):
                    recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        hyps = [r for r in recs if r.status == laws.HYPOTHESIS]
        self.assertTrue(hyps)
        self.assertEqual(hyps[0].strength, laws.STRENGTH_READS)
        self.assertIn("clamp", (hyps[0].extra or {}).get("docstring") or "")
        self.assertNotEqual(hyps[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(hyps[0].status))

    def test_pointer_needs_harness_not_error(self):
        f, p = fn("null_branch")
        self.assertEqual(f.kind, "POINTER")
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1, engine=None)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)
        self.assertIn("POINTER", recs[0].message)
        self.assertFalse(laws.is_proof(recs[0].status))

        class Eng:
            def available(self):
                return False

        recs2 = run_fuse([f], [], p.parent, budget=0.1, iters=1, engine=Eng())
        ptr = [r for r in recs2 if r.status == laws.NEEDS_HARNESS]
        self.assertTrue(ptr)
        self.assertEqual(ptr[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(ptr[0].status, laws.ERROR)
        self.assertIn("POINTER", ptr[0].message)
        auto = [r for r in recs2 if (r.extra or {}).get("autoprompt") == "NOTRUN"]
        self.assertTrue(auto)
        self.assertIn(auto[0].status, {laws.NOTRUN, laws.HYPOTHESIS})
        self.assertNotEqual(auto[0].status, laws.PROVED)
        self.assertFalse(any(r.status == laws.ERROR for r in recs2))
        self.assertFalse(any(laws.is_proof(r.status) for r in recs2))

    def test_afl_missing_compiler_notrun_does_not_steal_greybox_clean(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        notrun = Finding(
            stage="fuse", status=laws.NOTRUN, file=f.file, function=f.name,
            line=f.line, cls="", message="AFL: no C compiler on PATH",
            strength=laws.STRENGTH_FINDS,
            extra={"engine": "afl", "install": "install gcc or clang"},
        )
        env = {**os.environ, "PRISM_AFL": "1"}
        with patch.dict(os.environ, env, clear=True):
            with patch("prism.fuse.HAS_Z3", False):
                with patch("prism.fuse.afl_available", return_value="/fake/afl-fuzz"):
                    with patch("prism.fuse.fuzz_function", return_value=clean):
                        with patch("prism.fuse.run_afl_fuzz", return_value=notrun):
                            recs = run_fuse(
                                [f], [], p.parent, budget=0.2, iters=4, engine=None,
                            )
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertNotEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual((r.extra or {}).get("engine"), "afl")
        self.assertEqual((r.extra or {}).get("afl"), "NOTRUN")
        self.assertEqual((r.extra or {}).get("install"), "install gcc or clang")
        self.assertIn("not a proof", r.message.lower())
        self.assertFalse(laws.is_proof(r.status))

    def test_prism_afl_opt_in_missing_afl_is_notrun_not_engine_afl(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        env = {k: v for k, v in os.environ.items() if k != "PRISM_LIBFUZZER"}
        env["PRISM_AFL"] = "1"
        with patch.dict(os.environ, env, clear=True):
            with patch("prism.fuse.HAS_Z3", False):
                with patch("prism.fuse.afl_available", return_value=None):
                    with patch("prism.fuse.fuzz_function", return_value=clean):
                        with patch("prism.fuse.run_afl_fuzz") as afl:
                            recs = run_fuse(
                                [f], [], p.parent, budget=0.2, iters=4, engine=None,
                            )
        afl.assert_not_called()
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertEqual((r.extra or {}).get("afl"), "NOTRUN")
        self.assertNotEqual((r.extra or {}).get("engine"), "afl")
        self.assertIn("AFLplusplus", (r.extra or {}).get("install") or "")
        self.assertIn("not a proof", r.message.lower())
        self.assertFalse(laws.is_proof(r.status))

    def test_libfuzzer_notrun_does_not_claim_engine_or_steal_greybox(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        notrun = Finding(
            stage="libfuzzer", status=laws.NOTRUN, file=f.file, function=f.name,
            line=f.line, cls="", message="clang not on PATH",
            strength=laws.STRENGTH_FINDS,
            extra={"install": "clang -fsanitize=fuzzer is not vendored (see third_party/SOURCES.md)"},
        )
        env = {k: v for k, v in os.environ.items() if k != "PRISM_AFL"}
        env["PRISM_LIBFUZZER"] = "1"
        with patch.dict(os.environ, env, clear=True):
            with patch("prism.fuse.HAS_Z3", False):
                with patch("prism.fuse.fuzz_function", return_value=clean):
                    with patch("prism.adapters_extra._run_libfuzzer", return_value=notrun):
                        recs = run_fuse(
                            [f], [], p.parent, budget=0.2, iters=4, engine=None,
                        )
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN, r.message)
        self.assertEqual((r.extra or {}).get("libfuzzer"), "NOTRUN")
        self.assertNotEqual((r.extra or {}).get("engine"), "libfuzzer")
        self.assertNotEqual((r.extra or {}).get("engine"), "afl")
        self.assertIn("not a proof", r.message.lower())
        self.assertFalse(laws.is_proof(r.status))


if __name__ == "__main__":
    unittest.main()
