"""Roadmap 9.2 / 9.3 / 4.2 in the C++ engine (D8: the Python engine only lists
the review stage and records NOTRUN).

* Lean proof search (`prism prove`): a fake prover served over HTTP (the path
  a real llama-server takes, with the GBNF "grammar" parameter) proposes a
  wrong proof, then a right one; the real Lean kernel rejects the first and
  `lake build` gates the second; --write writes it back and extends the
  lemma library; the audit log has every call.
* The review stage: VACUOUS-ASSUMPTION without a model (Z3), PROOF-REGRESSION
  from the proof store after an edit, drafted contracts with requirement
  trace links that stay HYPOTHESIS until approved (fake model over HTTP).
* Parity locks: stage order, the verdict audit table, taxonomy classes, the
  shipped grammars.

The end-to-end cases need the C++ binary (PRISM_BIN or build/prism) and, for
the Lean search, `lake` (elan).
"""

from __future__ import annotations

import http.server
import json
import os
import shutil
import socketserver
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

from prism import laws
from prism.pipeline import STAGE_ORDER, run_review_notrun
from prism.taxonomy import CLASSES

ROOT = Path(__file__).resolve().parents[1]
NEW_GRAMMARS = ("lean_proof", "assumption_audit", "contract")


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    cands += [ROOT / d / "prism" for d in ("build", "build_wsl")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def _lake() -> str | None:
    if os.environ.get("PRISM_LAKE"):
        return os.environ["PRISM_LAKE"]
    found = shutil.which("lake")
    if found:
        return found
    home = Path.home() / ".elan" / "bin" / "lake"
    return str(home) if home.is_file() else None


class TestParity(unittest.TestCase):
    def test_grammars_embedded_verbatim(self):
        inc = (ROOT / "src" / "prism" / "ai" / "grammars.inc").read_text(encoding="utf-8")
        for name in NEW_GRAMMARS:
            text = (ROOT / "grammars" / f"{name}.gbnf").read_text(encoding="utf-8")
            self.assertIn(f'GBNF_{name.upper()} = R"GBNF({text})GBNF"', inc,
                          f"grammars/{name}.gbnf drifted: run tools/gen_ai_grammars.py")
        gen = (ROOT / "tools" / "gen_ai_grammars.py").read_text(encoding="utf-8")
        for name in NEW_GRAMMARS:
            self.assertIn(f'"{name}"', gen)

    def test_contract_grammar_has_trace_links(self):
        text = (ROOT / "grammars" / "contract.gbnf").read_text(encoding="utf-8")
        self.assertIn('trace  ::= " // from "', text)

    def test_review_stage_order_and_python_notrun(self):
        hpp = (ROOT / "include" / "prism" / "pipeline.hpp").read_text(encoding="utf-8")
        self.assertIn('"harness", "review", "concolic"', hpp)
        i = STAGE_ORDER.index("review")
        self.assertEqual(STAGE_ORDER[i - 1], "harness")
        self.assertEqual(STAGE_ORDER[i + 1], "concolic")
        rows = run_review_notrun()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, laws.NOTRUN)
        self.assertIn("C++ engine only", rows[0].message)
        self.assertIn('stage("review"', (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8"))

    def test_review_is_in_the_verdict_audit_table(self):
        self.assertEqual(laws.STAGE_ORIGIN["review"], laws.ORIGIN_SOLVER)
        lean = (ROOT / "proofs" / "Prism" / "Verdict.lean").read_text(encoding="utf-8")
        self.assertIn('review => "review"', lean)
        cpp = (ROOT / "src" / "prism" / "verdict" / "verdict.cpp").read_text(encoding="utf-8")
        self.assertIn('"harness", "review", "concolic"', cpp)
        tables = json.loads((ROOT / "tests" / "data" / "verdict_tables.json").read_text(encoding="utf-8"))
        self.assertIn("review", json.dumps(tables))

    def test_taxonomy_classes_in_both_engines(self):
        ids = {c["id"] for c in CLASSES}
        cpp = (ROOT / "src" / "prism" / "taxonomy.cpp").read_text(encoding="utf-8")
        for cls in ("VACUOUS-ASSUMPTION", "PROOF-REGRESSION"):
            self.assertIn(cls, ids)
            self.assertIn(f'{{"{cls}"', cpp)


class TestProveDriver(unittest.TestCase):
    def _driver(self):
        import importlib.util

        spec = importlib.util.spec_from_file_location("prism_prove", ROOT / "tools" / "prism_prove.py")
        assert spec and spec.loader
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def test_finds_sorry_theorems_ignoring_comments(self):
        mod = self._driver()
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "X.lean"
            p.write_text(
                "-- theorem ghost : True := sorry\n"
                "/- theorem ghost2 : True := sorry -/\n"
                "theorem a (n : Nat) : n = n := by\n  sorry\n"
                "theorem b (n : Nat) : n = n := rfl\n"
                "private theorem c : True := sorry\n", encoding="utf-8")
            self.assertEqual(mod.sorry_theorems(p), ["a", "c"])

    def test_repository_proofs_have_no_sorry(self):
        # proofs/check.sh forbids sorry; the driver agrees there is nothing to search.
        mod = self._driver()
        self.assertEqual([t for p in mod.lean_files(list(mod.DEFAULT_ROOTS)) for t in mod.sorry_theorems(p)], [])


class _FakeServer(http.server.BaseHTTPRequestHandler):
    """llama-server double: /health, /completion with a scripted reply queue."""

    replies: list[str] = []
    by_prompt: list[tuple[str, str]] = []
    log: list[dict] = []

    def log_message(self, *args):  # noqa: D401 - silence
        return

    def _send(self, obj: dict) -> None:
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - http.server API
        self._send({"status": "ok", "data": []})

    def do_POST(self):  # noqa: N802 - http.server API
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        type(self).log.append({"path": self.path, "req": req})
        if self.path == "/v1/chat/completions":
            self._send({"choices": [{"message": {"content": "{\"hypotheses\": []}"}}]})
            return
        prompt = req.get("prompt", "")
        for key, reply in type(self).by_prompt:
            if key in prompt:
                self._send({"content": reply})
                return
        queue = type(self).replies
        self._send({"content": queue.pop(0) if queue else ""})


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class _Served:
    def __init__(self, replies: list[str], by_prompt: list[tuple[str, str]] | None = None) -> None:
        _FakeServer.replies = list(replies)
        _FakeServer.by_prompt = list(by_prompt or [])
        _FakeServer.log = []
        self.server = _Server(("127.0.0.1", 0), _FakeServer)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> str:
        self.thread.start()
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def __exit__(self, *exc) -> None:
        self.server.shutdown()
        self.server.server_close()


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestProveCli(unittest.TestCase):
    def _project(self, td: Path) -> Path:
        proj = td / "proj"
        proj.mkdir()
        shutil.copy(ROOT / "proofs" / "lean-toolchain", proj / "lean-toolchain")
        (proj / "lakefile.toml").write_text(
            'name = "t"\nversion = "0.1.0"\ndefaultTargets = ["T"]\n\n[[lean_lib]]\nname = "T"\n', encoding="utf-8")
        (proj / "T.lean").write_text(
            "theorem t (a b : Nat) : a + b = b + a := by\n  sorry\n", encoding="utf-8")
        return proj

    def _prove(self, args: list[str], env: dict | None = None) -> subprocess.CompletedProcess:
        exe = _cpp_prism()
        assert exe is not None
        e = dict(os.environ)
        e.update(env or {})
        return subprocess.run([str(exe), "prove", *args], env=e, capture_output=True, text=True, timeout=900)

    def test_without_allow_exec_is_notrun(self):
        with tempfile.TemporaryDirectory() as t:
            proj = self._project(Path(t))
            r = self._prove([str(proj / "T.lean"), "t", "--out", str(Path(t) / "out"), "--json"])
            self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
            j = json.loads(r.stdout)
            self.assertEqual(j["status"], laws.NOTRUN)
            self.assertIn("--allow-exec", j["reason"])
            self.assertIn("sorry", (proj / "T.lean").read_text(encoding="utf-8"))

    def test_list_theorems_with_sorry(self):
        with tempfile.TemporaryDirectory() as t:
            proj = self._project(Path(t))
            r = self._prove([str(proj / "T.lean"), "--list"])
            self.assertEqual(r.stdout.split(), ["t"])

    def test_no_prover_is_notrun(self):
        with tempfile.TemporaryDirectory() as t:
            proj = self._project(Path(t))
            r = self._prove([str(proj / "T.lean"), "t", "--allow-exec", "--json", "--out", str(Path(t) / "out"),
                             "--prover-gguf", str(Path(t) / "missing.gguf")],
                            env={"PRISM_PROVER_SERVER": "", "HOME": t})
            self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
            self.assertEqual(json.loads(r.stdout)["status"], laws.NOTRUN)

    @unittest.skipUnless(_lake(), "Lean (lake) not installed")
    def test_fake_prover_wrong_then_right_real_kernel(self):
        with tempfile.TemporaryDirectory() as t:
            td = Path(t)
            proj = self._project(td)
            lemmas = td / "lemmas.jsonl"
            replies = ['{"tactics": ["rfl"]}', '{"tactics": ["omega"]}']
            with _Served(replies) as url:
                r = self._prove([str(proj / "T.lean"), "t", "--allow-exec", "--write", "--json",
                                 "--out", str(td / "out"), "--lemmas", str(lemmas), "--budget", "4"],
                                env={"PRISM_PROVER_SERVER": url, "PRISM_LAKE": _lake() or ""})
                log = list(_FakeServer.log)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            j = json.loads(r.stdout)
            self.assertEqual(j["status"], laws.PROVED)
            self.assertEqual(j["proof"], ["omega"])
            self.assertEqual(j["rejected_kernel"], 1)
            self.assertTrue(j["written"])
            self.assertIn(":= by\n  omega\n", (proj / "T.lean").read_text(encoding="utf-8"))
            lib = [json.loads(x) for x in lemmas.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([x["theorem"] for x in lib], ["t"])
            # The grammar reached the server; the second prompt carried the Lean error.
            completions = [x for x in log if x["path"] == "/completion"]
            self.assertEqual(len(completions), 2)
            self.assertIn("root   ::= ", completions[0]["req"]["grammar"])
            self.assertIn("tactics", completions[0]["req"]["grammar"])
            self.assertIn("LEAN ERROR", completions[1]["req"]["prompt"])
            audit = [json.loads(x) for x in (td / "out" / "ai_audit.jsonl").read_text().splitlines() if x.strip()]
            self.assertEqual([a["feature"] for a in audit], ["lean-proof", "lean-proof"])
            self.assertEqual(audit[-1]["checker_result"], "accepted")
            self.assertEqual(audit[-1]["id"], j["ai_audit_id"])


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestReviewStage(unittest.TestCase):
    STAGES = "inventory,classify,contracts,wp,bmc,harness,review"

    def _run(self, src: Path, out: Path, extra: list[str] | None = None, env: dict | None = None) -> dict:
        exe = _cpp_prism()
        assert exe is not None
        e = dict(os.environ)
        e.update({"PRISM_LLAMA_SERVER": "http://127.0.0.1:1", "OLLAMA_HOST": "http://127.0.0.1:1",
                  "PRISM_GGUF": "/nonexistent/prism-test.gguf"})
        e.update(env or {})
        subprocess.run([str(exe), str(src), "--out", str(out), "--stage", self.STAGES, *(extra or [])],
                       env=e, check=False, capture_output=True, timeout=900)
        return json.loads((out / "report.json").read_text(encoding="utf-8"))

    @staticmethod
    def _review(rep: dict) -> list[dict]:
        for s in rep["stages"]:
            if s["name"] == "review":
                return s.get("findings", [])
        raise AssertionError("no review stage")

    def test_vacuous_requires_and_proof_regression_without_model(self):
        with tempfile.TemporaryDirectory(prefix="prism review ") as t:
            td = Path(t)
            src = td / "src"
            src.mkdir()
            (src / "vac.c").write_text(
                "int never(int n) {\n    // requires: n > 10 && n < 5\n    // ensures: result >= 0\n    return n;\n}\n",
                encoding="utf-8")
            (src / "inc.c").write_text(
                "int inc(int n) {\n    // requires: n >= 0\n    // ensures: result >= 0\n"
                "    if (n > 100) return 0;\n    return n + 1;\n}\n", encoding="utf-8")
            out = td / "out"
            rep = self._run(src, out)
            rows = self._review(rep)
            vac = [f for f in rows if f["cls"] == "VACUOUS-ASSUMPTION"]
            self.assertEqual(len(vac), 1, rows)
            self.assertEqual(vac[0]["status"], laws.FAILED)
            self.assertEqual(vac[0]["function"], "never")
            self.assertTrue(any(f["status"] == laws.NOTRUN for f in rows))
            store = json.loads((out / "proof_store.json").read_text(encoding="utf-8"))
            self.assertIn("inc", [e["function"] for e in store["functions"]])
            self.assertFalse([f for f in rows if f["cls"] == "PROOF-REGRESSION"])

            # The edit breaks the proved contract: the stored proof no longer checks.
            (src / "inc.c").write_text(
                "int inc(int n) {\n    // requires: n >= 0\n    // ensures: result >= 0\n"
                "    if (n > 100) return 0;\n    return n - 1;\n}\n", encoding="utf-8")
            rep2 = self._run(src, out)
            reg = [f for f in self._review(rep2) if f["cls"] == "PROOF-REGRESSION"]
            self.assertEqual(len(reg), 1, self._review(rep2))
            self.assertEqual(reg[0]["status"], laws.UNKNOWN)
            self.assertEqual(reg[0]["function"], "inc")
            self.assertEqual(reg[0]["extra"]["regression"], "true")
            self.assertEqual(reg[0]["extra"]["code_changed"], "true")
            self.assertTrue(reg[0]["extra"]["repair"].startswith("NOTRUN"))

    def test_drafted_contract_hypothesis_until_approved(self):
        with tempfile.TemporaryDirectory(prefix="prism review ") as t:
            td = Path(t)
            src = td / "src"
            src.mkdir()
            (src / "half.c").write_text(
                "int half(int n) { return n / 2; }\n"
                "int use_half(int x) { if (x < 0) return 0; return half(x); }\n", encoding="utf-8")
            req = td / "requirements.md"
            req.write_text("# Spec\n\nThe half function shall only be called with a non-negative n.\n",
                           encoding="utf-8")
            draft = "requires n >= 0; // from R1\nensures \\result <= n; // from code\n"
            with _Served([], by_prompt=[("int half(int n)", draft)]) as url:
                rep = self._run(src, td / "out", ["--requirements", str(req)], env={"PRISM_LLAMA_SERVER": url})
            rows = [f for f in self._review(rep) if f.get("function") == "half"]
            self.assertEqual(len(rows), 1, self._review(rep))
            d = rows[0]
            self.assertEqual(d["status"], laws.HYPOTHESIS)
            self.assertEqual(d["extra"]["proof_status"], laws.PROVED_ASSUMING)
            trace = json.loads(d["extra"]["trace"])
            self.assertEqual(trace[0]["source"], "R1")
            self.assertEqual(Path(trace[0]["file"]).name, "requirements.md")
            self.assertEqual(trace[0]["line"], 3)
            self.assertEqual(json.loads(d["extra"]["callers"]), {"half.c:use_half": "satisfies"})
            for s in rep["stages"]:
                for f in s.get("findings", []):
                    if f.get("function") == "half" and s["name"] == "review":
                        self.assertNotIn(f["status"], {laws.PROVED, laws.PROVED_ASSUMING, laws.PROVED_UNBOUNDED})

            # Approval by a human turns the same clauses into PROVED-ASSUMING (no model needed).
            (src / "contracts.approved.json").write_text(
                json.dumps({"approved": json.loads(d["extra"]["approve_with"])}), encoding="utf-8")
            rep2 = self._run(src, td / "out2")
            ok = [f for f in self._review(rep2) if f.get("function") == "half"]
            self.assertEqual([f["status"] for f in ok], [laws.PROVED_ASSUMING], ok)
            self.assertEqual(ok[0]["extra"]["contract_state"], "approved")


if __name__ == "__main__":
    unittest.main()
