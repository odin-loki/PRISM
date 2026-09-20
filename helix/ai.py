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
import re
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

# Fuzz4All Target.AP_SYSTEM_MESSAGE / AP_INSTRUCTION (target.py).
AP_SYSTEM_MESSAGE = "You are an auto-prompting tool"
AP_INSTRUCTION = (
    "Please summarize the above documentation in a concise manner to describe the usage and "
    "functionality of the target "
)

SYSTEM_FUZZ4ALL_MUTATE = (
    "The previous generation was interesting. Mutate it into a new valid encoding. "
    "Given the function and a hex seed, reply JSON: "
    '{"mutants":["hex", "..."]}. No prose.'
)

# Fuzz4All Target.c_prompt (target.py): combine two previous generations.
SYSTEM_FUZZ4ALL_COMBINE = (
    "Combine the two previous interesting encodings into one valid encoding. "
    "Reply JSON: "
    '{"mutants":["hex", "..."]}. No prose.'
)

SYSTEM_CHATFUZZ = (
    "Coverage has stalled. Given the function and a hex seed, reply JSON: "
    '{"mutants":["hex", "..."]} semantically valid argument encodings. No prose.'
)

LLM_UNAVAILABLE_MSG = "llama.cpp/Ollama not reachable"
LLM_SKIP_FUSE_MSG = "llama.cpp/Ollama not reachable; stall mutants / autoprompt skipped"
LLM_INSTALL = "ollama serve  (qwen3.5:9b) or build native/llama.cpp"


def llm_complete_unavailable(err: str | None) -> bool:
    """HTTP / connection failure is a missing backend, never a code ERROR."""
    text = (err or "").lower()
    if not text:
        return False
    keys = (
        "http error",
        "http ",
        "urlopen",
        "urlerror",
        "connection refused",
        "connection reset",
        "connection aborted",
        "not reachable",
        "not loaded",
        "failed to connect",
        "name or service not known",
        "winerror",
        "errno 111",
        "errno 104",
    )
    return any(k in text for k in keys)

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


def parse_hex_seeds(data: Any, key: str = "seeds") -> list[bytes]:
    """Parse lowercase/0x hex blobs from an LLM JSON object. Invalid entries drop."""
    out: list[bytes] = []
    if not isinstance(data, dict):
        return out
    for h in data.get(key) or []:
        try:
            raw = bytes.fromhex(str(h).strip().replace("0x", "").replace("0X", ""))
        except ValueError:
            continue
        if raw:
            out.append(raw)
    return out


def score_prompt_seeds(seeds: list[bytes], nbytes: int) -> int:
    """Fuzz4All Target.validate_prompt: count unique valid encodings.

    Fuzz4All scores a candidate prompt by generating a batch and counting
    unique SAFE+filtered outputs. Helix's analog is unique padded stdin
    encodings of `nbytes` — empty/invalid blobs do not score.
    """
    n = nbytes if nbytes > 0 else 1
    uniq: set[bytes] = set()
    for s in seeds:
        if not s:
            continue
        uniq.add(s[:n].ljust(n, b"\x00"))
    return len(uniq)


def pick_best_prompt(
    candidates: list[tuple[str, list[bytes]]], nbytes: int
) -> tuple[str, list[bytes], int]:
    """Keep the candidate with the highest validate_prompt score (ties: first)."""
    best_p, best_s, best_sc = "", [], -1
    for prompt, seeds in candidates:
        sc = score_prompt_seeds(seeds, nbytes)
        if sc > best_sc:
            best_p, best_s, best_sc = prompt, list(seeds), sc
    if best_sc < 0:
        return "", [], 0
    return best_p, best_s, best_sc


def fuzz4all_update_strategy(new_hex: str, prev_hex: str | None, strategy: int) -> str:
    """Fuzz4All Target.update_strategy: generate / mutate / semantic / combine."""
    if strategy == 0:
        return f"seed={new_hex}\ngenerate a new encoding"
    if strategy == 1:
        return f"seed={new_hex}\nmutate the previous generation"
    if strategy == 2:
        return f"seed={new_hex}\nsemantically equivalent encoding"
    if prev_hex:
        return f"prev={prev_hex}\nseed={new_hex}\ncombine the two previous encodings"
    return f"seed={new_hex}\nmutate the previous generation"


_CONTRACT_COMMENT = re.compile(r"\b(requires|ensures|invariant|decreases|diff)\s*:", re.I)


def documentation_from_comments(source: str) -> str:
    """Pull documentation comments (Fuzz4All path_documentation ingredient)."""
    parts: list[str] = []
    text = source or ""
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            if text.startswith("/*@", i):
                end = text.find("*/", i + 2)
                i = n if end < 0 else end + 2
                continue
            end = text.find("*/", i + 2)
            if end < 0:
                break
            inner = text[i + 2 : end]
            i = end + 2
            blob = " ".join(ln.strip().lstrip("*").strip() for ln in inner.splitlines())
            blob = " ".join(blob.split())
            if blob and not _CONTRACT_COMMENT.search(blob):
                parts.append(blob)
            continue
        if text.startswith("//", i):
            eol = text.find("\n", i)
            line = text[i + 2 : (n if eol < 0 else eol)].strip()
            i = n if eol < 0 else eol + 1
            if line and not _CONTRACT_COMMENT.search(line):
                parts.append(line)
            continue
        i += 1
    return "\n".join(parts[:8])


def create_prompt_from_source(*, name: str, body: str, source: str) -> dict[str, str]:
    """Fuzz4All Target._create_prompt_from_config ingredients from a TU.

    documentation + example + handwritten prompt. Status of any distilled
    prompt is HYPOTHESIS/READS — never CLEAN, never a COVERED class.
    """
    docstring = documentation_from_comments(source)
    hw = docstring.split("\n", 1)[0] if docstring else ""
    return {
        "docstring": docstring,
        "example_code": (body or "")[:800],
        "separator": "// generate input",
        "begin": name or "",
        "hw_prompt": hw,
        "target_api": name or "",
    }
