"""Roadmap 9.1 / 9.3 / 9.4 assistant features (C++ engine; D8) and their tools.

Locks:

* grammars/ask.gbnf and grammars/draft.gbnf are embedded verbatim;
* tools/prism_ai (pure-Python GBDT, solver/bound prediction trainer,
  measurement) works without dependencies, and its feature names match the
  C++ loader src/prism/solver/predict.cpp;
* a model is exported "enabled" only when it beats the baseline (9.7);
* end to end with the C++ binary (PRISM_BIN): the pipeline writes
  triage.json and a "Clusters" section without changing a status;
  `prism ask` prints the structured query with the answer (grammar without a
  model; a fake llama-server's grammar-constrained query is validated and
  audited); `prism regress` writes tests under <out>/regression_tests only;
  `prism draft` links every claim; the triage embedding endpoint is used when
  PRISM_EMBED_SERVER is set;
* both GUIs have the assistant panel (source contract; Qt is not built here).
"""

from __future__ import annotations

import http.server
import json
import os
import re
import socketserver
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

from tools.prism_ai import measure, predict
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


class TestAssistGrammars(unittest.TestCase):
    def test_embedded_verbatim(self):
        inc = (ROOT / "src" / "prism" / "ai" / "grammars_assist.inc").read_text(encoding="utf-8")
        for name in ("ask", "draft"):
            text = (ROOT / "grammars" / f"{name}.gbnf").read_text(encoding="utf-8")
            self.assertIn("root", text)
            self.assertIn(f'GBNF_{name.upper()} = R"GBNF({text})GBNF"', inc,
                          f"grammars/{name}.gbnf drifted: run tools/gen_ai_grammars.py")

    def test_ask_grammar_statuses_are_the_vocabulary(self):
        from prism import laws
        text = (ROOT / "grammars" / "ask.gbnf").read_text(encoding="utf-8")
        line = next(ln for ln in text.splitlines() if ln.startswith("status "))
        vocab = {v for k, v in vars(laws).items() if k.isupper() and isinstance(v, str)}
        found = re.findall(r'\\"([A-Z-]+)\\"', line)
        self.assertEqual(len(found), 16)
        for st in found:
            self.assertIn(st, vocab)

    def test_draft_grammar_forces_links(self):
        text = (ROOT / "grammars" / "draft.gbnf").read_text(encoding="utf-8")
        claim = next(ln for ln in text.splitlines() if ln.startswith("claim"))
        self.assertIn('"\\"links\\""', claim)
        links = next(ln for ln in text.splitlines() if ln.startswith("links"))
        self.assertIn("link (", links)  # at least one link: an empty list cannot be decoded


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
        # The C++ loader's semantics: left when x[f] <= t.
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
        # Same bucket for all rows (so per-bucket history cannot separate
        # them); z3 is fast on small node counts, cadical on large ones.
        rows = []
        for i in range(n):
            nodes = 1.5 + (i % 30) / 10.0  # log10 nodes 1.5 .. 4.4
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
        for r in rows:  # times independent of the features: nothing to learn
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
        self.assertFalse(res["enabled"])  # incomplete rows are not training data

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
        self.assertEqual(pol1["agreement"], 0.5)  # b1 needs unwind 4, b3 needs 2
        # b0 finds it at 1 (0.01 s); b1 misses at 1 and escalates to 16 (0.01 + 0.16 s)
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


class TestMeasure(unittest.TestCase):
    def test_pairwise(self):
        label = {"a": "x", "b": "x", "c": "y"}
        self.assertEqual(measure.pairwise([["a", "b"], ["c"]], label)["f1"], 1.0)
        r = measure.pairwise([["a"], ["b"], ["c"]], label)
        self.assertEqual(r["recall"], 0.0)
        r = measure.pairwise([["a", "b", "c"]], label)
        self.assertAlmostEqual(r["precision"], 1 / 3, places=3)


class _FakeModelServer(http.server.BaseHTTPRequestHandler):
    """llama-server test double: /completion answers the ask grammar with a
    query JSON and the draft grammar with one linked and one unlinked claim;
    /embedding returns a 2-d vector."""

    log: list[dict] = []

    def log_message(self, *args):  # noqa: D401
        return

    def _send(self, obj: object) -> None:
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        self._send({"status": "ok"})

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        _FakeModelServer.log.append({"path": self.path, "req": req})
        if self.path == "/embedding":
            text = req.get("content", "")
            self._send({"embedding": [1.0, 0.0] if "div" in text else [0.0, 1.0]})
            return
        grammar = req.get("grammar", "")
        if 'statuses' in grammar:
            content = json.dumps({"stages": ["bmc"], "statuses": ["FAILED"], "cls": ["DIV"], "file_glob": "",
                                  "function_glob": "", "text": "", "group_by": "", "count_only": False})
        elif 'links' in grammar:
            content = json.dumps([
                {"section": "Defects", "text": "Dividing by a zero parameter is undefined.",
                 "links": ["verdict:bmc#1"]},
                {"section": "Defects", "text": "Everything else is proved.", "links": ["stage:bmc"]}])
        else:
            content = "{}"
        self._send({"content": content})


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


SRC = """\
int add_big(int x) {
    return x + 100;
}

int divide(int a, int b) {
    return a / b;
}

int ok(int x) {
    return x & 1;
}
"""


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestAssistEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.td = tempfile.TemporaryDirectory()
        td = Path(cls.td.name)
        cls.src = td / "src"
        cls.src.mkdir()
        (cls.src / "calc.c").write_text(SRC, encoding="utf-8")
        cls.out = td / "out"
        cls.exe = str(_cpp_prism())
        env = dict(os.environ, OLLAMA_HOST="http://127.0.0.1:1", PRISM_LLAMA_SERVER="http://127.0.0.1:1")
        env.pop("PRISM_EMBED_SERVER", None)
        cls.env = env
        subprocess.run([cls.exe, str(cls.src), "--out", str(cls.out), "--stage", "inventory,classify,bmc",
                        "--no-llm"], env=env, capture_output=True, timeout=600, check=False)
        cls.report_text = (cls.out / "report.json").read_text(encoding="utf-8")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.td.cleanup()

    def _run(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([self.exe, *args], env=env or self.env, capture_output=True, text=True, timeout=600)

    def test_pipeline_triage_orders_only(self):
        tri = json.loads((self.out / "triage.json").read_text(encoding="utf-8"))
        self.assertEqual(tri["kind"], "prism-triage")
        self.assertIn("NOTRUN", tri["embedder_note"])
        md = (self.out / "report.md").read_text(encoding="utf-8")
        self.assertIn("## Clusters", md)
        self.assertLess(md.index("## Findings"), md.index("## Clusters"))
        rep = json.loads(self.report_text)
        statuses = {f"{s['name']}#{i}": f["status"] for s in rep["stages"] for i, f in enumerate(s["findings"])}
        for c in tri["clusters"]:
            self.assertEqual(c["top_status"], statuses[c["representative"]])
        r = self._run("triage", str(self.out))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.out / "report.json").read_text(encoding="utf-8"), self.report_text)

    def test_ask_prints_query_with_answer(self):
        r = self._run("ask", "failed findings in function divide", "--report", str(self.out), "--no-llm")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('query (grammar): {', r.stdout)
        self.assertIn('"function_glob":"divide"', r.stdout)
        self.assertIn("answer: 1 finding", r.stdout)
        j = json.loads(self._run("ask", "how many failed", "--report", str(self.out), "--json",
                                 "--no-llm").stdout)
        self.assertEqual(j["translator"], "grammar")
        self.assertTrue(j["query"]["count_only"])
        self.assertGreaterEqual(j["count"], 2)
        e = self._run("ask", "explain", "bmc#0", "--report", str(self.out), "--no-llm").stdout
        self.assertIn("what", e)

    def test_ask_with_model_is_validated_and_audited(self):
        srv = _Server(("127.0.0.1", 0), _FakeModelServer)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            env = dict(self.env, PRISM_LLAMA_SERVER=f"http://127.0.0.1:{srv.server_address[1]}")
            j = json.loads(self._run("ask", "division problems from model checking", "--report", str(self.out),
                                     "--json", env=env).stdout)
            self.assertTrue(j["translator"].startswith("llm:"), j)
            self.assertEqual([m["function"] for m in j["matches"]], ["divide"])
            audit = [json.loads(x) for x in (self.out / "ai_audit.jsonl").read_text().splitlines() if x.strip()]
            ask = [a for a in audit if a["feature"] == "ask"]
            self.assertTrue(ask)
            self.assertEqual(ask[-1]["checker_result"], "accepted")
            self.assertEqual(ask[-1]["verdict_effect"], "none")
            # Drafting with the model: the unlinked "proved" claim is rejected.
            d = self._run("draft", "--report", str(self.out), "--out", str(self.out / "d.md"), env=env)
            self.assertEqual(d.returncode, 0, d.stderr)
            dj = json.loads((self.out / "d.json").read_text(encoding="utf-8"))
            self.assertTrue(dj["author"].startswith("llm:"))
            self.assertEqual(len(dj["rejected"]), 1)
            self.assertNotIn("Everything else is proved", (self.out / "d.md").read_text().split("## Rejected")[0])
        finally:
            srv.shutdown()
        self.assertEqual((self.out / "report.json").read_text(encoding="utf-8"), self.report_text)

    def test_triage_uses_embedding_endpoint(self):
        srv = _Server(("127.0.0.1", 0), _FakeModelServer)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            env = dict(self.env, PRISM_EMBED_SERVER=f"http://127.0.0.1:{srv.server_address[1]}")
            with tempfile.TemporaryDirectory() as td:
                out = Path(td)
                (out / "report.json").write_text(self.report_text, encoding="utf-8")
                r = self._run("triage", str(out), env=env)
                self.assertEqual(r.returncode, 0, r.stderr)
                tri = json.loads((out / "triage.json").read_text(encoding="utf-8"))
                self.assertIn("llama-server-embedding", tri["embedder"])
        finally:
            srv.shutdown()

    def test_regress_writes_under_out_only(self):
        r = self._run("regress", "--report", str(self.out / "report.json"))
        self.assertEqual(r.returncode, 0, r.stderr)
        m = json.loads((self.out / "regression_tests" / "manifest.json").read_text(encoding="utf-8"))
        written = [t for t in m["tests"] if t["status"] == "written"]
        self.assertEqual({t["function"] for t in written}, {"add_big", "divide"})
        self.assertEqual(sorted(p.name for p in self.src.iterdir()), ["calc.c"])
        # --run without --allow-exec executes nothing (Law 9).
        r = self._run("regress", "--report", str(self.out / "report.json"), "--run")
        m = json.loads((self.out / "regression_tests" / "manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(all(t["status"] == "NOTRUN" for t in m["tests"] if t["name"]))

    def test_draft_template_links_every_claim(self):
        r = self._run("draft", "--report", str(self.out), "--kind", "assurance", "--proofs", str(ROOT / "proofs"),
                      "--no-llm")
        self.assertEqual(r.returncode, 0, r.stderr)
        d = json.loads((self.out / "draft_assurance.json").read_text(encoding="utf-8"))
        self.assertEqual(d["rejected"], [])
        self.assertTrue(all(c["links"] for s in d["sections"] for c in s["claims"]))
        self.assertGreater(d["theorems_indexed"], 20)
        md = (self.out / "draft_assurance.md").read_text(encoding="utf-8")
        self.assertIn("[theorem:Prism.proved_bounded_never_merge]", md)


class TestGuiAssistant(unittest.TestCase):
    def test_cpp_gui_has_chat_panel(self):
        cpp = (ROOT / "src" / "gui" / "MainWindow.cpp").read_text(encoding="utf-8")
        hdr = (ROOT / "src" / "gui" / "MainWindow.h").read_text(encoding="utf-8")
        self.assertIn("void onAsk();", hdr)
        self.assertIn("prism::ai::assistant_reply", cpp)
        self.assertIn("cfg.resume = true;", cpp.split("void MainWindow::onAsk", 1)[1])
        self.assertIn("NOTRUN assistant", cpp)
        self.assertIn("cellDoubleClicked", cpp)

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
