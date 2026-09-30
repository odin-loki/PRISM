// Doctests for the wp stage (ensures proved by substituting each returned
// expression; PROVED-ASSUMING at best, never PROVED) and the harness stage
// (pointer-parameter functions materialized from an honest requires; Law 6).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path td() {
    return std::filesystem::path(PRISM_SOURCE_DIR) / "testdata";
}

prism::FunctionInfo fn_in(const char* file, const char* name) {
    auto p = td() / file;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << file);
    return {};
}

// A C file written under the private temp directory, parsed back.
prism::FunctionInfo fn_from(const std::string& stem, const std::string& src, const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "prism_wp_harness";
    std::filesystem::create_directories(dir);
    auto p = dir / stem;
    std::ofstream(p, std::ios::binary) << src;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << stem);
    return {};
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

std::string extra_or(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

bool truthy(const prism::Finding& f, const char* key) {
    auto v = extra_or(f, key);
    return !v.empty() && v != "false" && v != "0";
}

int stage_index(std::string_view name) {
    for (int i = 0; prism::STAGE_ORDER[i]; ++i)
        if (name == prism::STAGE_ORDER[i]) return i;
    return -1;
}

prism::FunctionInfo scalar_inc(const char* name = "inc") {
    prism::FunctionInfo f;
    f.file = "synthetic.c";
    f.name = name;
    f.kind = "SCALAR";
    f.line = 1;
    f.signature = std::string("int ") + name + "(int x)";
    f.params = {{"int", "x"}};
    f.body = "return x + 1;";
    return f;
}

}  // namespace

// ---- wp

TEST_CASE("wp: the stage runs right after contracts") {
    auto c = stage_index("contracts");
    REQUIRE(c >= 0);
    CHECK(stage_index("wp") == c + 1);
}

TEST_CASE("wp: scalar predicates encode; ACSL memory/logic predicates do not") {
    CHECK(prism::encode_wp_predicate("x < 100") == "x < 100");
    CHECK(prism::encode_wp_predicate("result == x+1") == "result == x+1");
    CHECK(prism::encode_wp_predicate("x > -2147483647").has_value());
    for (auto* bad : {"\\valid(&x)", "result == \\old(x) + 1", "\\forall integer k; k == x",
                      "\\exists integer k; k == x", "\\at(x, Pre)", "\\separated(p, q)", "p->x == 0",
                      "p->x == p->x", "result == foo(x)"}) {
        INFO(bad);
        CHECK_FALSE(prism::encode_wp_predicate(bad).has_value());
    }
}

TEST_CASE("wp: a tautological VC is PROVED-ASSUMING by substitution, never PROVED") {
    auto recs = prism::run_wp({fn_in("contract_add.c", "inc")}, 8);
    REQUIRE_FALSE(recs.empty());
    auto& r = recs[0];
    CHECK(r.stage == "wp");
    CHECK(r.status == prism::laws::PROVED_ASSUMING);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(r.status != prism::laws::PROVED_UNBOUNDED);
    CHECK(r.status != prism::laws::CLEAN);
    CHECK(r.status != prism::laws::BOUNDED);
    CHECK(has(lower(r.message), "never"));
    CHECK(extra_or(r, "engine") == "prism-wp");
    CHECK(extra_or(r, "wp") == "return-substitution");
    CHECK(truthy(r, "wp_qed"));
}

TEST_CASE("wp: the false twin of a tautological VC is FAILED, not a proof") {
    auto recs = prism::run_wp({fn_in("contract_add.c", "not_inc")}, 8);
    REQUIRE_FALSE(recs.empty());
    auto& r = recs[0];
    CHECK(r.status == prism::laws::FAILED);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(r.status != prism::laws::PROVED_ASSUMING);
    CHECK(r.status != prism::laws::CLEAN);
    CHECK_FALSE(truthy(r, "wp_qed"));
}

TEST_CASE("wp: a whitespace-only return is kept in the obligation, not dropped") {
    // Dropping the empty return would leave only `return x + 1;`, a
    // tautological VC, and a PROVED-ASSUMING for a path that returns nothing.
    for (auto* ret : {"return  ;", "return;"}) {
        INFO(ret);
        auto f = fn_from("wp_empty_return.c",
                         std::string("int er(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                                     "    if (x > 0) ") +
                             ret + "\n    return x + 1;\n}\n",
                         "er");
        REQUIRE(f.kind == "SCALAR");
        auto recs = prism::run_wp({f}, 8);
        REQUIRE(recs.size() == 1);
        auto& r = recs[0];
        INFO(r.status << ": " << r.message);
        CHECK_FALSE(truthy(r, "wp_qed"));
        CHECK(r.status == prism::laws::ERROR);
        CHECK_FALSE(prism::laws::is_proof(r.status));
        auto rets = nlohmann::json::parse(extra_or(r, "wp_returns"));
        REQUIRE(rets.is_array());
        REQUIRE(rets.size() == 2);
        CHECK(rets[0] == "");
        CHECK(rets[1] == "x + 1");
    }
    // The true twin without the empty return is closed by substitution.
    auto twin = fn_from("wp_empty_return_twin.c",
                        "int er2(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                        "    return x + 1;\n}\n",
                        "er2");
    auto t = prism::run_wp({twin}, 8);
    REQUIRE(t.size() == 1);
    CHECK(t[0].status == prism::laws::PROVED_ASSUMING);
    CHECK(truthy(t[0], "wp_qed"));
}

TEST_CASE("wp and contracts: `return(expr);` is checked, not skipped") {
    // return(x) with ensures result == x+1 is false; return(x + 1) is true.
    auto bad = fn_from("ret_paren_bad.c",
                       "int rp(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                       "    return(x);\n}\n",
                       "rp");
    auto good = fn_from("ret_paren_good.c",
                        "int rq(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                        "    return(x + 1);\n}\n",
                        "rq");
    REQUIRE(bad.kind == "SCALAR");
    REQUIRE(good.kind == "SCALAR");
    for (auto& r : prism::run_wp({bad}, 8)) {
        INFO("wp " << r.status << ": " << r.message);
        CHECK(r.status == prism::laws::FAILED);
        CHECK_FALSE(truthy(r, "wp_qed"));
    }
    for (auto& r : prism::prove_contracts({bad}, 8)) {
        INFO("contracts " << r.status << ": " << r.message);
        CHECK(r.status == prism::laws::FAILED);
    }
    auto w = prism::run_wp({good}, 8);
    REQUIRE(w.size() == 1);
    CHECK(w[0].status == prism::laws::PROVED_ASSUMING);
    auto c = prism::prove_contracts({good}, 8);
    REQUIRE(c.size() == 1);
    CHECK(c[0].status == prism::laws::PROVED_ASSUMING);
}

TEST_CASE("contracts: an empty return in a value-returning function is ERROR, not a proof") {
    auto f = fn_from("contract_empty_return.c",
                     "int ce(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                     "    if (x > 0) return;\n    return x + 1;\n}\n",
                     "ce");
    REQUIRE(f.kind == "SCALAR");
    auto recs = prism::prove_contracts({f}, 8);
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(recs[0].status == prism::laws::ERROR);
    CHECK(extra_or(recs[0], "empty_return") == "true");
    // The same function without the bare return is proved under requires.
    auto twin = fn_from("contract_empty_return_twin.c",
                        "int cf(int x) {\n    // requires: x < 100\n    // ensures: result == x+1\n"
                        "    if (x > 0) return x + 1;\n    return x + 1;\n}\n",
                        "cf");
    auto t = prism::prove_contracts({twin}, 8);
    REQUIRE(t.size() == 1);
    CHECK(t[0].status == prism::laws::PROVED_ASSUMING);
}

TEST_CASE("wp: VOID and OTHER functions with ensures are NEEDS-HARNESS") {
    auto v = fn_from("wp_void.c", "void v(void) {\n    // ensures: result == 0\n    return;\n}\n", "v");
    auto o = fn_from("wp_other.c",
                     "typedef struct S { int a; } S;\nint o(S s) {\n    // ensures: result == 0\n    return 0;\n}\n",
                     "o");
    REQUIRE(v.kind == "VOID");
    REQUIRE(o.kind == "OTHER");
    auto vr = prism::run_wp({v}, 4);
    auto orr = prism::run_wp({o}, 4);
    REQUIRE(vr.size() == 1);
    REQUIRE(orr.size() == 1);
    CHECK(vr[0].status == prism::laws::NEEDS_HARNESS);
    CHECK(orr[0].status == prism::laws::NEEDS_HARNESS);
    CHECK_FALSE(prism::laws::is_proof(vr[0].status));
    CHECK_FALSE(prism::laws::is_proof(orr[0].status));
    CHECK(has(vr[0].message, "VOID"));
    CHECK(has(orr[0].message, "OTHER"));
}

TEST_CASE("wp: ACSL block \\valid is ERROR, never PROVED-ASSUMING") {
    auto recs = prism::run_wp({fn_in("wp_unenc.c", "wp_acsl_valid_bad")}, 4);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    CHECK(r.status == prism::laws::ERROR);
    CHECK(r.status != prism::laws::PROVED_ASSUMING);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(truthy(r, "wp_unencoded"));
    CHECK(has(r.message, "\\valid"));
}

TEST_CASE("wp: arrow member access is ERROR, not a QED of `-` `>`") {
    auto recs = prism::run_wp({fn_in("wp_unenc.c", "wp_arrow_bad")}, 4);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    CHECK(r.status == prism::laws::ERROR);
    CHECK(r.status != prism::laws::PROVED_ASSUMING);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(truthy(r, "wp_unencoded"));
    CHECK(has(r.message, "->"));
}

TEST_CASE("wp: a BMC proof without requires is rewritten PROVED-ASSUMING") {
    // acsl_abs has no requires: the underlying BMC answer is a plain proof,
    // which wp must never pass on as PROVED.
    auto recs = prism::run_wp({fn_in("acsl_abs.c", "acsl_abs")}, 8);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::PROVED_ASSUMING);
    CHECK(has(lower(recs[0].message), "never"));
    auto orig = extra_or(recs[0], "original_status");
    CHECK((orig == prism::laws::PROVED || orig == prism::laws::PROVED_UNBOUNDED));
}

TEST_CASE("wp: an unbounded BMC proof is rewritten PROVED-ASSUMING, never PROVED-UNBOUNDED") {
    auto f = fn_from("wp_loop.c",
                     "int wp_loop_nonneg(int n) {\n"
                     "    // ensures: result >= 0\n"
                     "    int s = 0;\n"
                     "    while (s < n && s < 1000) {\n"
                     "        s = s + 1;\n"
                     "    }\n"
                     "    return s;\n"
                     "}\n",
                     "wp_loop_nonneg");
    auto recs = prism::run_wp({f}, 8);
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(extra_or(recs[0], "original_status") == prism::laws::PROVED_UNBOUNDED);
    CHECK(recs[0].status == prism::laws::PROVED_ASSUMING);
    CHECK(recs[0].status != prism::laws::PROVED_UNBOUNDED);
    CHECK(has(lower(recs[0].message), "never"));
}

TEST_CASE("wp: the false twin of the unbounded loop is not a proof") {
    auto f = fn_from("wp_loop_bad.c",
                     "int wp_loop_neg(int n) {\n"
                     "    // ensures: result > 0\n"
                     "    int s = 0;\n"
                     "    while (s < n && s < 1000) {\n"
                     "        s = s + 1;\n"
                     "    }\n"
                     "    return s;\n"
                     "}\n",
                     "wp_loop_neg");
    auto recs = prism::run_wp({f}, 8);
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(recs[0].status == prism::laws::FAILED);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

// ---- harness

TEST_CASE("harness: the stage runs after bmc, pir and conc") {
    auto b = stage_index("bmc");
    REQUIRE(b >= 0);
    CHECK(stage_index("pir") == b + 1);
    CHECK(stage_index("conc") == b + 2);
    CHECK(stage_index("harness") == b + 3);
}

TEST_CASE("harness: materialize needs an honest requires on a POINTER function") {
    CHECK_FALSE(prism::materialize_harness(fn_in("null_branch.c", "null_branch")).has_value());
    CHECK_FALSE(prism::materialize_harness(scalar_inc()).has_value());

    auto copy = fn_in("ptr_copy.c", "copy");
    CHECK(copy.kind == "POINTER");
    auto h = prism::materialize_harness(copy);
    REQUIRE(h.has_value());
    CHECK((h->kind == "SCALAR" || h->kind == "VOID"));
    CHECK(has(h->body, "_h_"));
    CHECK(has(h->body, "if (!"));

    auto d = prism::materialize_harness(fn_in("ptr_copy.c", "deref_ok"));
    REQUIRE(d.has_value());
    CHECK((d->kind == "SCALAR" || d->kind == "VOID"));
}

TEST_CASE("harness: POINTER without requires is NEEDS-HARNESS, not ERROR") {
    auto f = fn_in("null_branch.c", "null_branch");
    auto recs = prism::run_harness_bmc({f}, 8);
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    CHECK(r.stage == "harness");
    CHECK(r.status == prism::laws::NEEDS_HARNESS);
    for (auto s : {prism::laws::ERROR, prism::laws::CRASH, prism::laws::FAILED, prism::laws::PROVED,
                   prism::laws::NOTRUN})
        CHECK(r.status != s);
    CHECK(r.cls.empty());
    CHECK_FALSE(truthy(r, "assumed"));
    CHECK_FALSE(truthy(r, "harness"));
    CHECK(has(r.message, "no honest requires"));
}

TEST_CASE("harness: SCALAR functions are left to bmc") {
    CHECK(prism::run_harness_bmc({scalar_inc()}, 8).empty());
    auto f = fn_in("null_branch.c", "null_branch");
    auto recs = prism::run_harness_bmc({scalar_inc(), f}, 8);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].function == f.name);
    CHECK(recs[0].status == prism::laws::NEEDS_HARNESS);
}

TEST_CASE("harness: bmc finds ++/-- signed overflow and an uninitialised read") {
    auto f = scalar_inc("inc_ovf");
    for (auto* body : {"x++; return x;", "++x; return x;"}) {
        f.body = body;
        auto r = prism::run_bmc({f}, 8);
        REQUIRE_FALSE(r.empty());
        INFO(body << " -> " << r[0].status << ": " << r[0].message);
        CHECK(r[0].status == prism::laws::FAILED);
        CHECK(r[0].cls == "INT-SIGNED-OVF");
    }
    prism::FunctionInfo u;
    u.file = "synthetic.c";
    u.name = "uninit_read";
    u.kind = "VOID";
    u.line = 1;
    u.signature = "int uninit_read(void)";
    u.body = "int x; return x;";
    auto r = prism::run_bmc({u}, 8);
    REQUIRE_FALSE(r.empty());
    INFO(r[0].status << ": " << r[0].message);
    CHECK(r[0].status == prism::laws::FAILED);
    CHECK(r[0].cls == "UNINIT-READ");
}

TEST_CASE("harness: a materialized proof is PROVED-ASSUMING, never PROVED") {
    for (auto* name : {"copy", "deref_ok"}) {
        INFO(name);
        auto recs = prism::run_harness_bmc({fn_in("ptr_copy.c", name)}, 8);
        REQUIRE_FALSE(recs.empty());
        auto& r = recs[0];
        INFO(r.status << ": " << r.message);
        CHECK(r.stage == "harness");
        CHECK(r.status != prism::laws::ERROR);
        CHECK(r.status == prism::laws::PROVED_ASSUMING);
        CHECK(r.status != prism::laws::PROVED);
        CHECK(r.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(truthy(r, "assumed"));
        CHECK(truthy(r, "harness"));
        CHECK(has(lower(r.message), "never"));
    }
}

TEST_CASE("harness: a step that k-induction cannot close stays BOUNDED") {
    // Only a closed (PROVED/PROVED-UNBOUNDED) havocked step makes a loop
    // PROVED-UNBOUNDED; a violated step keeps the base case's BOUNDED record.
    // The division by zero at i == 500 is real but beyond the unwind bound, so
    // no invariant can close the step and nothing may call it proved.
    auto f = fn_from("kind_open.c",
                     "int kind_open(int n) {\n"
                     "    int i = 0;\n"
                     "    while (i < n && i < 1000) {\n"
                     "        i = i + 1;\n"
                     "    }\n"
                     "    return 100 / (i - 500);\n"
                     "}\n",
                     "kind_open");
    auto r = prism::run_bmc({f}, 8);
    REQUIRE_FALSE(r.empty());
    INFO(r[0].status << ": " << r[0].message);
    CHECK(r[0].status == prism::laws::BOUNDED);
    CHECK(extra_or(r[0], "k_induction") == "step-open");
    CHECK_FALSE(prism::laws::is_proof(r[0].status));
}
