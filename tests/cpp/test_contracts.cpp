// Doctests: Dafny-style / ACSL contracts (requires, ensures, invariant,
// decreases) proved by BMC as PROVED-ASSUMING, never PROVED; decreases
// well-formedness; the FuSeBMC helpers that seed from BMC counterexamples.
// Linked into prism_tests next to test_main.cpp.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai_proof.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/stages/common.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace laws = prism::laws;
namespace sd = prism::stages_detail;

fs::path td_root() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / "testdata";
}

prism::FunctionInfo tfn(const char* file, const char* name) {
    auto p = td_root() / file;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << file);
    return {};
}

std::string xget(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::string nospace(std::string s) {
    std::erase_if(s, [](char c) { return c == ' '; });
    return s;
}

bool any_has(const std::vector<std::string>& xs, std::string_view compact) {
    for (auto& x : xs)
        if (has(nospace(x), compact)) return true;
    return false;
}

prism::Finding prove1(const prism::FunctionInfo& fn) {
    auto recs = prism::prove_contracts({fn}, 8);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].stage == "contracts");
    return recs[0];
}

void require_not_proof(const prism::Finding& f) {
    CHECK(f.status != std::string(laws::PROVED));
    CHECK(f.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(f.status != std::string(laws::PROVED_ASSUMING));
    CHECK(f.status != std::string(laws::CLEAN));
    CHECK_FALSE(laws::is_proof(f.status));
}

void require_unencoded(const prism::Finding& f) {
    CHECK(f.status == std::string(laws::ERROR));
    require_not_proof(f);
    CHECK(xget(f, "decreases_unencoded") == "true");
}

}  // namespace

// ---- clause parsing ----

TEST_CASE("contracts parse // requires / ensures / decreases / invariant") {
    auto inc = sd::parse_comments(tfn("contract_add.c", "inc"));
    CHECK(any_has(inc.requires_, "x<100"));
    CHECK(any_has(inc.ensures, "result"));
    CHECK(any_has(inc.ensures, "x+1"));
    CHECK(sd::parse_comments(tfn("decreases_loop.c", "countdown")).decreases == std::vector<std::string>{"i"});
    CHECK(sd::parse_comments(tfn("decreases_complex.c", "countdown_star")).decreases ==
          std::vector<std::string>{"*"});
    auto inv = sd::parse_comments(tfn("invariant_loop.c", "sum_inv"));
    REQUIRE_FALSE(inv.invariant.empty());
    CHECK(any_has(inv.invariant, "s>=0"));
    CHECK(sd::parse_comments(tfn("decreases_loop.c", "countdown_open")).requires_.empty());
}

TEST_CASE("contracts parse ACSL, rewrite \\result, and do not leak to the next function") {
    auto spec = sd::parse_comments(tfn("acsl_abs.c", "acsl_abs"));
    CHECK(any_has(spec.requires_, "x>-2147483647"));
    CHECK(any_has(spec.ensures, "result>=0"));
    for (auto& e : spec.ensures) CHECK_FALSE(has(e, "\\result"));
    auto plain = sd::parse_comments(tfn("acsl_abs.c", "acsl_plain"));
    CHECK(plain.requires_.empty());
    CHECK(plain.ensures.empty());
}

TEST_CASE("contracts clause keywords are case-insensitive in // and ACSL") {
    auto caps = sd::parse_comments(tfn("contract_case.c", "inc_caps"));
    CHECK(caps.requires_ == std::vector<std::string>{"x < 100"});
    CHECK(caps.ensures == std::vector<std::string>{"result == x + 1"});
    auto acsl = sd::parse_comments(tfn("contract_case.c", "abs_caps"));
    CHECK(any_has(acsl.requires_, "x>-2147483647"));
    CHECK(any_has(acsl.ensures, "result>=0"));
}

TEST_CASE("contracts capitalised clauses count as a spec for drafting and for rapid") {
    // Drafting skips a function that already has a spec, in any case.
    CHECK(prism::ai::has_spec(tfn("contract_case.c", "inc_caps")));
    CHECK(prism::ai::has_spec(tfn("contract_case.c", "inc_caps_bad")));
    CHECK(prism::ai::has_spec(tfn("contract_case.c", "abs_caps")));
    CHECK_FALSE(prism::ai::has_spec(tfn("fsm_recur.c", "fsm_recur")));
    // A plain comment block above is not a spec.
    CHECK_FALSE(prism::ai::has_spec(tfn("fsm_recur.c", "fsm_settle")));
    // rapid reads `// Requires:` / `// ENSURES:` as its property; with no
    // clause there is nothing to sample.
    auto ok = prism::run_rapid({tfn("contract_case.c", "inc_caps")}, 16);
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].status == std::string(laws::CLEAN));
    CHECK(ok[0].extra.at("ensures") == R"(["result == x + 1"])");
    CHECK(ok[0].extra.at("requires") == R"(["x < 100"])");
    auto bad = prism::run_rapid({tfn("contract_case.c", "inc_caps_bad")}, 16);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].status == std::string(laws::FAILED));
    CHECK(bad[0].cls == "FUNC-CONTRACT");
    CHECK(has(bad[0].message, "ensures (result == x) failed"));
    CHECK(prism::run_rapid({tfn("fsm_recur.c", "fsm_recur")}, 16).empty());
}

// ---- PROVED-ASSUMING, never PROVED ----

#ifdef PRISM_HAS_Z3
TEST_CASE("contracts inc is PROVED-ASSUMING; without the requires it overflows") {
    auto fn = tfn("contract_add.c", "inc");
    auto r = prove1(fn);
    CHECK(r.status == std::string(laws::PROVED_ASSUMING));
    CHECK(r.status != std::string(laws::PROVED));
    CHECK(r.extra.contains("requires"));
    CHECK(xget(r, "assumed") == "true");
    auto raw = prism::run_bmc({fn}, 8);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].status == std::string(laws::FAILED));
    CHECK(raw[0].cls == "INT-SIGNED-OVF");
}

TEST_CASE("contracts capitalised clauses are checked: holds and its false twin fails") {
    auto ok = prove1(tfn("contract_case.c", "inc_caps"));
    CHECK(ok.status == std::string(laws::PROVED_ASSUMING));
    CHECK(xget(ok, "ensures") == "result == x + 1");
    auto bad = prove1(tfn("contract_case.c", "inc_caps_bad"));
    CHECK(bad.status == std::string(laws::FAILED));
    require_not_proof(bad);
    auto acsl = prove1(tfn("contract_case.c", "abs_caps"));
    CHECK(acsl.status == std::string(laws::PROVED_ASSUMING));
    CHECK(xget(acsl, "assumed") == "true");
}

TEST_CASE("contracts ACSL acsl_abs is PROVED-ASSUMING never PROVED") {
    auto r = prove1(tfn("acsl_abs.c", "acsl_abs"));
    CHECK(r.status == std::string(laws::PROVED_ASSUMING));
    CHECK(r.status != std::string(laws::PROVED));
    CHECK(r.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(xget(r, "assumed") == "true");
    CHECK(r.extra.contains("requires"));
}

TEST_CASE("contracts loop invariant is PROVED-ASSUMING and encoded") {
    auto r = prove1(tfn("invariant_loop.c", "sum_inv"));
    CHECK(r.status == std::string(laws::PROVED_ASSUMING));
    CHECK(r.status != std::string(laws::PROVED));
    CHECK(r.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(r.extra.contains("invariant"));
    CHECK(xget(r, "invariant_encoded") == "true");
    CHECK(has(r.message, "invariant"));
}
#endif

// ---- decreases ----

TEST_CASE("contracts star and call decreases are ERROR, never PROVED-ASSUMING") {
    auto star = prove1(tfn("decreases_complex.c", "countdown_star"));
    require_unencoded(star);
    CHECK(has(star.message, "*"));
    auto absd = prove1(tfn("decreases_complex.c", "countdown_abs"));
    require_unencoded(absd);
    CHECK(has(absd.message, "abs(n)"));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("contracts identifier decreases is encoded and PROVED-ASSUMING") {
    auto r = prove1(tfn("decreases_loop.c", "countdown"));
    CHECK(r.status == std::string(laws::PROVED_ASSUMING));
    CHECK(r.status != std::string(laws::PROVED));
    CHECK(r.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(xget(r, "decreases_unencoded") != "true");
    CHECK(xget(r, "decreases") == "i");
    CHECK(xget(r, "decreases_encoded") == "true");
}

TEST_CASE("contracts open countdown is never PROVED-UNBOUNDED") {
    auto r = prove1(tfn("decreases_loop.c", "countdown_open"));
    CHECK(r.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(r.status != std::string(laws::PROVED));
    CHECK(xget(r, "original_status") != std::string(laws::PROVED_UNBOUNDED));
    auto req = xget(r, "requires");
    CHECK_FALSE((has(req, "n >= 0") && has(req, "n < 8")));
}

// A linear measure is checked, not trusted. countdown_complex claims
// `n - i`, which grows as i counts down: the ranking assert refutes it. This
// used to be ERROR (decreases_unencoded); FAILED is the correct verdict.
TEST_CASE("contracts compound decreases n - i that grows is FAILED, not assumed") {
    auto fn = tfn("decreases_complex.c", "countdown_complex");
    auto spec = sd::parse_comments(fn);
    REQUIRE_FALSE(spec.decreases.empty());
    CHECK(has(spec.decreases[0], "-"));
    auto r = prove1(fn);
    CHECK(r.status == std::string(laws::FAILED));
    require_not_proof(r);
    CHECK(xget(r, "decreases_unencoded") != "true");
    CHECK(xget(r, "decreases") == "n - i");
    CHECK(xget(r, "decreases_encoded") == "true");
    // The refutation is the ranking assert, not the ensures or an overflow:
    // the same requires / ensures without the decreases clause proves.
    CHECK(r.cls == "FUNC-CONTRACT");
    CHECK(has(r.message, "FUNC-CONTRACT"));
    CHECK(xget(r, "decreases_assumed") == "false");
    auto without = sd::bmc_with_assume(fn, 8, xget(r, "requires"), xget(r, "ensures"), std::nullopt, std::nullopt);
    CHECK(without.status == std::string(laws::PROVED_ASSUMING));
}

TEST_CASE("contracts valid linear decreases are PROVED-ASSUMING; overflowing measure is FAILED") {
    for (auto* name : {"countdown_lin", "countup_gap"}) {
        CAPTURE(name);
        auto r = prove1(tfn("decreases_linear.c", name));
        CHECK(r.status == std::string(laws::PROVED_ASSUMING));
        CHECK(r.status != std::string(laws::PROVED_UNBOUNDED));
        CHECK(xget(r, "decreases_encoded") == "true");
    }
    auto ovf_fn = tfn("decreases_linear.c", "countdown_ovf");
    auto ovf = prove1(ovf_fn);
    CHECK(ovf.status == std::string(laws::FAILED));
    require_not_proof(ovf);
    // The failure is signed overflow in the measure itself: the same
    // function proves without the clause and with a measure that fits.
    CHECK(ovf.cls == "INT-SIGNED-OVF");
    CHECK(has(ovf.message, "INT-SIGNED-OVF"));
    auto req = xget(ovf, "requires"), ens = xget(ovf, "ensures");
    CHECK(sd::bmc_with_assume(ovf_fn, 8, req, ens, std::nullopt, std::nullopt).status ==
          std::string(laws::PROVED_ASSUMING));
    CHECK(sd::bmc_with_assume(ovf_fn, 8, req, ens, std::string("i - 0"), std::nullopt).status ==
          std::string(laws::PROVED_ASSUMING));
}
#endif

// ---- scalar subset only ----

TEST_CASE("contracts POINTER and ACSL POINTER are NEEDS-HARNESS") {
    auto r = prove1(tfn("wp_ptr.c", "wp_ptr_get"));
    CHECK(r.status == std::string(laws::NEEDS_HARNESS));
    require_not_proof(r);
    CHECK(has(r.message, "POINTER"));
    auto acsl = prove1(tfn("wp_ptr.c", "wp_acsl_ptr"));
    CHECK(acsl.status == std::string(laws::NEEDS_HARNESS));
    require_not_proof(acsl);
}

TEST_CASE("contracts VOID and OTHER functions are NEEDS-HARNESS") {
    auto dir = fs::temp_directory_path() / "prism_contracts_kinds";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto src = dir / "x.c";
    std::ofstream(src) << "void v(void) {\n    // ensures: result == 0\n    return;\n}\n"
                          "int o(S s) {\n    // ensures: result == 0\n    return 0;\n}\n";
    prism::FunctionInfo vfn;
    vfn.file = src.string();
    vfn.name = "v";
    vfn.kind = "VOID";
    vfn.line = 1;
    vfn.signature = "void v(void)";
    vfn.body = "return;";
    vfn.return_type = "void";
    prism::FunctionInfo ofn;
    ofn.file = src.string();
    ofn.name = "o";
    ofn.kind = "OTHER";
    ofn.line = 5;
    ofn.signature = "int o(S s)";
    ofn.params = {{"S", "s"}};
    ofn.body = "return 0;";
    auto v = prove1(vfn);
    auto o = prove1(ofn);
    CHECK(v.status == std::string(laws::NEEDS_HARNESS));
    CHECK(o.status == std::string(laws::NEEDS_HARNESS));
    require_not_proof(v);
    require_not_proof(o);
    CHECK(has(v.message, "VOID"));
    CHECK(has(o.message, "OTHER"));
    fs::remove_all(dir, ec);
}

// ---- FuSeBMC helpers and the differential stage ----

#ifdef PRISM_HAS_Z3
TEST_CASE("fuse seeds come from a BMC counterexample") {
    auto fn = tfn("add_overflow.c", "add_overflow");
    auto bmc = prism::run_bmc({fn}, 8);
    REQUIRE(bmc.size() == 1);
    CHECK(bmc[0].status == std::string(laws::FAILED));
    auto seeds = sd::seeds_from_bmc(fn, bmc);
    REQUIRE_FALSE(seeds.empty());
    REQUIRE(seeds[0].size() == 4);
    // Little-endian x from the counterexample; it must overflow x + 100.
    uint32_t u = 0;
    for (int i = 3; i >= 0; --i) u = (u << 8) | seeds[0][static_cast<std::size_t>(i)];
    auto x = static_cast<int32_t>(u);
    CHECK(static_cast<long long>(x) + 100 > 2147483647LL);
    // A solver bit-vector literal and a plain C literal decode alike.
    prism::Finding hex = bmc[0], dec = bmc[0];
    hex.counterexample = "x=#x7fffff9c";
    dec.counterexample = "x=2147483548";
    auto hs = sd::seeds_from_bmc(fn, {hex});
    auto ds = sd::seeds_from_bmc(fn, {dec});
    REQUIRE(hs.size() == 1);
    CHECK(hs == ds);
    CHECK(hs[0] == std::vector<uint8_t>{0x9c, 0xff, 0xff, 0x7f});
    // Not FAILED, or another function: no seed.
    prism::Finding other = hex;
    other.function = "not_add_overflow";
    CHECK(sd::seeds_from_bmc(fn, {other}).empty());
    prism::Finding bounded = hex;
    bounded.status = std::string(laws::BOUNDED);
    CHECK(sd::seeds_from_bmc(fn, {bounded}).empty());
}
#endif

TEST_CASE("fuse branch goals and CLEAN is not a proof") {
    auto fn = tfn("saturate.c", "saturate");
    CHECK(sd::branch_goals(fn).size() >= 1);
    auto recs = prism::run_fuse({fn}, {}, td_root(), 0.4, 8, false);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == std::string(laws::CLEAN));
    CHECK_FALSE(laws::is_proof(recs[0].status));
    CHECK(recs[0].status != std::string(laws::PROVED));
    CHECK(recs[0].status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(recs[0].status != std::string(laws::PROVED_ASSUMING));
    CHECK(has(recs[0].message, "not a proof"));
}

TEST_CASE("diff of disagreeing _a / _b pair is FAILED with a counterexample") {
    prism::sandbox::Policy allow(true);
    std::vector<prism::FunctionInfo> funcs;
    for (auto* file : {"diff_a.c", "diff_b.c"}) {
        auto p = td_root() / file;
        for (auto& f : prism::extract_functions(p, file)) funcs.push_back(f);
    }
    auto recs = prism::run_diff(funcs, td_root());
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == std::string(laws::FAILED));
    CHECK_FALSE(recs[0].counterexample.empty());
}
