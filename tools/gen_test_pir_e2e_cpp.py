#!/usr/bin/env python3
"""Regenerate tests/cpp/test_pir_e2e.cpp from tests/test_pir.py expectation tables.

When tests/test_pir.py is deleted, reads the last committed copy (PIR_PY_REV).
"""
from __future__ import annotations

import ast
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from prism import laws  # noqa: E402

PY = ROOT / "tests" / "test_pir.py"
OUT = ROOT / "tests" / "cpp" / "test_pir_e2e.cpp"
PIR_PY_REV = "HEAD:tests/test_pir.py"


def pir_py_source() -> str:
    if PY.is_file():
        return PY.read_text(encoding="utf-8")
    r = subprocess.run(
        ["git", "show", PIR_PY_REV],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if r.returncode == 0 and r.stdout:
        return r.stdout
    print("gen_test_pir_e2e_cpp: cannot find tests/test_pir.py", file=sys.stderr)
    sys.exit(2)


def extract_dict(module: ast.Module, name: str) -> dict:
    ns = {"laws": laws}
    for node in module.body:
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == name:
            return eval(compile(ast.Expression(node.value), "<pir>", "eval"), ns)
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == name:
                    return eval(compile(ast.Expression(node.value), "<pir>", "eval"), ns)
    raise KeyError(name)


def main() -> None:
    mod = ast.parse(pir_py_source())
    expected = extract_dict(mod, "EXPECTED")
    mem = extract_dict(mod, "MEM_EXPECTED")
    pir3 = extract_dict(mod, "PIR3_EXPECTED")
    classes = extract_dict(mod, "CLASSES")

    rows: list[tuple[str, str, str, str]] = []
    for (file, fn), status in expected.items():
        rows.append((file, fn, status, classes.get((file, fn), "")))
    for (file, fn), (status, cls) in mem.items():
        rows.append((file, fn, status, cls or ""))
    for (file, fn), (status, cls) in pir3.items():
        rows.append((file, fn, status, cls or ""))
    rows.sort()

    files = sorted({r[0] for r in rows})

    def esc(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')

    row_lines = "\n".join(
        f'        {{"{esc(f)}", "{esc(fn)}", "{esc(st)}", "{esc(cl)}"}},'
        for f, fn, st, cl in rows
    )
    file_lines = "\n".join(f'        "{esc(f)}",' for f in files)

    body = textwrap.dedent(
        f'''\
// Table-driven PIR stage on tests/pir (was tests/test_pir.py::TestPirStage).
// Regenerate: python tools/gen_test_pir_e2e_cpp.py
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {{

namespace fs = std::filesystem;
namespace pp = prism::pir;

struct Row {{
    const char* file;
    const char* function;
    const char* status;
    const char* cls;  // empty = do not check
}};

const std::vector<Row>& rows() {{
    static const std::vector<Row> k{{
{row_lines}
    }};
    return k;
}}

const std::vector<const char*>& pir_files() {{
    static const std::vector<const char*> k{{
{file_lines}
    }};
    return k;
}}

std::map<std::pair<std::string, std::string>, const prism::Finding*> index(
    const std::vector<prism::Finding>& out) {{
    std::map<std::pair<std::string, std::string>, const prism::Finding*> m;
    for (auto& f : out) {{
        if (!f.function) continue;
        auto key = std::make_pair(fs::path(f.file).filename().string(), *f.function);
        m[key] = &f;
    }}
    return m;
}}

}}  // namespace

TEST_CASE("pir e2e: tests/pir verdict table (skips without clang/opt)") {{
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) {{
        MESSAGE("clang/opt not on PATH: pir e2e skipped");
        return;
    }}
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    std::vector<fs::path> paths;
    for (auto* name : pir_files()) paths.push_back(dir / name);
    auto out = pp::run_pir(paths, cfg);
    auto got = index(out);
    std::vector<std::string> wrong;
    for (auto& r : rows()) {{
        auto it = got.find({{r.file, r.function}});
        if (it == got.end()) {{
            wrong.push_back(std::string(r.file) + ":" + r.function + " missing");
            continue;
        }}
        auto& f = *it->second;
        if (f.status != r.status) {{
            wrong.push_back(std::string(r.file) + ":" + r.function + " status " + f.status + " != " + r.status);
            continue;
        }}
        if (r.cls[0] && f.cls != r.cls)
            wrong.push_back(std::string(r.file) + ":" + r.function + " cls " + f.cls + " != " + r.cls);
        if (f.status == prism::laws::FAILED) {{
            if (f.counterexample.empty())
                wrong.push_back(std::string(r.file) + ":" + r.function + " FAILED without counterexample");
            else if (f.extra.count("cex") && f.extra.at("cex") != f.counterexample)
                wrong.push_back(std::string(r.file) + ":" + r.function + " cex mismatch");
        }}
    }}
    for (auto& w : wrong) MESSAGE(w);
    CHECK(wrong.empty());
}}

TEST_CASE("pir e2e: Law 9 holds translation validation without --allow-exec") {{
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
    cfg.root = dir;
    auto out = pp::run_pir({{dir / "overflow.c"}}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({{"overflow.c", "add_ok"}}));
    CHECK(got.at({{"overflow.c", "add_ok"}})->extra.at("tv") == "NOTRUN (needs --allow-exec)");
    int notrun_rows = 0;
    for (auto& f : out)
        if (f.status == prism::laws::NOTRUN) ++notrun_rows;
    CHECK(notrun_rows == 1);
    CHECK(out.back().extra.at("reason") == "executes-scanned-code");
}}

TEST_CASE("pir e2e: --fp-checks is opt-in (skips without clang/opt)") {{
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
    cfg.root = dir;
    cfg.fp_checks = true;
    auto out = pp::run_pir({{dir / "fp_arith.c"}}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({{"fp_arith.c", "fp_div"}}));
    auto& f = *got.at({{"fp_arith.c", "fp_div"}});
    CHECK(f.status == prism::laws::FAILED);
    CHECK((f.cls == "FLOAT-DIV-ZERO" || f.cls == "FLOAT-INVALID" || f.cls == "FLOAT-OVERFLOW"));
    REQUIRE(got.contains({{"fp_arith.c", "fp_div_guard"}}));
    CHECK(got.at({{"fp_arith.c", "fp_div_guard"}})->status == prism::laws::PROVED);
}}

TEST_CASE("pir e2e: --strict-aliasing is opt-in (skips without clang/opt)") {{
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::path(PRISM_SOURCE_DIR) / "tests" / "pir";
    cfg.root = dir;
    {{
        auto out = pp::run_pir({{dir / "mem_alias.c"}}, cfg);
        auto got = index(out);
        REQUIRE(got.contains({{"mem_alias.c", "pun_bad"}}));
        CHECK(got.at({{"mem_alias.c", "pun_bad"}})->status == prism::laws::PROVED);
    }}
    cfg.strict_aliasing = true;
    auto out = pp::run_pir({{dir / "mem_alias.c"}}, cfg);
    auto got = index(out);
    REQUIRE(got.contains({{"mem_alias.c", "pun_bad"}}));
    auto& f = *got.at({{"mem_alias.c", "pun_bad"}});
    CHECK(f.status == prism::laws::FAILED);
    CHECK(f.cls == "MEM-STRICT-ALIAS");
    REQUIRE(got.contains({{"mem_alias.c", "char_ok"}}));
    CHECK(got.at({{"mem_alias.c", "char_ok"}})->status == prism::laws::PROVED);
}}
'''
    )
    OUT.write_text(body, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({len(rows)} rows, {len(files)} files)")


if __name__ == "__main__":
    main()
