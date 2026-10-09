"""Roadmap 9.1 / 9.3 / 9.4 assistant tools (tools/prism_ai) and Python GUI hooks.

C++ assistant behaviour is in tests/cpp/test_ai_assist.cpp."""

from __future__ import annotations

import json
import os
import re
import tempfile
import unittest
from pathlib import Path

from tools.prism_ai import measure, predict, sched
from tools.prism_ai.gbdt import GBDT, eval_tree, rmse

ROOT = Path(__file__).resolve().parents[1]


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    cands += [ROOT / d / "prism" for d in ("build", "build_wsl")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


class TestGbdt(unittest.TestCase):
    def test_fits_a_step_and_an_interaction(self):
        xs = [[float(a), float(b)] for a in range(10) for b in range(4)]
        ys = [(5.0 if a > 4 else 0.0) + (2.0 if b >= 2 and a < 3 else 0.0) for a, b in
              ([int(x[0]), int(x[1])] for x in xs)]
        g = GBDT(n_trees=80, lr=0.3, depth=3, min_leaf=1).fit(xs, ys)
        self.assertLess(rmse(g, xs, ys), 0.1)
        j = g.to_json()
        g2 = GBDT.from_json(json.loads(json.dumps(j)))
        for x in xs:
            self.assertAlmostEqual(g.predict(x), g2.predict(x))
        node = {"f": 0, "t": 1.0, "l": {"v": -1.0}, "r": {"v": 1.0}}
        self.assertEqual(eval_tree(node, [1.0]), -1.0)
        self.assertEqual(eval_tree(node, [1.5]), 1.0)

    def test_depth_is_bounded(self):
        xs = [[float(i)] for i in range(64)]
        ys = [float(i % 7) for i in range(64)]
        g = GBDT(n_trees=3, depth=3, min_leaf=1).fit(xs, ys)

        def depth(n: dict) -> int:
            return 0 if "v" in n else 1 + max(depth(n["l"]), depth(n["r"]))

        self.assertTrue(all(depth(t) <= 3 for t in g.trees))


class TestPredictTool(unittest.TestCase):
    def test_feature_names_match_cpp(self):
        src = (ROOT / "src" / "prism" / "solver" / "predict.cpp").read_text(encoding="utf-8")

        def names(fn: str) -> list[str]:
            body = src.split(fn, 1)[1].split("return v;", 1)[0]
            return re.findall(r'"([a-z0-9_]+)"', body)

        self.assertEqual(names("query_feature_names()"), predict.QUERY_FEATURES)
        self.assertEqual(names("function_feature_names()"), predict.FUNCTION_FEATURES)

    @staticmethod
    def _solver_logs(n: int = 300) -> list[dict]:
        rows = []
        for i in range(n):
            nodes = 1.5 + (i % 30) / 10.0
            fast_z3 = nodes < 3.0
            rows.append({"file": f"f{i}.c", "bucket": "QF_BV|w32|n1k",
                         "features": [5.0, nodes, 1.0, 0, 0, 0, 0, 0, 0],
                         "runs": {"z3": {"kind": "unsat", "wall_s": 0.05 if fast_z3 else 2.0},
                                  "cadical": {"kind": "unsat", "wall_s": 1.0 if fast_z3 else 0.1}}})
        return rows

    def test_solver_model_beats_history_and_is_enabled(self):
        res = predict.measure_solver(predict.solver_rows(self._solver_logs()), n_trees=30)
        self.assertTrue(res["enabled"], res.get("policies"))
        pol = res["policies"]
        self.assertLess(pol["gbdt"]["seconds"], pol["history"]["seconds"])
        self.assertGreaterEqual(pol["gbdt"]["accuracy"], 0.95)
        model = predict.build_model(res, None)
        self.assertTrue(model["enabled"])
        self.assertEqual(model["query_features"], predict.QUERY_FEATURES)
        self.assertEqual(set(model["solvers"]), {"z3", "cadical"})

    def test_noise_model_stays_off(self):
        rows = self._solver_logs()
        for r in rows:
            h = int(r["file"][1:-2]) * 2654435761 % 97
            r["runs"]["z3"]["wall_s"] = 0.5 + (h % 10) / 100.0
            r["runs"]["cadical"]["wall_s"] = 0.5 + (h % 7) / 100.0
        res = predict.measure_solver(predict.solver_rows(rows), n_trees=30)
        self.assertFalse(res["enabled"])
        model = predict.build_model(res, None)
        self.assertFalse(model["enabled"])
        self.assertNotIn("solvers", model)

    def test_production_log_is_censored_not_trained(self):
        prod = [{"hash": "h1", "bucket": "b", "features": [5, 2, 1, 0, 0, 0, 0, 0, 0],
                 "times": {"z3": 0.1}, "winner": "z3", "raced": ["z3", "cadical"], "wall_s": 0.1}]
        rows = predict.solver_rows(prod)
        self.assertEqual(rows[0]["times"], {"z3": 0.1})
        res = predict.measure_solver(rows)
        self.assertFalse(res["enabled"])

    def test_bound_labels_and_policy(self):
        def rec(i: int, statuses: list[str]) -> dict:
            return {"file": f"b{i}.c", "function": "f", "features": [1, 2, i % 5, 3, 1, i % 3, 1, 0, 32, 3],
                    "runs": [{"unwind": u, "status": s, "seconds": 0.01 * u} for u, s in
                             zip(predict.UNWINDS, statuses)]}
        rows = predict.bound_rows([
            rec(0, ["FAILED"] * 5),
            rec(1, ["BOUNDED", "BOUNDED", "FAILED", "FAILED", "FAILED"]),
            rec(2, ["BOUNDED"] * 5),
            rec(3, ["BOUNDED", "PROVED", "PROVED", "PROVED", "PROVED"]),
            rec(4, ["ERROR"] * 5),
        ])
        self.assertEqual([r["label"] for r in rows], [1, 4, 16, 2])
        self.assertEqual(predict.snap(1.2), 4)
        self.assertEqual(predict.snap(0.0), 1)
        pol = predict._policy(rows, lambda r: 8)
        self.assertEqual(pol["agreement"], 1.0)
        self.assertEqual(pol["failed_functions"], 2)
        pol1 = predict._policy(rows, lambda r: 1)
        self.assertEqual(pol1["agreement"], 0.5)
        self.assertAlmostEqual(pol1["time_to_first_cex"], 0.18, places=6)

    def test_cli_writes_model_and_markdown(self):
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "solve_runs.jsonl"
            log.write_text("\n".join(json.dumps(r) for r in self._solver_logs()), encoding="utf-8")
            out = Path(td) / "model.json"
            md = Path(td) / "m.md"
            self.assertEqual(predict.main(["--solve-log", str(log), "--out", str(out), "--markdown", str(md),
                                           "--trees", "20"]), 0)
            m = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(m["kind"], "prism-gbdt")
            self.assertIn("solver choice", md.read_text(encoding="utf-8"))


class TestBuiltinModel(unittest.TestCase):
    INC = ROOT / "src" / "prism" / "solver" / "predict_default.inc"

    def test_generated_and_consistent(self):
        text = self.INC.read_text(encoding="utf-8")
        m = predict.parse_inc(text)
        self.assertEqual(predict.emit_inc(m), text, "regenerate with predict.py --emit-inc")
        self.assertEqual(m["query_features"], predict.QUERY_FEATURES)
        self.assertEqual(m["kind"], "prism-gbdt")
        self.assertTrue(all(len(p) <= predict.INC_PART for p in re.findall(r'R"JSON\((.*?)\)JSON"', text, re.S)))

    def test_enabled_only_with_a_measured_win(self):
        m = predict.parse_inc(self.INC.read_text(encoding="utf-8"))
        if not m["enabled"]:
            return
        rp = m["metrics"]["replay"]
        self.assertTrue(rp["chosen"])
        self.assertNotIn("bound", m)
        for k in predict.DECIDE_KS:
            per = rp["k"][str(k)]
            noise = max(v["rel"] for key, v in rp["noise"].items() if key != "queries")
            best = min(per["rules"]["total_s"], per["history"]["total_s"])
            self.assertLess(per[rp["chosen"]]["total_s"], (1 - noise) * best)
            self.assertLessEqual(per[rp["chosen"]]["timeouts"], per["rules"]["timeouts"])
        for run in m["metrics"].get("end_to_end", {}).values():
            self.assertEqual(run["disagreements"], 0)
            self.assertLess(run["model"]["total_s"], run["rules"]["total_s"])


class TestSchedReplay(unittest.TestCase):
    @staticmethod
    def _row(runs: dict, key: str = "a.c", T: float = 8.0) -> dict:
        return {"key": key, "sha": "", "bucket": "b", "x": [5.0, 2.0, 1.0, 0, 0, 0, 0, 0, 0], "T": T,
                "runs": runs, "answer": "unsat"}

    def test_rules_replay_matches_the_portfolio_order(self):
        r = self._row({"z3": ("ans", 1.0), "cadical": ("ans", 0.2)})
        est, lead, delay = sched.plan_rules(r)
        self.assertEqual(sched.simulate(r, est, lead, delay, k=2), (0.35, "cadical"))
        self.assertEqual(sched.simulate(r, est, lead, delay, k=1), (1.0, "z3"))

    def test_censored_member_holds_its_core_until_the_timeout(self):
        r = self._row({"z3": ("cens", 8.0), "kissat": ("ans", 0.5)})
        est, lead, delay = sched.plan_rules(r)
        self.assertEqual(sched.simulate(r, est, lead, delay, k=1), (8.0, ""))
        self.assertEqual(sched.simulate(r, est, "kissat", 0.15, k=1), (0.5, "kissat"))
        self.assertEqual(sched.simulate(r, est, lead, delay, k=2), (0.65, "kissat"))

    def test_a_member_that_gives_up_frees_its_core(self):
        r = self._row({"z3": ("ans", 2.0), "bitwuzla": ("gave", 0.01), "sls": ("gave", 0.25)})
        est = {"bitwuzla": 0.0, "sls": 0.1, "z3": 1.0}
        self.assertEqual(sched.simulate(r, est, "bitwuzla", 0.0, k=1), (3.01, "z3"))

    def test_censored_boosting_predicts_above_the_censoring_point(self):
        xs = [[float(i % 2)] for i in range(40)]
        ys = [1.0 if x[0] else 0.0 for x in xs]
        cens = [bool(x[0]) for x in xs]
        plain = GBDT(n_trees=30, min_leaf=1).fit(xs, ys)
        aft = GBDT(n_trees=30, min_leaf=1).fit_censored(xs, ys, cens)
        self.assertLessEqual(plain.predict([1.0]), 1.0 + 1e-9)
        self.assertGreater(aft.predict([1.0]), 1.0)
        self.assertAlmostEqual(aft.predict([0.0]), 0.0, places=1)
        self.assertEqual(GBDT.from_json(aft.to_json()).predict([1.0]), aft.predict([1.0]))

    def test_load_dedups_by_vc_and_splits_by_file(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "s.jsonl"
            recs = [{"file": f, "function": "f", "vc": "p", "sha": sha, "bucket": "b", "timeout_s": 8.0,
                     "features": [5, 2, 1, 0, 0, 0, 0, 0, 0],
                     "runs": {"z3": {"kind": "unsat", "wall_s": 0.1}, "kissat": {"kind": "timeout", "wall_s": 8.0},
                              "sls": {"kind": "unknown", "wall_s": 0.25}}}
                    for f, sha in (("b.c", "s1"), ("a.c", "s1"), ("a.c", "s2"))]
            p.write_text("\n".join(json.dumps(r) for r in recs), encoding="utf-8")
            rows = sched.load([p])
            self.assertEqual([(r["key"], r["sha"]) for r in rows], [("a.c", "s1"), ("a.c", "s2")])
            self.assertEqual(rows[0]["runs"], {"z3": ("ans", 0.1), "kissat": ("cens", 8.0), "sls": ("gave", 0.25)})
        train, test = sched.split([self._row({}, key=f"f{i}.c") for i in range(200)])
        self.assertTrue(train and test)
        self.assertFalse({r["key"] for r in train} & {r["key"] for r in test})

    def test_noise_only_data_enables_nothing(self):
        rows = []
        for i in range(300):
            h = i * 2654435761 % 97
            rows.append({**self._row({"z3": ("ans", 0.05 + (h % 10) / 1000.0),
                                      "cadical": ("ans", 0.05 + (h % 7) / 1000.0)}, key=f"f{i}.c"),
                         "sha": f"s{i}"})
        res = sched.run_all(rows, [1, 2], n_trees=10)
        chosen, why = sched.decide(res, [1, 2])
        self.assertIsNone(chosen, why)

    def test_a_learnable_split_is_found_and_exported(self):
        rows = []
        for i in range(300):
            big = i % 2 == 0
            x = [5.0, 4.0 if big else 1.5, 1.0, 0, 0, 0, 0, 0, 0]
            runs = {"z3": ("cens", 8.0) if big else ("ans", 0.01), "kissat": ("ans", 0.3) if big else ("ans", 0.2)}
            rows.append({"key": f"f{i}.c", "sha": f"s{i}", "bucket": "b", "x": x, "T": 8.0, "runs": runs,
                         "answer": "unsat"})
        res = sched.run_all(rows, [1, 2], n_trees=20)
        chosen, _ = sched.decide(res, [1])
        self.assertIsNotNone(chosen)
        self.assertLess(res["k"]["1"][chosen]["timeouts"], res["k"]["1"]["rules"]["timeouts"])
        res["chosen"], res["why"] = chosen, ""
        model = predict.build_model(None, None, res)
        self.assertTrue(model["enabled"])
        self.assertEqual(set(model["solvers"]), {"z3", "kissat"})


class TestMeasure(unittest.TestCase):
    def test_pairwise(self):
        label = {"a": "x", "b": "x", "c": "y"}
        self.assertEqual(measure.pairwise([["a", "b"], ["c"]], label)["f1"], 1.0)
        r = measure.pairwise([["a"], ["b"], ["c"]], label)
        self.assertEqual(r["recall"], 0.0)
        r = measure.pairwise([["a", "b", "c"]], label)
        self.assertAlmostEqual(r["precision"], 1 / 3, places=3)


class TestGuiAssistant(unittest.TestCase):
    def test_py_gui_has_chat_panel(self):
        py = (ROOT / "prism" / "gui.py").read_text(encoding="utf-8")
        self.assertIn("def _ask(self)", py)
        self.assertIn("QLineEdit()", py)

    def test_py_assistant_reply_is_notrun_without_report_or_binary(self):
        from prism.gui import assistant_reply
        with tempfile.TemporaryDirectory() as td:
            self.assertIn("NOTRUN assistant: no report.json", assistant_reply("failed", Path(td)))
            (Path(td) / "report.json").write_text('{"root": "x", "stages": []}', encoding="utf-8")
            r = assistant_reply("failed", Path(td), prism_bin="/nonexistent/prism")
            if _cpp_prism() is None:
                self.assertIn("NOTRUN assistant: C++ prism binary not found", r)
            self.assertEqual(assistant_reply("  ", Path(td)), "")

    @unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
    def test_py_assistant_reply_runs_prism_ask(self):
        from prism.gui import assistant_reply
        with tempfile.TemporaryDirectory() as td:
            rep = {"root": td, "stages": [{"name": "bmc", "status": "ok", "findings": [
                {"stage": "bmc", "status": "FAILED", "file": "a.c", "function": "f", "line": 1,
                 "cls": "INT-DIV-ZERO", "message": "div0", "strength": "PROVES", "evidence": "",
                 "counterexample": "x=0", "extra": {}}]}]}
            (Path(td) / "report.json").write_text(json.dumps(rep), encoding="utf-8")
            r = assistant_reply("failed division", Path(td), prism_bin=str(_cpp_prism()))
            self.assertIn("query (grammar)", r)
            self.assertIn("bmc#0 FAILED a.c:1", r)


if __name__ == "__main__":
    unittest.main()
