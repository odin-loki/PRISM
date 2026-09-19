from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import os
import shutil


def _default_pbsd() -> Path:
    env = os.environ.get("HELIX_PBSD")
    if env:
        return Path(env)
    sibling = Path(r"C:\Users\odinl\OneDrive\Desktop\ParanoidBSD")
    if sibling.exists():
        return sibling
    here = Path(__file__).resolve().parents[1]
    cand = here.parent / "ParanoidBSD"
    return cand


def _default_gguf() -> Path:
    env = os.environ.get("HELIX_GGUF")
    if env:
        return Path(env)
    blob = Path.home() / ".ollama" / "models" / "blobs" / (
        "sha256-dec52a44569a2a25341c4e4d3fee25846eed4f6f0b936278e3a3c900bb99d37c"
    )
    return blob


@dataclass
class Config:
    root: Path = Path(".")
    out: Path = Path("helix-out")
    pbsd_root: Path = field(default_factory=_default_pbsd)
    model: str = os.environ.get("HELIX_MODEL", "qwen3.5:9b")
    ollama_host: str = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")
    gguf: Path = field(default_factory=_default_gguf)
    llama_server: str = os.environ.get("HELIX_LLAMA_SERVER", "http://127.0.0.1:8080")
    jobs: int = max(1, (os.cpu_count() or 4) // 2)
    timeout: float = 30.0
    unwind: int = 8
    fuzz_budget: float = 8.0
    fuzz_iters: int = 2048
    llm: bool = True
    repair_rounds: int = 3
    stages: list[str] | None = None
    skip: list[str] = field(default_factory=list)
    resume: bool = False

    def want(self, name: str) -> bool:
        if name in self.skip:
            return False
        if self.stages is None:
            return True
        return name in self.stages

    def which(self, *names: str) -> str | None:
        for n in names:
            p = shutil.which(n)
            if p:
                return p
        return None
