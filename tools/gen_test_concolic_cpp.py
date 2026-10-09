#!/usr/bin/env python3
"""Regenerate tests/cpp/test_concolic.cpp (table rows for prism::run_concolic).

Case names were mined from tests/test_concolic.py before phase 5 deleted it.
When the file is gone, this script reads the last committed copy from git
(see CONCOLIC_PY_REV).
"""
from __future__ import annotations

import re
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = ROOT / "tests" / "test_concolic.py"
OUT = ROOT / "tests" / "cpp" / "test_concolic.cpp"
# Commit immediately before tests/test_concolic.py was removed from the tree.
CONCOLIC_PY_REV = "190103945^"


def concolic_py_source() -> str:
    if PY.is_file():
        return PY.read_text(encoding="utf-8")
    for rev in (CONCOLIC_PY_REV, "HEAD:tests/test_concolic.py"):
        spec = rev if ":" in rev else f"{rev}:tests/test_concolic.py"
        r = subprocess.run(
            ["git", "show", spec],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        if r.returncode == 0 and r.stdout:
            return r.stdout
    print(
        "gen_test_concolic_cpp: cannot find tests/test_concolic.py "
        f"(tried working tree and git show {CONCOLIC_PY_REV}:...)",
        file=sys.stderr,
    )
    sys.exit(2)

# --- extract (func, kind) from Python tests ---------------------------------

CRASH_EXACT = {
    ("add_overflow", "INT-SIGNED-OVF"),
    ("div_param", "INT-DIV-ZERO"),
    ("oob_write", "MEM-OOB-WRITE"),
    ("shift_ub", "INT-SHIFT-UB"),
    ("copy_bad", "MEM-OOB-WRITE"),
}

CRASH_CLS_ANY = {
    "idx_u_bad": ("MEM-OOB-READ", "MEM-OOB-WRITE"),
}

EXACT_STATUS: dict[str, str] = {
    "null_branch": "NEEDS-HARNESS",  # allow set in py; first check NEEDS_HARNESS in set
    "unchecked_alloc": "NEEDS-HARNESS",
    "arr_esc_bad": "NEEDS-HARNESS",
    "vla_bad": "NEEDS-HARNESS",
    "fdiv_bad": "NEEDS-HARNESS",
    "rec_id": "NEEDS-HARNESS",
    "with_goto": "ERROR",
    "trunc_ok": "CLEAN",
    "alloca_bad": "NEEDS-HARNESS",
    "alloca_ok": "NEEDS-HARNESS",
    "jmp_bad": "NEEDS-HARNESS",
    "jmp_ok": "CLEAN",
    "arr_esc_plus0": "NEEDS-HARNESS",
    "arr_esc_plus_rhs": "NEEDS-HARNESS",
    "atom_qual_bad": "NEEDS-HARNESS",
    "lock_init_bad": "NEEDS-HARNESS",
    "asm_nop": "NEEDS-HARNESS",
    "generic_sel": "NEEDS-HARNESS",
    "const_local_bad": "NEEDS-HARNESS",
    "struct_local_bad": "NEEDS-HARNESS",
    "typedef_local_bad": "NEEDS-HARNESS",
    "arr_esc_paren": "NEEDS-HARNESS",
    "esc_paren_addr": "NEEDS-HARNESS",
    "register_local_bad": "NEEDS-HARNESS",
    "auto_type_bad": "NEEDS-HARNESS",
    "auto_type_gnu_bad": "NEEDS-HARNESS",
    "static_local_bad": "NEEDS-HARNESS",
    "extern_local_bad": "NEEDS-HARNESS",
    "anon_enum_bad": "NEEDS-HARNESS",
    "enum_const_ok": "CLEAN",
    "alignas_bad": "NEEDS-HARNESS",
    "compound_bad": "NEEDS-HARNESS",
    "const_for_bad": "NEEDS-HARNESS",
    "anon_struct_bad": "NEEDS-HARNESS",
    "missing_nul_bad": "NEEDS-HARNESS",
    "mkstemp_ok": "NEEDS-HARNESS",
    "mkstemp_bad": "NEEDS-HARNESS",
    "percent_n_bad": "NEEDS-HARNESS",
    "saturate": "CLEAN",
    "abs_ok": "CLEAN",
    "add_u": "CLEAN",
    "idx_u_ok": "CLEAN",
    "klee_fork_neg": "CRASH",
    "klee_fork_unsat": "CLEAN",
}

FORBID_PROOF = {
    "taut_bound_bad",
    "empty_inf_ok",
    "spaceship_ok",
    "const_param_ok",
    "vol_ok",
    "tls_local_ok",
    "complex_ok",
    "sizeof_ok",
    "nested_fn_ok",
    "desig_init_ok",
    "static_assert_ok",
    "alignof_ok",
    "va_arg_ok",
    "range_for_ok",
    "lambda_ok",
    "static_cast_ok",
    "packed_ok",
    "coro_ok",
    "wide_ok",
    "bitfield_ok",
}

NOT_ERROR = set()  # filled from all names that assertNotEqual ERROR

text = concolic_py_source()
all_names: list[str] = []
for m in re.finditer(r"""fn\(['"]([\w]+)['"]\)""", text):
    all_names.append(m.group(1))
for m in re.finditer(r"for name in \(([^)]+)\):", text):
    all_names.extend(re.findall(r'"([\w]+)"', m.group(1)))
# cpp-specific plants
all_names.extend(["throws_not_dtor", "try_ok"])

# Standard unenc: NEEDS-HARNESS, not ERROR/CLEAN/CRASH/proof
unenc_names = sorted({n for n in all_names if n.endswith("_unenc_bad") or n.endswith("_unenc_bad")})
# also single-file unenc without _bad suffix pattern from py
for n in all_names:
    if "_unenc_" in n or n.endswith("_unenc_bad"):
        pass
unenc_names = sorted({n for n in all_names if "unenc" in n and n not in EXACT_STATUS and n not in CRASH_EXACT})

# Key semantic names (non-unenc bulk): everything referenced before dlopen test
cut = text.index("def test_dlopen_unenc")
head = text[:cut]
head_names: list[str] = []
for m in re.finditer(r"""fn\(['"]([\w]+)['"]\)""", head):
    head_names.append(m.group(1))
for m in re.finditer(r"for name in \(([^)]+)\):", head):
    head_names.extend(re.findall(r'"([\w]+)"', m.group(1)))
head_names.extend(["throws_not_dtor", "try_ok"])

# Tail clean/unsigned (after unenc in py)
tail_names = ["saturate", "abs_ok", "add_u", "idx_u_ok", "idx_u_bad", "add_ll"]

key_names = []
seen = set()
for n in head_names + tail_names:
    if n not in seen and "unenc" not in n and not n.startswith("klee_fork"):
        seen.add(n)
        key_names.append(n)

# planted via run_concolic
planted = ["add_overflow", "div_param", "oob_write", "shift_ub"]


def names_needing_harness(text: str) -> set[str]:
    out: set[str] = set()
    for m in re.finditer(r"^\s*def (test_\w+)\(", text, re.MULTILINE):
        start = m.start()
        nxt = text.find("\n    def ", start + 1)
        if nxt < 0:
            nxt = text.find("\nclass ", start + 1)
        block = text[start : nxt if nxt > 0 else len(text)]
        if "NEEDS_HARNESS" not in block:
            continue
        if "assertEqual(r.status, laws.NEEDS_HARNESS" not in block and "assertIn(rs[0].status" not in block:
            continue
        out.update(re.findall(r"""fn\(['"]([\w]+)['"]\)""", block))
        for fm in re.finditer(r"for name in \(([^)]+)\):", block):
            out.update(re.findall(r'"([\w]+)"', fm.group(1)))
    return out


NEEDS_HARNESS_NAMES = names_needing_harness(text)

_FORBID_CONST: dict[frozenset[str], str] = {
    frozenset({"ERROR", "CLEAN", "CRASH", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"}): "kForbidNeedHarness",
    frozenset({"NEEDS-HARNESS", "ERROR", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"}): "kForbidFlexible",
    frozenset({"PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "ERROR"}): "kForbidCrashPlant",
    frozenset({"ERROR", "PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING"}): "kForbidClean",
    frozenset({"NEEDS-HARNESS", "PROVED", "PROVED-UNBOUNDED", "BOUNDED", "CLEAN"}): "kForbidGoto",
    frozenset({"CRASH"}): "kForbidOnlyCrash",
}


def _forbid_cpp(forbid: list[str]) -> str:
    if not forbid:
        return "{}"
    key = frozenset(forbid)
    if key not in _FORBID_CONST:
        raise ValueError(f"no kForbid* constant for forbid set {forbid!r}")
    return _FORBID_CONST[key]


def cpp_row(name: str, *, budget: int = 8, planted_bug: bool = False) -> str:
    cls = ""
    exact = EXACT_STATUS.get(name)
    if exact is None and name in NEEDS_HARNESS_NAMES:
        exact = "NEEDS-HARNESS"
    if exact is None and "unenc" in name:
        exact = "NEEDS-HARNESS"
    if (name, "") in [(a, b) for a, b in CRASH_EXACT]:
        pass
    for fn, c in CRASH_EXACT:
        if fn == name:
            exact = "CRASH"
            cls = c
            break
    if name in CRASH_CLS_ANY:
        exact = "CRASH"
    if planted_bug:
        budget = 32
    forbid: list[str] = []
    if exact == "NEEDS-HARNESS":
        forbid = ["ERROR", "CLEAN", "CRASH", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"]
    elif exact == "CLEAN":
        forbid = ["ERROR", "PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING"]
    elif exact == "ERROR":
        forbid = ["NEEDS-HARNESS", "PROVED", "PROVED-UNBOUNDED", "BOUNDED", "CLEAN"]
    elif exact == "CRASH" and name not in CRASH_CLS_ANY:
        forbid = ["PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "ERROR"]
    elif name in FORBID_PROOF or name in (
        "taut_bound_bad",
        "empty_inf_ok",
        "const_param_ok",
        "vol_ok",
        "tls_local_ok",
        "complex_ok",
        "sizeof_ok",
        "nested_fn_ok",
        "desig_init_ok",
        "static_assert_ok",
        "alignof_ok",
        "va_arg_ok",
        "range_for_ok",
        "lambda_ok",
        "static_cast_ok",
        "packed_ok",
        "coro_ok",
        "wide_ok",
        "bitfield_ok",
        "spaceship_ok",
    ):
        forbid = ["NEEDS-HARNESS", "ERROR", "PROVED", "PROVED-UNBOUNDED", "BOUNDED"]
    elif name == "add_ll":
        forbid = ["CRASH"]
    elif name in ("throws_not_dtor", "try_ok"):
        forbid = []
        exact = None
    if name == "idx_u_bad":
        exact = "CRASH"
        forbid = []
    fb = _forbid_cpp(forbid)
    if cls:
        ac = "{}"
        ec = f'"{cls}"'
    elif name in CRASH_CLS_ANY:
        ec = "{}"
        ac = "kAllowOobCls"
    else:
        ec = "{}"
        ac = "{}"
    exact_s = f'"{exact}"' if exact else "{}"
    # Python only asserts counterexample on the four run_concolic planted bugs.
    counter = "true" if planted_bug else "false"
    not_proof = "true" if exact == "CLEAN" else "false"
    return f'{{"{name}", {budget}, {exact_s}, {fb}, {ec}, {ac}, {counter}, {not_proof}}}'


header = textwrap.dedent(
    """\
    // Table-driven doctests for prism::run_concolic (src/prism/stages/concolic.cpp).
    // Ports tests/test_concolic.py planted bugs, semantic harness gates, and unenc corpus.
    // Generated by tools/gen_test_concolic_cpp.py — regenerate after Python test changes.

    #include <doctest/doctest.h>
    #ifdef ERROR
    #  undef ERROR
    #endif

    #include "prism/cparse.hpp"
    #include "prism/laws.hpp"
    #include "prism/models.hpp"
    #include "prism/stages.hpp"

    #include <algorithm>
    #include <filesystem>
    #include <map>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    namespace {

    namespace fs = std::filesystem;
    namespace laws = prism::laws;

    fs::path td_root() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

    std::pair<prism::FunctionInfo, fs::path> fn_named(const std::string& name) {
        static std::map<std::string, std::pair<prism::FunctionInfo, fs::path>> cache = [] {
            std::map<std::string, std::pair<prism::FunctionInfo, fs::path>> m;
            for (auto& e : fs::directory_iterator(td_root())) {
                auto ext = e.path().extension();
                if (ext != ".c" && ext != ".cpp") continue;
                for (auto& f : prism::extract_functions(e.path(), e.path().string()))
                    m.emplace(f.name, std::make_pair(f, e.path()));
            }
            return m;
        }();
        auto it = cache.find(name);
        REQUIRE_MESSAGE(it != cache.end(), name);
        return it->second;
    }

    prism::FunctionInfo load_fn(std::string_view name) { return fn_named(std::string(name)).first; }

    prism::Finding concolic_one(const prism::FunctionInfo& fn, int budget) {
        auto recs = prism::run_concolic({fn}, budget);
        REQUIRE(recs.size() == 1);
        return recs[0];
    }

    using Names = std::vector<std::string_view>;

    bool one_of(std::string_view s, const Names& xs) {
        return std::find(xs.begin(), xs.end(), s) != xs.end();
    }

    const Names kForbidNeedHarness = {laws::ERROR, laws::CLEAN, laws::CRASH, laws::PROVED, laws::PROVED_UNBOUNDED,
                                      laws::BOUNDED};
    const Names kForbidFlexible = {laws::NEEDS_HARNESS, laws::ERROR, laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED};
    const Names kForbidCrashPlant = {laws::PROVED, laws::PROVED_UNBOUNDED, laws::PROVED_ASSUMING, laws::ERROR};
    const Names kForbidClean = {laws::ERROR, laws::PROVED, laws::PROVED_UNBOUNDED, laws::PROVED_ASSUMING};
    const Names kForbidGoto = {laws::NEEDS_HARNESS, laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED, laws::CLEAN};
    const Names kForbidOnlyCrash = {laws::CRASH};
    const Names kAllowOobCls = {"MEM-OOB-READ", "MEM-OOB-WRITE"};

    struct ConcolicRow {
        std::string_view func;
        int budget;
        std::string_view exact_status;
        Names forbid_status;
        std::string_view exact_cls;
        Names allow_cls;
        bool counterexample;
        bool message_not_proof;
    };

    void check_row(const ConcolicRow& row) {
        INFO("concolic " << row.func << " budget=" << row.budget);
        auto fn = load_fn(row.func);
        auto r = concolic_one(fn, row.budget);
        CHECK(r.stage == "concolic");
        CHECK(r.function == row.func);
        if (!row.exact_status.empty())
            CHECK_MESSAGE(r.status == row.exact_status, r.status << " " << r.message);
        for (auto bad : row.forbid_status) CHECK_MESSAGE(r.status != bad, r.status << " " << r.message);
        if (!row.exact_cls.empty()) CHECK(r.cls == row.exact_cls);
        if (!row.allow_cls.empty()) CHECK_MESSAGE(one_of(r.cls, row.allow_cls), r.cls << " " << r.message);
        if (row.counterexample) CHECK_FALSE(r.counterexample.empty());
        if (row.message_not_proof) CHECK(r.message.find("not a proof") != std::string::npos);
        if (row.exact_status == laws::CRASH) {
            CHECK(r.strength == laws::STRENGTH_FINDS);
            CHECK_FALSE(laws::is_proof(r.status));
        }
        if (row.exact_status == laws::CLEAN) CHECK_FALSE(laws::is_proof(r.status));
    }

    """
)

planted_rows = ",\n        ".join(cpp_row(n, planted_bug=True) for n in planted)
key_rows = ",\n        ".join(cpp_row(n) for n in key_names if n not in planted)
unenc_rows = ",\n        ".join(
    cpp_row(n) for n in unenc_names if n not in key_names and n not in planted
)

body = header
body += f"""
const std::vector<ConcolicRow>& planted_rows() {{
    static const std::vector<ConcolicRow> rows = {{
        {planted_rows},
    }};
    return rows;
}}

const std::vector<ConcolicRow>& key_rows() {{
    static const std::vector<ConcolicRow> rows = {{
        {key_rows},
    }};
    return rows;
}}

const std::vector<ConcolicRow>& unenc_rows() {{
    static const std::vector<ConcolicRow> rows = {{
        {unenc_rows},
    }};
    return rows;
}}

}}  // namespace

TEST_CASE("concolic: planted bugs crash with counterexample") {{
    for (const auto& row : planted_rows()) check_row(row);
}}

TEST_CASE("concolic: semantic and harness gates (testdata)") {{
    const auto& rows = key_rows();
    REQUIRE(rows.size() >= 40);
    for (const auto& row : rows) check_row(row);
}}

TEST_CASE("concolic: unencoded libc/C++ plants are NEEDS-HARNESS") {{
    const auto& rows = unenc_rows();
    REQUIRE(rows.size() >= 100);
    for (const auto& row : rows) check_row(row);
}}

#ifdef PRISM_HAS_Z3
TEST_CASE("concolic: KLEE fork negated branch finds div-zero") {{
    auto fn = load_fn("klee_fork_neg");
    auto r = concolic_one(fn, 32);
    CHECK(r.status == std::string(laws::CRASH));
    CHECK(r.cls == "INT-DIV-ZERO");
    CHECK(r.extra.at("oracle") == "z3");
    CHECK(std::stoi(r.extra.at("z3_seeds")) > 0);
}}

TEST_CASE("concolic: unsat branch skipped when Z3 available") {{
    auto fn = load_fn("klee_fork_unsat");
    auto r = concolic_one(fn, 32);
    CHECK(r.status == std::string(laws::CLEAN));
    CHECK(std::stoi(r.extra.at("skipped_unsat")) > 0);
}}
#endif

TEST_CASE("concolic: run_concolic returns one row per function") {{
    auto fn = load_fn("abs_ok");
    auto rs = prism::run_concolic({{fn}}, 8);
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].function == "abs_ok");
    CHECK(rs[0].stage == "concolic");
    CHECK(prism::run_concolic({{}}, 8).empty());
}}
"""

OUT.write_text(body, encoding="utf-8")
print(f"wrote {OUT}")
print(f"planted={len(planted)} key={len(key_names)-len(planted)} unenc={len(unenc_rows.split(chr(10)))}")
