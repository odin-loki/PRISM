"""The repair stage's "verified fix" is a claim about the LLM-written patch,
not about the scanned code (Law 4, docs/VERDICTS.md "The verdict audit").

rlef_repair used to report a patch that BMC proves as PROVED. The repair
stage has the Model origin, so the verdict audit demoted it to UNKNOWN and
wrote an ERROR row. Now the finding is HYPOTHESIS / READS with
extra.patch_verdict = PROVED and the "verified fix" label, so the audit has
nothing to flag. End to end with the C++ binary and a fake llama-server
(the same HTTP path a real one takes). The Python engine's unit test is
tests/test_execute_compile.py::test_bmc_proved_is_terminal_success.
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

ROOT = Path(__file__).resolve().parents[1]

BUGGY = "int div_param(int a, int b) {\n    return a / b;\n}\n"
PATCH = """```c
int div_param(int a, int b) {
    if (b == 0 || (a == -2147483647 - 1 && b == -1)) return 0;
    return a / b;
}
int main(void) { return div_param(4, 2) - 2; }
```"""


def _cpp_prism() -> Path | None:
    env = os.environ.get("PRISM_BIN")
    cands = [Path(env)] if env else []
    cands += [ROOT / d / "prism" for d in ("build", "build_wsl")]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


class _FakeModel(http.server.BaseHTTPRequestHandler):
    chats = 0

    def log_message(self, *args):  # noqa: D401 - silence the default stderr log
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
        if self.path == "/v1/chat/completions":
            _FakeModel.chats += 1
            self._send({"choices": [{"message": {"content": PATCH}}]})
            return
        if '"explanation"' in req.get("grammar", ""):
            self._send({"content": json.dumps({"explanation": "b may be 0", "fix_body": "return 0;"})})
            return
        self._send({"content": "[]"})


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


@unittest.skipUnless(_cpp_prism(), "C++ prism binary not built (set PRISM_BIN)")
class TestRepairVerifiedFixIsAHypothesis(unittest.TestCase):
    def test_patch_proof_is_hypothesis_and_audit_clean(self) -> None:
        if shutil.which("cc") is None and shutil.which("gcc") is None and shutil.which("clang") is None:
            self.skipTest("no C compiler")
        srv = _Server(("127.0.0.1", 0), _FakeModel)
        th = threading.Thread(target=srv.serve_forever, daemon=True)
        th.start()
        try:
            with tempfile.TemporaryDirectory() as tdn:
                td = Path(tdn)
                src = td / "src"
                src.mkdir()
                (src / "div_param.c").write_text(BUGGY, encoding="utf-8")
                out = td / "out"
                env = dict(os.environ)
                env["PRISM_LLAMA_SERVER"] = f"http://127.0.0.1:{srv.server_address[1]}"
                env["OLLAMA_HOST"] = "http://127.0.0.1:1"
                env["PRISM_GGUF"] = "/nonexistent/prism-test.gguf"
                exe = _cpp_prism()
                assert exe is not None
                r = subprocess.run([str(exe), str(src), "--out", str(out), "--allow-exec", "--repair-rounds", "2",
                                    "--stage", "inventory,classify,bmc,repair"],
                                   env=env, capture_output=True, text=True, timeout=900)
                rep = json.loads((out / "report.json").read_text(encoding="utf-8"))
                repair = next(s for s in rep["stages"] if s["name"] == "repair")
                rows = repair["findings"]
                self.assertTrue(rows, r.stdout[-2000:])
                self.assertGreaterEqual(_FakeModel.chats, 1)
                fix = next((f for f in rows if f.get("extra", {}).get("patch_verdict")), None)
                if fix is None:
                    notes = [f["message"] for f in rows]
                    if any("sandbox" in m or "bwrap" in m for m in notes):
                        self.skipTest(f"sandbox could not run the candidate: {notes}")
                    self.fail(f"no verified fix row: {rows}")
                self.assertEqual(fix["status"], laws.HYPOTHESIS)
                self.assertEqual(fix["strength"], laws.STRENGTH_READS)
                self.assertTrue(laws.is_proof(fix["extra"]["patch_verdict"]), fix)
                self.assertEqual(fix["extra"]["fix_label"], "verified fix")
                self.assertIn("verified fix", fix["message"])
                self.assertIn("not the scanned code", fix["message"])
                # The verdict audit had nothing to demote in this stage.
                self.assertFalse([f for f in rows if f.get("extra", {}).get("audit") == "verdict"], rows)
                self.assertNotIn("audit_original", fix.get("extra", {}))
                # No finding of the repair stage claims a proof.
                self.assertFalse([f for f in rows if laws.is_proof(f["status"])])
                # The audit log records the checker's result but no verdict effect.
                recs = [json.loads(x) for x in (out / "ai_audit.jsonl").read_text(encoding="utf-8").splitlines()]
                checked = [x for x in recs if x.get("checker") == "bmc(rlef candidate)"]
                self.assertTrue(checked)
                self.assertEqual(checked[0]["verdict_effect"], "none")
                self.assertEqual(checked[0]["checker_result"], fix["extra"]["patch_verdict"])
        finally:
            srv.shutdown()


if __name__ == "__main__":
    unittest.main()
