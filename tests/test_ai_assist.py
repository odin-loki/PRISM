"""Roadmap 9.1 / 9.3 / 9.4 assistant features (C++ engine; D8) and their tools.

Locks:

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

ROOT = Path(__file__).resolve().parents[1]


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    cands += [ROOT / d / "prism" for d in ("build", "build_wsl")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


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
