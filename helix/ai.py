"""Qwen 3.5 9B inference backends.

Order:
  1. llama.cpp HTTP server (HELIX_LLAMA_SERVER, default :8080)
  2. Ollama /api/chat on qwen3.5:9b — current GGUF host on the 3090
  3. llama-cpp-python (optional) if the package imports and HELIX_GGUF loads

Ollama is the primary backend today. llama-cpp-python is an optional local
loader for the same GGUF blob; it never downloads models. On Windows,
CUDA offload via llama-cpp-python needs an MSVC-built wheel — without
``cl`` on PATH we load CPU-only (n_gpu_layers=0) instead of pretending
CUDA works.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
import shutil
import sys
import urllib.error
import urllib.request
from typing import Any

from helix.config import Config


@dataclass
class ChatResult:
    text: str
    backend: str
    raw: dict[str, Any] = field(default_factory=dict)
    error: str | None = None


class LlamaEngine:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self._llama = None
        self.backend = "none"
        self._bind()

    def _bind(self) -> None:
        if self._ok(self.cfg.llama_server + "/health") or self._ok(
            self.cfg.llama_server + "/v1/models"
        ):
            self.backend = "llama-server"
            return
        if self._ok(self.cfg.ollama_host + "/api/tags"):
            self.backend = "ollama-llamacpp"
            return
        if self._try_llama_cpp_bindings():
            return
        self.backend = "none"

    @staticmethod
    def _llama_cpp_importable() -> bool:
        try:
            import llama_cpp  # type: ignore  # noqa: F401
            return True
        except ImportError:
            return False

    @staticmethod
    def _llama_n_gpu_layers() -> int:
        """CUDA llama.cpp on Windows needs MSVC; do not pretend otherwise."""
        if sys.platform == "win32" and not shutil.which("cl"):
            return 0
        raw = os.environ.get("HELIX_N_GPU_LAYERS", "-1")
        try:
            return int(raw)
        except ValueError:
            return -1

    def _try_llama_cpp_bindings(self) -> bool:
        if not self._llama_cpp_importable():
            return False
        gguf = self.cfg.gguf
        if not (gguf.exists() and gguf.stat().st_size > 1_000_000):
            return False
        try:
            from llama_cpp import Llama  # type: ignore
            self._llama = Llama(
                model_path=str(gguf),
                n_gpu_layers=self._llama_n_gpu_layers(),
                n_ctx=min(8192, 32768),
                verbose=False,
            )
            self.backend = "llama.cpp"
            return True
        except Exception:
            self._llama = None
            return False

    def available(self) -> bool:
        return self.backend != "none"

    def complete(self, messages: list[dict[str, str]], *, timeout: float = 180.0) -> ChatResult:
        if self.backend == "none":
            return ChatResult("", "none", error="llama.cpp not loaded and Ollama not reachable")
        if self._llama is not None:
            return self._via_bindings(messages)
        if self.backend == "llama-server":
            return self._via_openai(self.cfg.llama_server + "/v1/chat/completions",
                                    messages, timeout, "llama-server")
        return self._via_ollama(messages, timeout)

    def _via_bindings(self, messages: list[dict[str, str]]) -> ChatResult:
        out = self._llama.create_chat_completion(messages=messages, temperature=0.2)
        text = out["choices"][0]["message"]["content"] or ""
        return ChatResult(text, "llama.cpp", raw=out)

    def _via_openai(self, url: str, messages: list[dict[str, str]], timeout: float, name: str) -> ChatResult:
        body = json.dumps({
            "model": self.cfg.model,
            "messages": messages,
            "temperature": 0.2,
        }).encode()
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = json.loads(resp.read().decode())
        except Exception as ex:
            return ChatResult("", name, error=str(ex))
        text = raw.get("choices", [{}])[0].get("message", {}).get("content") or ""
        return ChatResult(text, name, raw=raw)

    def _via_ollama(self, messages: list[dict[str, str]], timeout: float) -> ChatResult:
        url = self.cfg.ollama_host.rstrip("/") + "/api/chat"
        body = json.dumps({
            "model": self.cfg.model,
            "messages": messages,
            "stream": False,
            "think": False,
            "options": {"temperature": 0.2, "num_ctx": 8192},
        }).encode()
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = json.loads(resp.read().decode())
        except urllib.error.HTTPError as ex:
            return ChatResult("", "ollama-llamacpp", error=f"HTTP {ex.code}: {ex.read()[:400]!r}")
        except Exception as ex:
            return ChatResult("", "ollama-llamacpp", error=str(ex))
        text = (raw.get("message") or {}).get("content") or ""
        return ChatResult(text, "ollama-llamacpp", raw=raw)

    @staticmethod
    def _ok(url: str) -> bool:
        try:
            urllib.request.urlopen(url, timeout=1.5)
            return True
        except Exception:
            return False


SYSTEM_AUDITOR = (
    "You are Helix, a code auditor. You READ code. You never claim a proof. "
    "Every defect you name is a HYPOTHESIS that must be checked by BMC, a "
    "fuzzer, or a human. Reply with JSON only: "
    '{"hypotheses":[{"function":"...","line":0,"cls":"INT-SIGNED-OVF","why":"..."}]}'
)

SYSTEM_FUZZ4ALL = (
    "You distill a fuzzing prompt. Given C source, reply JSON: "
    '{"prompt":"...", "seeds":["hexbytes", "..."]}. Seeds are little-endian '
    "argument encodings as lowercase hex. No prose."
)

SYSTEM_CHATFUZZ = (
    "Coverage has stalled. Given the function and a hex seed, reply JSON: "
    '{"mutants":["hex", "..."]} semantically valid argument encodings. No prose.'
)

SYSTEM_DAFNY = (
    "Propose Dafny-style contracts for a C function. JSON: "
    '{"requires":["..."],"ensures":["..."],"invariant":["..."],"decreases":["..."]}. '
    "These are hypotheses."
)

SYSTEM_REPAIR = (
    "You receive C source plus compiler/sanitizer/BMC output. Reply with "
    "a full corrected C file only, no markdown fences unless the file itself "
    "needs them. Preserve function names."
)

SYSTEM_HARNESS = (
    "Write a complete C file that includes the target as a string in comments "
    "and a main() that tests the stated property. No markdown."
)


def extract_json(text: str) -> Any | None:
    text = text.strip()
    if text.startswith("```"):
        text = text.strip("`")
        text = text.split("\n", 1)[-1]
        if text.endswith("```"):
            text = text[: text.rfind("```")]
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        a, b = text.find("{"), text.rfind("}")
        if a >= 0 and b > a:
            try:
                return json.loads(text[a : b + 1])
            except json.JSONDecodeError:
                return None
    return None
