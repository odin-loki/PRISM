// Harness phase 2 (docs/HARNESS_PHASE2.md): contract stubs for callees without a body (--callee-contracts),
// function-pointer fields through ops tables (--pir-ops-auto), ptrtoint / inttoptr constants (--pir-addr-values),
// deeper pointer fields with the bound recorded (--pir-harness-deep).
// Every verdict that rests on a stub, a derived table or an address model must name it; a stub never makes a
// result PROVED; a callee without a usable contract is treated exactly as before.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../../src/prism/pir/callee_contract.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;
namespace L = prism::laws;

bool skip_without_frontend() {
    auto cfg = prism::default_config();
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

const fs::path kDir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";

std::map<std::string, prism::Finding> run(const std::string& file, prism::Config cfg) {
    cfg.root = kDir;
    cfg.jobs = 2;
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({kDir / file}, cfg))
        if (f.function) m[*f.function] = f;
    return m;
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

prism::Config contract_cfg() {
    auto cfg = prism::default_config();
    cfg.callee_contracts = kDir / "callee_contract.tsv";
    return cfg;
}

}  // namespace

// ---- unit tests (no front end) -------------------------------------------------------------------------------

TEST_CASE("callee contracts: the TSV loader needs a status and a source, 7 columns, no duplicates") {
    const auto dir = fs::temp_directory_path() / "prism_cc_loader_test";
    fs::create_directories(dir);
    auto write = [&](const std::string& name, const std::string& text) {
        std::ofstream(dir / name, std::ios::binary) << text;
        return dir / name;
    };
    std::string err;
    auto ok = pp::load_callee_contracts(write("ok.tsv", "# c\nfunction\tsignature\trequires\tassigns\tensures\tstatus\tsource\n"
                                                         "f\tx:s32->s32\t\t\\nothing\t\\result >= 0\tUNVALIDATED\tmined\n"),
                                        err);
    CHECK(err.empty());
    REQUIRE(ok.contains("f"));
    CHECK(ok["f"].status == "UNVALIDATED");
    CHECK(ok["f"].ensures == "\\result >= 0");
    pp::load_callee_contracts(write("nostatus.tsv", "f\tx:s32\t\t\t\t\tsrc\n"), err);
    CHECK(err.find("status") != std::string::npos);
    pp::load_callee_contracts(write("nosource.tsv", "f\tx:s32\t\t\t\tUNVALIDATED\t\n"), err);
    CHECK(err.find("source") != std::string::npos);
    pp::load_callee_contracts(write("cols.tsv", "f\tx:s32\t\t\n"), err);
    CHECK(err.find("7 tab-separated") != std::string::npos);
    pp::load_callee_contracts(write("dup.tsv", "f\t\t\t\t\tU\ts\nf\t\t\t\t\tU\ts\n"), err);
    CHECK(err.find("second contract") != std::string::npos);
    pp::load_callee_contracts(dir / "missing.tsv", err);
    CHECK(err.find("cannot read") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("callee contracts: clauses that cannot be applied are rejected with a reason") {
    auto prob = [](std::string sig, std::string req, std::string asg, std::string ens) {
        pp::CalleeContract c;
        c.function = "f";
        c.signature = std::move(sig);
        c.requires_ = std::move(req);
        c.assigns = std::move(asg);
        c.ensures = std::move(ens);
        c.status = "UNVALIDATED";
        c.source = "test";
        return pp::pirmem::callee_contract_problem(c);
    };
    CHECK(prob("n:u32->s32", "n <= 4096", "\\nothing", "\\result >= 0 && \\result <= n").empty());
    CHECK(prob("p:ptr,n:u64", "\\valid_bytes(p, n)", "\\bytes(p, n)", "").empty());
    CHECK(prob("n:u32->s32", "m <= 4096", "", "").find("unknown name") != std::string::npos);
    CHECK(prob("n:u32", "", "", "\\old(n) == n").find("unsupported ACSL") != std::string::npos);
    CHECK(prob("n:q32", "", "", "").find("unknown type") != std::string::npos);
    CHECK(prob("n", "", "", "").find("no type") != std::string::npos);
    CHECK(prob("n:u32,n:u32", "", "", "").find("repeated") != std::string::npos);
    CHECK(prob("n:u13", "", "", "").find("width") != std::string::npos);
    CHECK(prob("n:s32", "", "", "n >").find("expected an expression") != std::string::npos);
}

TEST_CASE("ops-auto: the table is what the unit stores and initialises, nothing more") {
    const std::string ir =
        "%struct.ops = type { ptr, ptr }\n"
        "@tbl = internal constant %struct.ops { ptr @a, ptr @b }, align 8\n"
        "define internal i32 @a(i32 %x) {\n  ret i32 %x\n}\n"
        "define internal i32 @b(i32 %x) {\n  ret i32 %x\n}\n"
        "define i32 @c(i32 %x) {\n  ret i32 %x\n}\n"
        "define void @init(ptr %p) {\n"
        "  %f = getelementptr inbounds %struct.ops, ptr %p, i32 0, i32 1\n"
        "  store ptr @a, ptr %f, align 8\n"
        "  %g = getelementptr inbounds %struct.ops, ptr %p, i32 0, i32 0\n"
        "  store ptr null, ptr %g, align 8\n"
        "  ret void\n}\n";
    auto m = pp::ir::parse_module(ir);
    auto t = pp::derive_ops_table(m);
    std::map<std::pair<std::string, int>, std::vector<std::string>> got;
    for (auto& f : t) {
        CHECK(f.role == "unit-derived");
        got[{f.struct_type, f.index}] = f.targets;
    }
    REQUIRE(got.contains({"%struct.ops", 0}));
    CHECK(got[{"%struct.ops", 0}] == std::vector<std::string>{"a"});            // initialiser only; null is no target
    REQUIRE(got.contains({"%struct.ops", 1}));
    CHECK(got[{"%struct.ops", 1}] == std::vector<std::string>{"a", "b"});      // store + initialiser
    CHECK(t.size() == 2);
    // @c is never stored: not a target of anything
    for (auto& f : t) CHECK(std::find(f.targets.begin(), f.targets.end(), "c") == f.targets.end());
    // a field with more targets than the limit is left out (the call stays UNENCODED / soft-stopped, never guessed)
    CHECK(pp::derive_ops_table(m, 1).size() == 1);
    // explicit entries win
    std::vector<pp::OpsField> ex;
    pp::OpsField e;
    e.struct_type = "%struct.ops";
    e.index = 1;
    e.targets = {"c"};
    e.role = "explicit";
    ex.push_back(e);
    pp::merge_ops_tables(ex, t);
    REQUIRE(ex.size() == 2);
    CHECK(ex[0].targets == std::vector<std::string>{"c"});
    CHECK(ex[1].index == 0);
}

// ---- contract stubs ------------------------------------------------------------------------------------------

TEST_CASE("callee contracts: default unchanged (without the option every call is UNENCODED)") {
    if (skip_without_frontend()) return;
    auto got = run("callee_contract.c", prism::default_config());
    for (auto* f : {"stub_ensures_ok", "stub_ensures_bad", "stub_requires_ok", "stub_alloc_ok", "stub_false_ensures", "stub_unspecified"}) {
        REQUIRE_MESSAGE(got.contains(f), f);
        CHECK_MESSAGE(got[f].status == L::NEEDS_HARNESS, std::string(f) + ": " + got[f].status);
        CHECK_MESSAGE(has(got[f].message, "UNENCODED: call @hw_"), got[f].message);
    }
}

TEST_CASE("callee contracts: requires checked at the call site, assigns bound the havoc, ensures assumed and named") {
    if (skip_without_frontend()) return;
    auto got = run("callee_contract.c", contract_cfg());
    struct Row {
        const char* fn;
        const char* status;
        const char* tag;  // PROVED-ASSUMING: a substring the assumptions must contain
    };
    const Row rows[] = {
        {"stub_ensures_ok", "PROVED-ASSUMING", "callee-contract: @hw_count"},
        {"stub_ensures_bad", "FAILED", ""},
        {"planted_oob", "FAILED", ""},
        {"planted_oob_ok", "PROVED-ASSUMING", "callee-contract: @hw_count"},
        {"stub_requires_ok", "PROVED-ASSUMING", "callee-contract: @hw_fill"},
        {"stub_requires_bad", "FAILED", ""},
        {"stub_assigns_only_ok", "PROVED-ASSUMING", "assigns \\bytes(buf, n): only those byte ranges are havocked"},
        {"stub_assigns_written_bad", "FAILED", ""},
        {"stub_nothing_ok", "PROVED-ASSUMING", "assigns \\nothing: no memory is written"},
        {"stub_alloc_ok", "PROVED-ASSUMING", "callee-contract: @hw_alloc"},
        {"stub_alloc_null_bad", "FAILED", ""},
        {"stub_alloc_oob_bad", "FAILED", ""},
        {"stub_defined_bad", "FAILED", ""},
    };
    for (auto& r : rows) {
        REQUIRE_MESSAGE(got.contains(r.fn), r.fn);
        auto& f = got[r.fn];
        CHECK_MESSAGE(f.status == r.status, std::string(r.fn) + ": " + f.status + " (" + f.message + ")");
        if (f.status == L::PROVED_ASSUMING && r.tag[0])
            CHECK_MESSAGE(has(extra(f, "assumptions"), r.tag), std::string(r.fn) + ": " + extra(f, "assumptions"));
    }
    // the findings are the right ones
    CHECK(got["stub_ensures_bad"].cls == "INT-DIV-ZERO");
    CHECK(got["planted_oob"].cls == "MEM-OOB-READ");
    CHECK(got["stub_requires_bad"].cls == "FUNC-CONTRACT");
    CHECK(has(got["stub_requires_bad"].message, "violates its contract"));
    CHECK(got["stub_alloc_oob_bad"].cls == "MEM-OOB-READ");
    // a callee without a row, and a row whose signature does not fit the call: as before
    for (auto* f : {"stub_unspecified", "stub_badsig"}) {
        CHECK_MESSAGE(got[f].status == L::NEEDS_HARNESS, std::string(f) + ": " + got[f].status);
        CHECK(has(got[f].message, "UNENCODED: call @hw_"));
    }
}

TEST_CASE("callee contracts: the status of every contract used is in the assumption, and no result is PROVED") {
    if (skip_without_frontend()) return;
    auto got = run("callee_contract.c", contract_cfg());
    for (auto& [name, f] : got) {
        const auto a = extra(f, "assumptions");
        if (has(a, "callee-contract:")) {
            CHECK_MESSAGE(has(a, "status: UNVALIDATED"), name + ": " + a);
            CHECK_MESSAGE(has(a, "the contract was not verified against the callee"), name);
            CHECK_MESSAGE(has(a, "source: test fixture"), name);
            // never PROVED, whatever the stub assumed
            CHECK_MESSAGE(f.status != L::PROVED && f.status != L::PROVED_UNBOUNDED, name + ": " + f.status);
        }
    }
}

TEST_CASE("callee contracts: SOUNDNESS - a false ensures can make a caller proved only with the assumption named") {
    if (skip_without_frontend()) return;
    // 1. with the (false) contract: the caller is "proved" under the stub, and the verdict says so
    auto with = run("callee_contract.c", contract_cfg());
    REQUIRE(with.contains("stub_false_ensures"));
    auto& w = with["stub_false_ensures"];
    CHECK(w.status == L::PROVED_ASSUMING);  // never PROVED
    const auto a = extra(w, "assumptions");
    CHECK(has(a, "callee-contract: @hw_false"));
    CHECK(has(a, "status: UNVALIDATED"));
    CHECK(has(a, "ensures [\\false] is assumed"));
    CHECK(has(a, "the contract was not verified against the callee"));
    // 2. without the contract the same function is the planted bug: the stub is the only reason it is not reported
    auto cfg = prism::default_config();
    cfg.pir_assume_unknown_calls = true;
    auto without = run("callee_contract.c", cfg);
    REQUIRE(without.contains("stub_false_ensures"));
    CHECK(without["stub_false_ensures"].status == L::FAILED);
    CHECK(without["stub_false_ensures"].cls == "INT-DIV-ZERO");
    // 3. an unspecified callee is never silently assumed away
    CHECK(has(extra(without["stub_unspecified"], "assumptions"), "unknown-call: @hw_nocontract"));
}

TEST_CASE("callee contracts: precedence - a contract replaces unknown-call only where it is usable") {
    if (skip_without_frontend()) return;
    auto cfg = contract_cfg();
    cfg.pir_assume_unknown_calls = true;
    auto got = run("callee_contract.c", cfg);
    // contract rows: the stub (tighter than a havoc)
    CHECK(got["stub_ensures_ok"].status == L::PROVED_ASSUMING);
    CHECK(has(extra(got["stub_ensures_ok"], "assumptions"), "callee-contract: @hw_count"));
    CHECK_FALSE(has(extra(got["stub_ensures_ok"], "assumptions"), "unknown-call: @hw_count"));
    // no row, or a row that does not fit: the existing opt-in, named as before
    for (auto* f : {"stub_unspecified", "stub_badsig"}) {
        CHECK_MESSAGE(got[f].status == L::PROVED_ASSUMING, std::string(f) + ": " + got[f].status);
        CHECK(has(extra(got[f], "assumptions"), "unknown-call: @hw_"));
    }
    // a callee with a body keeps its body
    CHECK(got["stub_defined_bad"].status == L::FAILED);
}

// ---- function-pointer fields ---------------------------------------------------------------------------------

TEST_CASE("ops-auto: a harness function-pointer field is bound to the targets the unit assigns") {
    if (skip_without_frontend()) return;
    auto base = prism::default_config();
    base.pir_harness = true;
    // without the option: the field is arbitrary, the call is a null call under the harness
    auto off = run("ops_auto.c", base);
    REQUIRE(off.contains("dev_call_read"));
    CHECK(off["dev_call_read"].status == L::NEEDS_HARNESS);
    CHECK(has(off["dev_call_read"].message, "null-call"));
    // with it
    auto cfg = base;
    cfg.pir_ops_auto = true;
    auto on = run("ops_auto.c", cfg);
    for (auto* f : {"dev_call_read", "dev_call_div_ok"}) {
        REQUIRE_MESSAGE(on.contains(f), f);
        CHECK_MESSAGE(on[f].status == L::PROVED_ASSUMING, std::string(f) + ": " + on[f].status + " (" + on[f].message + ")");
        const auto a = extra(on[f], "assumptions");
        CHECK_MESSAGE(has(a, "function-pointer field %struct.dev_ops."), std::string(f) + ": " + a);
        CHECK_MESSAGE(has(a, "unit-derived"), f);
        CHECK_MESSAGE(has(a, "read_safe"), f);
    }
    // a violation inside a target is still a violation (under the harness: NEEDS-HARNESS naming it)
    CHECK(on["dev_call_div_bad"].status == L::NEEDS_HARNESS);
    CHECK(has(on["dev_call_div_bad"].message, "INT-DIV-ZERO"));
    // a field nothing in the unit assigns stays arbitrary: no guessed target
    CHECK(on["dev_call_unset"].status == L::NEEDS_HARNESS);
    CHECK(has(on["dev_call_unset"].message, "null-call"));
    // the static initialiser is a source of targets
    REQUIRE(on.contains("fixed_read_ok"));
    CHECK(on["fixed_read_ok"].status != L::FAILED);
}

// ---- address values ------------------------------------------------------------------------------------------

TEST_CASE("addr-values: constant ptrtoint / inttoptr are modelled only with the option, and named") {
    if (skip_without_frontend()) return;
    auto off = run("addr_values.c", prism::default_config());
    for (auto* f : {"addr_value", "addr_diff_ok", "err_deref_bad", "marker_deref_bad"}) {
        REQUIRE_MESSAGE(off.contains(f), f);
        CHECK_MESSAGE(off[f].status == L::NEEDS_HARNESS, std::string(f) + ": " + off[f].status);
        CHECK_MESSAGE(has(off[f].message, "UNENCODED"), off[f].message);
    }
    auto cfg = prism::default_config();
    cfg.pir_addr_values = true;
    auto on = run("addr_values.c", cfg);
    CHECK_MESSAGE(on["addr_value"].status == L::PROVED_ASSUMING, on["addr_value"].status + ": " + on["addr_value"].message);
    CHECK(has(extra(on["addr_value"], "assumptions"), "addr-values: a constant ptrtoint"));
    CHECK_MESSAGE(on["addr_diff_ok"].status == L::PROVED_ASSUMING, on["addr_diff_ok"].status + ": " + on["addr_diff_ok"].message);
    CHECK_MESSAGE(on["addr_diff_bad"].status == L::FAILED, on["addr_diff_bad"].status + ": " + on["addr_diff_bad"].message);
    CHECK(on["addr_diff_bad"].cls == "INT-DIV-ZERO");
    // a pointer value with no object: its dereference is a finding, the marker is named
    CHECK_MESSAGE(on["err_deref_bad"].status == L::FAILED, on["err_deref_bad"].status + ": " + on["err_deref_bad"].message);
    CHECK_MESSAGE(on["marker_deref_bad"].status == L::FAILED, on["marker_deref_bad"].status + ": " + on["marker_deref_bad"].message);
    CHECK_MESSAGE(on["marker_is_set"].status == L::PROVED_ASSUMING, on["marker_is_set"].status + ": " + on["marker_is_set"].message);
    CHECK(has(extra(on["marker_is_set"], "assumptions"), "addr-values: the integer constant"));
    CHECK_MESSAGE(on["err_ptr"].status == L::PROVED_ASSUMING, on["err_ptr"].status + ": " + on["err_ptr"].message);
}

// ---- deeper pointer fields -----------------------------------------------------------------------------------

TEST_CASE("harness-deep: depth 4, the bound and what it cut are recorded in the assumptions and the jsonl record") {
    if (skip_without_frontend()) return;
    auto base = prism::default_config();
    base.pir_harness = true;
    const auto path = fs::temp_directory_path() / "prism_pir_harness2_test" / "out.jsonl";
    fs::remove_all(path.parent_path());
    auto shallow = run("harness_deep.c", base);
    CHECK(shallow["list_second"].status == L::PROVED_ASSUMING);
    CHECK(shallow["list_fourth"].status == L::NEEDS_HARNESS);
    auto cfg = base;
    cfg.pir_harness_deep = true;
    cfg.jsonl = path;
    auto deep = run("harness_deep.c", cfg);
    CHECK_MESSAGE(deep["list_fourth"].status == L::PROVED_ASSUMING, deep["list_fourth"].status + ": " + deep["list_fourth"].message);
    CHECK(has(extra(deep["list_fourth"], "assumptions"), "depth bound 4"));
    CHECK(has(extra(deep["list_fourth"], "assumptions"), "deepest object at level 4 of 4"));
    // past the bound: still NEEDS-HARNESS, with the cut recorded
    CHECK(deep["list_fifth"].status == L::NEEDS_HARNESS);
    CHECK(has(extra(deep["list_fifth"], "assumptions"), "1 past the depth bound"));
    REQUIRE(fs::exists(path));
    std::map<std::string, nlohmann::json> rec;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto j = nlohmann::json::parse(line);
        if (j["record"] == "function") rec[j["function"].get<std::string>()] = j;
    }
    REQUIRE(rec.contains("list_fourth"));
    CHECK(rec["list_fourth"]["harness"]["depth_bound"] == 4);
    CHECK(rec["list_fourth"]["harness"]["depth_reached"] == 4);
    CHECK(rec["list_fourth"]["harness"]["cut_past_depth"] == 0);
    CHECK(rec["list_fifth"]["harness"]["cut_past_depth"] == 1);
    fs::remove_all(path.parent_path());
}

TEST_CASE("callee contracts: the jsonl record lists the stubs a verdict rests on, with status") {
    if (skip_without_frontend()) return;
    auto cfg = contract_cfg();
    const auto path = fs::temp_directory_path() / "prism_pir_harness2_test2" / "out.jsonl";
    fs::remove_all(path.parent_path());
    cfg.jsonl = path;
    auto got = run("callee_contract.c", cfg);
    std::map<std::string, nlohmann::json> rec;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto j = nlohmann::json::parse(line);
        if (j["record"] == "function") rec[j["function"].get<std::string>()] = j;
    }
    REQUIRE(rec.contains("stub_false_ensures"));
    auto& r = rec["stub_false_ensures"];
    CHECK(r["verdict"] == "PROVED-ASSUMING");
    REQUIRE(r.contains("callee_stubs"));
    CHECK(r["callee_stubs"][0]["callee"] == "hw_false");
    CHECK(r["callee_stubs"][0]["status"] == "UNVALIDATED");
    CHECK(rec["stub_unspecified"].contains("callee_stubs") == false);
    fs::remove_all(path.parent_path());
}
