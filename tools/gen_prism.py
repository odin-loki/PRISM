"""One-shot generators: taxonomy + BMC unencoded gates from the Python engine Python."""
from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from prism.taxonomy import CLASSES, _BMC_UB  # noqa: E402


def cpp_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def gen_taxonomy() -> str:
    lines = [
        '#include "prism/taxonomy.hpp"',
        '#include "prism/laws.hpp"',
        "",
        "#include <map>",
        "#include <set>",
        "#include <string>",
        "",
        "namespace prism {",
        "",
        "const std::vector<TaxonomyClass>& taxonomy_classes() {",
        "    static const std::vector<TaxonomyClass> C = {",
    ]
    for c in CLASSES:
        cwe = ", ".join(str(x) for x in (c.get("cwe") or []))
        seen = ", ".join(
            '{"' + cpp_escape(k) + '", "' + cpp_escape(v) + '"}'
            for k, v in (c.get("seen") or {}).items()
        )
        lines.append(
            f'        {{"{cpp_escape(c["id"])}", "{cpp_escape(c["name"])}", {{{cwe}}}, {{{seen}}}}},'
        )
    lines += [
        "    };",
        "    return C;",
        "}",
        "",
        "std::vector<TaxonomyRow> coverage_from_report(const RunReport& report) {",
        '    const auto PROVES = std::string(laws::STRENGTH_PROVES);',
        '    const auto FINDS = std::string(laws::STRENGTH_FINDS);',
        '    const auto SOME = std::string(laws::STRENGTH_SOME);',
        '    const auto READS = std::string(laws::STRENGTH_READS);',
        "    std::map<std::string, int> rank{{PROVES, 3}, {FINDS, 2}, {SOME, 1}, {READS, 1}};",
        "    std::map<std::string, std::string> hits;",
        "    std::set<std::string> ran_ok;",
        "    bool bmc_closed = false;",
        '    for (auto& s : report.stages) if (s.status == "ok") ran_ok.insert(s.name);',
        "    auto note = [&](const std::string& cls, const std::string& strength) {",
        "        if (cls.empty() || !rank.contains(strength)) return;",
        "        auto it = hits.find(cls);",
        "        if (it == hits.end() || rank[strength] > rank[it->second]) hits[cls] = strength;",
        "    };",
        "    for (auto& s : report.stages) {",
        "        for (auto& f : s.findings) {",
        "            if (s.name == \"llm\" || f.status == laws::HYPOTHESIS || f.status == laws::READS) {",
        "                note(f.cls.empty() ? \"INTENT\" : f.cls, READS); continue;",
        "            }",
        "            if (f.status == laws::ERROR && !f.cls.empty() && f.strength == FINDS) { note(f.cls, FINDS); continue; }",
        "            if (f.status == laws::NOTRUN || f.status == laws::CLEAN || f.status == laws::ERROR ||",
        "                f.status == laws::NEEDS_HARNESS || f.status == laws::TIMEOUT ||",
        "                f.status == laws::UNKNOWN || f.status == laws::NOSEED) continue;",
        "            if (f.status == laws::PROVED || f.status == laws::PROVED_UNBOUNDED || f.status == laws::PROVED_ASSUMING) {",
        '                if (s.name == "bmc") bmc_closed = true;',
        "                std::string cls = f.cls;",
        "                if (cls.empty() && (s.name == \"wp\" || s.name == \"contracts\")) cls = \"FUNC-CONTRACT\";",
        "                note(cls, f.strength.empty() ? PROVES : f.strength);",
        "                continue;",
        "            }",
        "            if (f.status == laws::FAILED || f.status == laws::CRASH || f.status == laws::BOUNDED) {",
        "                auto st = rank.contains(f.strength) ? f.strength : FINDS;",
        "                if (f.status == laws::BOUNDED) st = SOME;",
        "                note(f.cls, st);",
        "            }",
        "        }",
        "    }",
        "    if (bmc_closed) {",
        "        for (auto* id : {" + ", ".join(f'"{x}"' for x in sorted(_BMC_UB)) + "})",
        "            if (!hits.contains(id)) hits[id] = PROVES;",
        "    }",
        "    std::vector<TaxonomyRow> rows;",
        "    for (auto& c : taxonomy_classes()) {",
        "        TaxonomyRow r; r.id = c.id; r.name = c.name; r.cwe = c.cwe; r.seen = c.seen;",
        '        r.best = hits.contains(c.id) ? hits[c.id] : "";',
        '        if (r.best == PROVES || r.best == FINDS) r.verdict = "COVERED";',
        '        else if (r.best == SOME || r.best == READS) r.verdict = "PARTIAL";',
        "        else {",
        "            bool capable = false;",
        "            for (auto& [inst, _] : c.seen) if (ran_ok.contains(inst)) capable = true;",
        '            r.verdict = capable ? "PARTIAL" : "GAP";',
        "            if (capable && r.best.empty()) r.best = SOME;",
        "        }",
        "        rows.push_back(std::move(r));",
        "    }",
        "    return rows;",
        "}",
        "",
        "}  // namespace prism",
        "",
    ]
    return "\n".join(lines)


DESIGNATED_INIT_RE = r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*="
_BANNED_UNENC = ("strcpy", "strcat", "sprintf", "vsprintf", "gets")


def _const_join(node: ast.AST, env: dict[str, str]) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        parts: list[str] = []
        for v in node.values:
            if isinstance(v, ast.Constant) and isinstance(v.value, str):
                parts.append(v.value)
            elif isinstance(v, ast.FormattedValue) and isinstance(v.value, ast.Name) and v.value.id == "engine":
                parts.append("{engine}")
            else:
                return None
        return "".join(parts)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        a, b = _const_join(node.left, env), _const_join(node.right, env)
        if a is not None and b is not None:
            return a + b
    if isinstance(node, ast.Name) and node.id in env:
        return env[node.id]
    return None


def _is_re_call(node: ast.AST, name: str) -> bool:
    return (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == name
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == "re"
    )


def _pats_from_test(test: ast.AST, env: dict[str, str]) -> list[str]:
    pats: list[str] = []

    def walk(n: ast.AST) -> None:
        if isinstance(n, ast.BoolOp) and isinstance(n.op, ast.Or):
            for v in n.values:
                walk(v)
            return
        if _is_re_call(n, "search") and n.args:
            p = _const_join(n.args[0], env)
            if p is not None:
                pats.append(p)

    walk(test)
    return pats


def _first_return_msg(body: list[ast.stmt], env: dict[str, str]) -> str | None:
    for stmt in body:
        if isinstance(stmt, ast.Return) and stmt.value is not None:
            return _const_join(stmt.value, env)
    return None


def _cpp_re_lit(raw: str) -> str:
    esc = cpp_escape(raw)
    if len(esc) < 1400:
        return f'"{esc}"'
    chunks: list[str] = []
    i = 0
    while i < len(esc):
        j = min(i + 1400, len(esc))
        while j > i and esc[j - 1] == "\\":
            j -= 1
        if j == i:
            j = min(i + 1400, len(esc))
        chunks.append(f'"{esc[i:j]}"')
        i = j
    return " ".join(chunks)


def gen_unenc() -> tuple[str, int]:
    """Lift sequential re.search gates from unencoded_syntax_reason."""
    src = (ROOT / "prism" / "bmc.py").read_text(encoding="utf-8")
    tree = ast.parse(src)
    env: dict[str, str] = {}
    start_words: list[str] | None = None
    fn_node: ast.FunctionDef | None = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            if node.targets[0].id == "_DECL_TYPE":
                val = _const_join(node.value, env)
                if val is not None:
                    env["_DECL_TYPE"] = val
            if node.targets[0].id == "_STMT_START_WORDS":
                if isinstance(node.value, ast.Set):
                    words = []
                    for elt in node.value.elts:
                        if isinstance(elt, ast.Constant) and isinstance(elt.value, str):
                            words.append(elt.value)
                    start_words = words
        if isinstance(node, ast.FunctionDef) and node.name == "unencoded_syntax_reason":
            fn_node = node
    if fn_node is None:
        raise SystemExit("unencoded_syntax_reason not found")
    if "_DECL_TYPE" not in env:
        raise SystemExit("_DECL_TYPE not found")
    if not start_words:
        raise SystemExit("_STMT_START_WORDS not found")

    gates: list[tuple[str, str]] = []
    typedef_pat: str | None = None
    typedef_msg: str | None = None
    for stmt in fn_node.body:
        if isinstance(stmt, ast.If):
            pats = _pats_from_test(stmt.test, env)
            msg = _first_return_msg(stmt.body, env)
            if pats and msg:
                if len(pats) == 1:
                    raw = pats[0]
                else:
                    raw = "|".join(f"(?:{p})" for p in pats)
                gates.append((raw, msg))
        elif isinstance(stmt, ast.For):
            if _is_re_call(stmt.iter, "finditer") and stmt.iter.args:
                typedef_pat = _const_join(stmt.iter.args[0], env)
                typedef_msg = _first_return_msg(stmt.body, env)
                if typedef_msg is None:
                    for inner in stmt.body:
                        if isinstance(inner, ast.If):
                            typedef_msg = _first_return_msg(inner.body, env)
                            if typedef_msg:
                                break

    for raw, msg in gates:
        for banned in _BANNED_UNENC:
            needle = rf"\b{banned}\s*\("
            if needle in raw:
                raise SystemExit(f"{banned} leaked into unencoded_syntax_reason")

    n_search = len(gates) + (1 if typedef_pat else 0)
    out = [
        "static std::optional<std::string> unencoded_syntax_reason(const FunctionInfo& fn, std::string_view engine) {",
        "    const std::string& body = fn.body;",
        "    if (body.empty()) return std::nullopt;",
        "    std::string blob = fn.signature + \"\\n\" + body;",
        f"    // generated {n_search} gates from prism/bmc.py",
        f'    if (prism::re_search("{cpp_escape(DESIGNATED_INIT_RE)}", body)) {{',
        '        return std::format("designated init unencoded: {} is not a designated-init model", engine);',
        "    }",
    ]
    for raw, msg in gates:
        if "designated" in msg.lower():
            continue
        lit = _cpp_re_lit(raw)
        fmt = msg.replace("{engine}", "{}")
        hay = "blob" if ("requires" in raw and "concept" in raw) else "body"
        out.append(f"    if (prism::re_search({lit}, {hay})) {{")
        out.append(f'        return std::format("{cpp_escape(fmt)}", engine);')
        out.append("    }")

    if typedef_pat and typedef_msg:
        words = ", ".join(f'"{cpp_escape(w)}"' for w in start_words)
        fmt = typedef_msg.replace("{engine}", "{}")
        out += [
            "    {",
            "        static const std::unordered_set<std::string> start{",
            f"            {words}}};",
            f'        static prism::Regex td({_cpp_re_lit(typedef_pat)}, true);',
            "        for (auto& m : td.finditer(body)) {",
            "            if (!start.contains(m.group(1)))",
            f'                return std::format("{cpp_escape(fmt)}", engine);',
            "        }",
            "    }",
        ]
    out += ["    return std::nullopt;", "}", ""]
    return "\n".join(out), n_search


def main() -> None:
    unenc, n = gen_unenc()
    (ROOT / "src" / "prism" / "bmc_unenc.inc").write_text(unenc, encoding="utf-8")
    print("unenc gates", n, "bytes", len(unenc))
    if "--taxonomy" in sys.argv:
        tax = gen_taxonomy()
        (ROOT / "src" / "prism" / "taxonomy.cpp").write_text(tax, encoding="utf-8")
        print("taxonomy classes", len(CLASSES), "bytes", len(tax))


if __name__ == "__main__":
    main()
