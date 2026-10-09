#!/usr/bin/env python3
"""Regenerate tests/cpp/test_bmc.cpp (table rows for prism::run_bmc).

Oracle rows mirror tests/test_bmc.py TestBMC / TestKInduction / TestLocalPointerHarness
before phase 5 deleted the file. When the file is gone, read the last copy from git
(BMC_PY_REV).
"""
from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
PY = ROOT / "tests" / "test_bmc.py"
OUT = ROOT / "tests" / "cpp" / "test_bmc.cpp"
BMC_PY_REV = "190103945^"

# (id, file, func, exact_status, allow_status, forbid_status, exact_cls, allow_cls, counter, extra)
# Status/class strings use laws:: names where the C++ table does; defect classes stay quoted.
BMC_ROWS: list[tuple] = [
    (
        "overflow",
        "add_overflow.c",
        "add_overflow",
        "FAILED",
        None,
        ("ERROR", "NOTRUN"),
        "INT-SIGNED-OVF",
        (),
        True,
        (("incremental_k", "1"), ("param_premise", "named")),
    ),
    ("div0", "div_param.c", "div_param", "FAILED", None, ("ERROR",), "INT-DIV-ZERO", (), False, ()),
    (
        "oob",
        "oob_write.c",
        "oob_write",
        "FAILED",
        None,
        ("ERROR",),
        None,
        ("MEM-OOB-WRITE", "MEM-OOB-READ"),
        False,
        (),
    ),
    ("abs", "abs_ok.c", "abs_ok", None, "kProof", ("ERROR", "NEEDS_HARNESS"), None, (), False, ()),
    ("fsm", "fsm.c", "fsm_step", None, "kFsmOk", ("ERROR",), None, (), False, ()),
    ("masked_switch", "masked_switch.c", "masked_switch", None, "kFsmOk", ("ERROR",), None, (), False, ()),
    ("loop_prove", "loop_prove.c", "loop_prove", None, "kProof", ("ERROR",), None, (), False, ()),
    ("loop_overflow", "loop_overflow.c", "loop_overflow", "FAILED", None, ("ERROR",), "INT-SIGNED-OVF", (), False, ()),
    ("taut_bound_ok", "taut_bound.c", "taut_bound_ok", None, "kProofOrBounded", ("ERROR",), None, (), False, ()),
    ("do_once", "subset.c", "do_once", None, "kProof", ("ERROR",), None, (), False, ()),
    ("do_overflow", "subset.c", "do_overflow", "FAILED", None, ("ERROR",), "INT-SIGNED-OVF", (), False, ()),
    ("sizeof_int", "subset.c", "sz_int", None, "kProof", ("ERROR",), None, (), False, ()),
    ("sizeof_arr", "subset.c", "sz_arr", None, "kProof", ("ERROR",), None, (), False, ()),
    ("ternary_pick", "subset.c", "pick", None, "kProof", ("ERROR",), None, (), False, ()),
    ("ternary_abs_ovf", "subset.c", "abs_ter", "FAILED", None, ("ERROR",), "INT-SIGNED-OVF", (), False, ()),
    ("trunc_ok", "trunc.c", "trunc_ok", None, "kProof", ("ERROR",), None, (), False, ()),
    ("trunc_bad", "trunc.c", "trunc_bad", None, None, ("ERROR",), None, (), False, ()),
    ("continue_skip", "continue.c", "cont_skip", None, "kProof", ("FAILED", "ERROR"), None, (), False, ()),
    ("comma_sum", "comma.c", "comma_sum", None, "kProof", ("ERROR",), None, (), False, ()),
    ("comma_ovf", "comma.c", "comma_ovf", "FAILED", None, ("ERROR",), "INT-SIGNED-OVF", (), False, ()),
    (
        "goto_unstructured",
        "goto_structured.c",
        "goto_into_block",
        "NEEDS_HARNESS",
        None,
        ("PROVED", "PROVED_UNBOUNDED", "BOUNDED"),
        None,
        (),
        False,
        (),
    ),
    ("unsigned_add_wrap", "unsigned.c", "add_u", None, "kProof", ("FAILED", "ERROR"), None, (), False, ()),
    ("unsigned_idx_ok", "unsigned.c", "idx_u_ok", None, "kProof", ("ERROR",), None, (), False, ()),
    (
        "unsigned_idx_bad",
        "unsigned.c",
        "idx_u_bad",
        "FAILED",
        None,
        ("ERROR",),
        None,
        ("MEM-OOB-READ", "MEM-OOB-WRITE"),
        False,
        (),
    ),
    ("unsigned_dead", "unsigned.c", "dead_u", None, "kProof", ("FAILED", "ERROR"), None, (), False, ()),
    (
        "vla_bad",
        "vla.c",
        "vla_bad",
        "NEEDS_HARNESS",
        None,
        ("PROVED", "PROVED_UNBOUNDED", "BOUNDED"),
        None,
        (),
        False,
        (),
    ),
    ("vla_ok", "vla.c", "vla_ok", None, "kProofOrBounded", ("NEEDS_HARNESS", "ERROR"), None, (), False, ()),
    (
        "nested_ovf",
        "nested.c",
        "nested_ovf",
        "FAILED",
        None,
        ("ERROR",),
        "INT-SIGNED-OVF",
        (),
        False,
        (("k_induction", "not-needed"),),
    ),
    ("nested_ok", "nested.c", "nested_ok", None, "kProofOrBounded", ("ERROR", "FAILED"), None, (), False, ()),
    ("long_long_ovf", "longlong.c", "add_ll", "FAILED", None, ("ERROR",), "INT-SIGNED-OVF", (), False, ()),
    ("long_long_ok", "longlong.c", "add_ll_ok", None, "kProof", ("ERROR",), None, (), False, ()),
    ("cxx_move", "use_after_move.cpp", "move_bad", "NEEDS_HARNESS", None, ("ERROR",), None, (), False, ()),
    (
        "kinduct_closed",
        "kinduct.c",
        "kinduct_closed",
        "PROVED_UNBOUNDED",
        None,
        ("ERROR", "FAILED"),
        None,
        (),
        False,
        (("k_induction", "closed"), ("k_induction_k", "1")),
    ),
    (
        "kinduct_step_open",
        "kinduct.c",
        "kinduct_step_open",
        "BOUNDED",
        None,
        ("FAILED", "ERROR"),
        None,
        (),
        False,
        (("k_induction_tried", "1"),),
    ),
    ("unchecked_alloc", "unchecked_alloc.c", "unchecked_alloc", "NEEDS_HARNESS", None, ("ERROR",), None, (), False, ()),
    ("esc_bad", "stack_escape.c", "esc_bad", "NEEDS_HARNESS", None, ("ERROR",), None, (), False, ()),
    (
        "arr_esc_bad",
        "stack_escape.c",
        "arr_esc_bad",
        "NEEDS_HARNESS",
        None,
        ("PROVED", "PROVED_UNBOUNDED", "BOUNDED", "FAILED", "ERROR"),
        None,
        (),
        False,
        (),
    ),
]

_LAWS = {
    "FAILED",
    "ERROR",
    "NOTRUN",
    "NEEDS_HARNESS",
    "PROVED",
    "PROVED_UNBOUNDED",
    "BOUNDED",
    "CLEAN",
}


def bmc_py_source() -> str:
    if PY.is_file():
        return PY.read_text(encoding="utf-8")
    for rev in (BMC_PY_REV, "HEAD:tests/test_bmc.py"):
        spec = rev if ":" in rev else f"{rev}:tests/test_bmc.py"
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
        f"gen_test_bmc_cpp: cannot find tests/test_bmc.py (tried git show {BMC_PY_REV}:...)",
        file=sys.stderr,
    )
    sys.exit(2)


def _law_ref(name: str) -> str:
    if name in _LAWS:
        return f"laws::{name}"
    raise ValueError(name)


def _status_ref(s: str | None) -> str:
    if not s:
        return "{}"
    return _law_ref(s)


def _names_ref(names: Sequence[str] | None) -> str:
    if not names:
        return "{}"
    return "{" + ", ".join(_law_ref(n) for n in names) + "}"


def _allow_ref(name: str | None) -> str:
    if not name:
        return "{}"
    return name


def _cls_list(classes: Sequence[str]) -> str:
    if not classes:
        return "{}"
    return "{" + ", ".join(f'"{c}"' for c in classes) + "}"


def _extra_list(pairs: Sequence[tuple[str, str]]) -> str:
    if not pairs:
        return "{}"
    inner = ", ".join(f'{{"{k}", "{v}"}}' for k, v in pairs)
    return "{" + inner + "}"


def cpp_row(row: tuple) -> str:
    rid, file, func, exact, allow, forbid, ecls, acls, counter, extra = row
    parts = [
        f'"{rid}"',
        f'"{file}"',
        f'"{func}"',
        _status_ref(exact),
        _allow_ref(allow),
        _names_ref(forbid),
        f'"{ecls}"' if ecls else "{}",
        _cls_list(acls),
        "true" if counter else "false",
        _extra_list(extra),
    ]
    return "        {" + ", ".join(parts) + "}"


def main() -> None:
    _ = bmc_py_source()  # fail early if git history is missing
    rows_cpp = ",\n".join(cpp_row(r) for r in BMC_ROWS)
    body = textwrap.dedent(
        f"""\
        // Table-driven doctests for prism::run_bmc (declared in prism/stages.hpp).
        // Ports the testdata oracle rows from tests/test_bmc.py TestBMC and k-induction
        // extras from TestKInduction. Generated by tools/gen_test_bmc_cpp.py.

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
        #include <string>
        #include <string_view>
        #include <utility>
        #include <vector>

        namespace {{

        namespace fs = std::filesystem;
        namespace laws = prism::laws;

        fs::path td_root() {{ return fs::path(PRISM_SOURCE_DIR) / "testdata"; }}

        using Names = std::vector<std::string_view>;

        prism::FunctionInfo load_fn(std::string_view file, std::string_view name) {{
            auto p = td_root() / file;
            for (auto& f : prism::extract_functions(p, p.string()))
                if (f.name == name) return f;
            FAIL("missing " << name << " in " << file);
            return {{}};
        }}

        prism::Finding bmc_one(const prism::FunctionInfo& fn, int unwind = 8) {{
            auto recs = prism::run_bmc({{fn}}, unwind);
            REQUIRE(recs.size() == 1);
            return recs[0];
        }}

        bool one_of(std::string_view s, const Names& xs) {{
            return std::find(xs.begin(), xs.end(), s) != xs.end();
        }}

        std::string extra_get(const prism::Finding& f, std::string_view key) {{
            auto it = f.extra.find(std::string(key));
            return it == f.extra.end() ? std::string() : it->second;
        }}

        const Names kProof = {{laws::PROVED, laws::PROVED_UNBOUNDED}};
        const Names kProofOrBounded = {{laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED}};
        const Names kFsmOk = {{laws::FAILED, laws::PROVED, laws::PROVED_UNBOUNDED, laws::BOUNDED}};

        struct BmcRow {{
            std::string_view id;
            std::string_view file;
            std::string_view func;
            std::string_view exact_status;
            Names allow_status;
            Names forbid_status;
            std::string_view exact_cls;
            Names allow_cls;
            bool counterexample = false;
            std::vector<std::pair<std::string_view, std::string_view>> extra;
        }};

        const std::vector<BmcRow>& bmc_rows() {{
            static const std::vector<BmcRow> rows = {{
                {rows_cpp},
            }};
            return rows;
        }}

        void check_row(const BmcRow& row) {{
            INFO("row " << row.id << " " << row.file << ":" << row.func);
            auto fn = load_fn(row.file, row.func);
            auto r = bmc_one(fn);
            CHECK(r.function == row.func);
            CHECK(r.stage == "bmc");
            if (!row.exact_status.empty()) {{
                CHECK_MESSAGE(r.status == row.exact_status, r.status << " " << r.message);
            }} else if (!row.allow_status.empty()) {{
                CHECK_MESSAGE(one_of(r.status, row.allow_status), r.status << " " << r.message);
            }}
            for (auto bad : row.forbid_status) CHECK_MESSAGE(r.status != bad, r.status << " " << r.message);
            if (!row.exact_cls.empty()) CHECK(r.cls == row.exact_cls);
            else if (!row.allow_cls.empty()) CHECK_MESSAGE(one_of(r.cls, row.allow_cls), r.cls << " " << r.message);
            if (row.counterexample) CHECK_FALSE(r.counterexample.empty());
            for (auto [k, v] : row.extra)
                CHECK_MESSAGE(extra_get(r, k) == v, k << "=" << extra_get(r, k) << " expected " << v);
            if (row.id == "goto_unstructured") CHECK(r.message.find("goto") != std::string::npos);
        }}

        }}  // namespace

        TEST_CASE("bmc: missing Z3 is NOTRUN, never a proof or clean") {{
        #ifndef PRISM_HAS_Z3
            prism::FunctionInfo fn;
            fn.file = "abs_ok.c";
            fn.name = "abs_ok";
            fn.kind = "SCALAR";
            fn.line = 1;
            fn.signature = "int abs_ok(int x)";
            fn.params = {{"int", "x"}};
            fn.body = "return x < 0 ? -x : x;";
            auto recs = prism::run_bmc({{fn}}, 8);
            REQUIRE(recs.size() == 1);
            CHECK(recs[0].status == std::string(laws::NOTRUN));
            CHECK(recs[0].status != std::string(laws::CLEAN));
            CHECK(recs[0].status != std::string(laws::PROVED));
            CHECK_FALSE(laws::is_proof(recs[0].status));
            CHECK(recs[0].message.find("z3") != std::string::npos);
            CHECK(extra_get(recs[0], "install").find("PRISM_Z3") != std::string::npos);
        #else
            MESSAGE("PRISM_HAS_Z3: skip NOTRUN row");
        #endif
        }}

        #ifdef PRISM_HAS_Z3
        TEST_CASE("bmc: testdata table (run_bmc)") {{
            const auto& rows = bmc_rows();
            REQUIRE(rows.size() >= 15);
            for (const auto& row : rows) check_row(row);
        }}

        TEST_CASE("run_bmc never silent-empty on a function list") {{
            auto fn = load_fn("abs_ok.c", "abs_ok");
            auto recs = prism::run_bmc({{fn}}, 8);
            REQUIRE_FALSE(recs.empty());
            CHECK(recs[0].function == "abs_ok");
            auto empty = prism::run_bmc({{}}, 8);
            CHECK(empty.empty());
        }}
        #endif
        """
    )
    OUT.write_text(body, encoding="utf-8")
    print(f"wrote {OUT} ({len(BMC_ROWS)} rows)")


if __name__ == "__main__":
    main()
