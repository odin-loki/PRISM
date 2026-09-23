"""Regenerate src/prism/ai/grammars.inc from grammars/*.gbnf (verbatim raw strings)."""
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
out = ["// Generated from grammars/*.gbnf. Keep byte-identical (tests/test_ai.py).\n"]
for name in ("invariants", "harness", "contract", "explain", "lean_proof", "assumption_audit"):
    text = (root / "grammars" / f"{name}.gbnf").read_text(encoding="utf-8")
    assert ")GBNF\"" not in text
    out.append(f'constexpr const char* GBNF_{name.upper()} = R"GBNF({text})GBNF";\n')
(root / "src/prism/ai/grammars.inc").write_text("".join(out), encoding="utf-8")

# Roadmap 9.4 assistant grammars (src/prism/ai/ask.cpp, draft.cpp): a separate
# include so the core list above stays as it is.
out = ["// Generated from grammars/{ask,draft}.gbnf. Keep byte-identical (tests/test_ai_assist.py).\n"]
for name in ("ask", "draft"):
    text = (root / "grammars" / f"{name}.gbnf").read_text(encoding="utf-8")
    assert ")GBNF\"" not in text
    out.append(f'constexpr const char* GBNF_{name.upper()} = R"GBNF({text})GBNF";\n')
(root / "src/prism/ai/grammars_assist.inc").write_text("".join(out), encoding="utf-8")
