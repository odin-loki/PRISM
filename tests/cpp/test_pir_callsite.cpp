// Doctests: grounded and whole-build call-site discharge of the automatic pointer harness
// (--pir-discharge-closure, --pir-discharge-out, --pir-obligations, prism-qa callsite-discharge;
// docs/HARNESS_GENERATOR.md "Whole-build call-site discharge", docs/PIR.md).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../../src/prism/pir/discharge.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace D = prism::pir::discharge;
namespace fs = std::filesystem;
namespace L = prism::laws;
using nlohmann::json;

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

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

std::map<std::string, prism::Finding> run_files(prism::Config cfg, const std::vector<fs::path>& files) {
    cfg.root = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.jobs = 1;
    std::map<std::string, prism::Finding> st;
    for (auto& f : pp::run_pir(files, cfg))
        if (f.function) st[*f.function] = f;
    return st;
}

std::map<std::string, prism::Finding> run_callsite(bool closure) {
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    cfg.pir_discharge_closure = closure;
    return run_files(cfg, {fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir" / "harness_callsite.c"});
}

fs::path scratch(const std::string& name) {
    static std::atomic<int> n{0};
    auto d = fs::temp_directory_path() /
             ("prism-callsite-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
              std::to_string(n++));
    fs::create_directories(d);
    return d;
}

prism::CliResult<prism::CliOptions> parse_args(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
}

}  // namespace

TEST_CASE("pir callsite: a grounded callee is PROVED, the chain above it too (--pir-discharge-closure)") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(true);
    for (auto* ok : {"leaf_ok", "leaf_chain", "mid_chain"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        CHECK_MESSAGE(st[ok].status == L::PROVED, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        CHECK_MESSAGE(extra(st[ok], "harness_discharge").starts_with("discharged and grounded at"),
                      (std::string(ok) + ": " + extra(st[ok], "harness_discharge")));
        CHECK_MESSAGE(extra(st[ok], "assumptions").empty(), (std::string(ok) + ": " + extra(st[ok], "assumptions")));
        CHECK_MESSAGE(extra(st[ok], "verdict_before_discharge").starts_with("PROVED-ASSUMING"), ok);
    }
}

TEST_CASE("pir callsite: without the closure the discharged callee stays PROVED-ASSUMING") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(false);
    REQUIRE(st.contains("leaf_ok"));
    CHECK(st["leaf_ok"].status == L::PROVED_ASSUMING);
    CHECK(extra(st["leaf_ok"], "harness_discharge").starts_with("discharged at"));
}

TEST_CASE("pir callsite: a precondition with one unsatisfying caller stays an assumption") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(true);
    REQUIRE(st.contains("leaf_bad"));
    CHECK(st["leaf_bad"].status == L::PROVED_ASSUMING);
    const auto d = extra(st["leaf_bad"], "harness_discharge");
    CHECK_MESSAGE(d.starts_with("not discharged"), d);
    CHECK_MESSAGE(d.find("top_bad_null") != std::string::npos, d);
    CHECK(extra(st["leaf_bad"], "assumptions").find("(auto harness)") != std::string::npos);
}

TEST_CASE("pir callsite: an exported (external) function stays an assumption") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(true);
    REQUIRE(st.contains("leaf_ext"));
    CHECK(st["leaf_ext"].status == L::PROVED_ASSUMING);
    const auto d = extra(st["leaf_ext"], "harness_discharge");
    CHECK_MESSAGE(d.find("external linkage") != std::string::npos, d);
}

TEST_CASE("pir callsite: an indirect caller (address taken) blocks the discharge") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(true);
    REQUIRE(st.contains("leaf_taken"));
    CHECK(st["leaf_taken"].status == L::PROVED_ASSUMING);
    const auto d = extra(st["leaf_taken"], "harness_discharge");
    CHECK_MESSAGE(d.find("address is taken") != std::string::npos, d);
}

TEST_CASE("pir callsite: an entry caller with a pointer parameter discharges but does not ground") {
    if (skip_without_frontend()) return;
    auto st = run_callsite(true);
    REQUIRE(st.contains("leaf_entry"));
    CHECK(st["leaf_entry"].status == L::PROVED_ASSUMING);
    CHECK_MESSAGE(extra(st["leaf_entry"], "harness_discharge").starts_with("discharged at"), extra(st["leaf_entry"], "harness_discharge"));
    const auto g = extra(st["leaf_entry"], "discharge_grounding");
    CHECK_MESSAGE(g.starts_with("not grounded"), g);
    CHECK_MESSAGE(g.find("entry_with_param") != std::string::npos, g);
}

TEST_CASE("pir callsite: a global argument is not the harness's fresh object (ob-origin)") {
    if (skip_without_frontend()) return;
    // with the alias the callee divides by zero; the harness took its object distinct from every global
    auto st = run_callsite(true);
    REQUIRE(st.contains("leaf_global"));
    CHECK(st["leaf_global"].status == L::PROVED_ASSUMING);
    const auto d = extra(st["leaf_global"], "harness_discharge");
    CHECK_MESSAGE(d.starts_with("not discharged"), d);
    CHECK_MESSAGE(d.find("top_global") != std::string::npos, d);
    CHECK_MESSAGE(d.find("not a local") != std::string::npos, d);
}

TEST_CASE("pir callsite: obligation plans round-trip through JSON") {
    auto o = std::make_shared<pp::ObligationNode>();
    o->name = "d";
    o->bytes = 32;
    o->member_off = 8;
    o->align = 8;
    o->check_init = false;
    auto c = std::make_shared<pp::ObligationNode>();
    c->name = "d->@16";
    c->bytes = 4;
    o->nested.emplace_back(16, c);
    auto back = D::obligation_from_json(D::obligation_json(*o));
    REQUIRE(back);
    CHECK(back->bytes == 32);
    CHECK(back->member_off == 8);
    CHECK(back->align == 8);
    CHECK(!back->check_init);
    REQUIRE(back->nested.size() == 1);
    CHECK(back->nested[0].first == 16);
    CHECK(back->nested[0].second->bytes == 4);
    CHECK(back->nested[0].second->check_init);
    CHECK(!D::obligation_from_json(json{{"name", "x"}}));  // no size: refused, never a weaker plan
}

TEST_CASE("pir callsite: the grounding never grounds a cycle, a failed site or an open caller") {
    D::Grounder G;
    auto site = [&](const std::string& c, const std::string& g, const char* verdict, bool caller_harness,
                    std::vector<std::string> as = {}) {
        D::Site s;
        s.caller = c;
        s.callee = g;
        s.sites = 1;
        s.verdict = verdict;
        if (caller_harness) {
            s.caller_harness = {"x points to a valid object (auto harness)"};
            as.push_back(s.caller_harness[0]);
        }
        s.assumptions = as;
        G.sites[{c, g}] = s;
    };
    auto node = [&](const std::string& k, bool closed, std::vector<std::string> callers) {
        D::Node n;
        n.display = k;
        n.closed = closed;
        n.why_not = closed ? "" : "external linkage";
        n.callers = std::move(callers);
        G.nodes[k] = n;
    };
    // a <- b <- c (c has no harness): grounded, b's context assumption is inherited
    node("a", true, {"b"});
    node("b", true, {"c"});
    site("b", "a", "PROVED", true);
    site("c", "b", "PROVED", false, {"unknown-call: @h ..."});
    auto ga = G.ground("a");
    CHECK(ga.ok);
    REQUIRE(ga.inherited.size() == 1);
    CHECK(ga.inherited[0].starts_with("unknown-call"));
    // a cycle p <-> q
    node("p", true, {"q"});
    node("q", true, {"p"});
    site("q", "p", "PROVED", true);
    site("p", "q", "PROVED", true);
    auto gp = G.ground("p");
    CHECK(!gp.ok);
    CHECK(gp.reason.find("recursion") != std::string::npos);
    // a FAILED site
    node("f", true, {"g1", "g2"});
    site("g1", "f", "PROVED", false);
    site("g2", "f", "FAILED", false);
    CHECK(!G.ground("f").ok);
    // a caller that is not closed (an entry)
    node("e", true, {"entry"});
    node("entry", false, {});
    site("entry", "e", "PROVED", true);
    auto ge = G.ground("e");
    CHECK(!ge.ok);
    CHECK(ge.reason.find("external linkage") != std::string::npos);
    // a missing site
    node("m", true, {"x"});
    CHECK(!G.ground("m").ok);
    // the outcome: PROVED only with nothing left
    D::Grounding ok;
    ok.ok = true;
    auto o1 = D::outcome("PROVED", {"p points to ... (auto harness)", "auto harness for p: nested pointer depth bound 2"}, {}, false, ok, "x");
    CHECK(o1.proved);
    CHECK(o1.status == L::PROVED);
    auto o2 = D::outcome("PROVED", {"p points to ... (auto harness)", "kernel-stop: ..."}, {}, false, ok, "x");
    CHECK(!o2.proved);
    CHECK(o2.status == L::PROVED_ASSUMING);
    auto o3 = D::outcome("PROVED", {"p (auto harness)"}, {}, true, ok, "x");
    CHECK(!o3.proved);
    CHECK(o3.assumptions.back() == D::invented_objects_line());
    auto o4 = D::outcome("PROVED", {"p (auto harness)"}, {"the bytes of p ... (auto harness)"}, false, ok, "x");
    CHECK(!o4.proved);
    D::Grounding bad;
    auto o5 = D::outcome("PROVED", {"p (auto harness)"}, {}, false, bad, "x");
    CHECK(!o5.proved);
}

TEST_CASE("pir callsite: whole build, two units of one module (prism-qa callsite-discharge)") {
    if (skip_without_frontend()) return;
    const auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir" / "callsite";
    const auto a = dir / "a.c", b = dir / "b.c";
    const auto tmp = scratch("whole");
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    cfg.pir_discharge_out = tmp / "pass1.jsonl";
    auto st = run_files(cfg, {a, b});
    REQUIRE(st.contains("ext_leaf"));
    CHECK(st["ext_leaf"].status == L::PROVED_ASSUMING);  // the unit alone never decides an external function
    auto write_db = [&](const fs::path& p, bool with_missing, bool with_asm) {
        json db = json::array();
        auto entry = [&](const fs::path& f) {
            return json{{"directory", dir.generic_string()},
                        {"file", f.generic_string()},
                        {"arguments", {"clang", "-DMODULE", "-DKBUILD_MODNAME=\"demo\"", "-D__KBUILD_MODNAME=kmod_demo", "-c",
                                       f.generic_string()}}};
        };
        db.push_back(entry(a));
        db.push_back(entry(b));
        if (with_missing) db.push_back(entry(dir / "c_not_run.c"));
        if (with_asm) db.push_back(entry(dir / "entry.S"));
        db.push_back(json{{"directory", dir.generic_string()}, {"file", (dir / "core.c").generic_string()},
                          {"arguments", {"clang", "-c", "core.c"}}});  // vmlinux: another link unit
        std::ofstream(p) << db.dump(1);
    };
    write_db(tmp / "db.json", false, false);
    D::WholeBuildOptions o;
    o.inputs = {tmp / "pass1.jsonl"};
    o.compile_dbs = {tmp / "db.json"};
    o.complete_db = true;
    o.link_rule = "linux";
    o.config = "test";
    auto decisions = [](const D::WholeBuildResult& r) {
        std::map<std::string, json> m;
        for (const auto& d : r.decisions) m[d.value("function", "")] = d;
        return m;
    };
    auto r1 = D::whole_build(o);
    CHECK(r1.errors.empty());
    auto d1 = decisions(r1);
    REQUIRE(d1.contains("ext_leaf"));
    CHECK(d1["ext_leaf"]["level"] == "none");  // b.c's call site is not checked yet
    CHECK(d1["ext_leaf"]["reason"].get<std::string>().find("not checked") != std::string::npos);
    // the work list names b.c with the plans of ext_leaf and ext_bad
    json todo_b;
    for (const auto& t : r1.todo)
        if (fs::path(t.value("file", "")).filename() == "b.c") todo_b = t;
    REQUIRE(todo_b.is_object());
    std::set<std::string> planned;
    for (const auto& p : todo_b["plans"]) planned.insert(p.value("function", ""));
    CHECK(planned.count("ext_leaf"));
    CHECK(planned.count("ext_bad"));
    CHECK(!planned.count("ext_exported"));  // address taken: never closed, nothing to check
    // pass 2: b.c checks the plans at its call sites
    {
        std::ofstream pf(tmp / "plans_b.jsonl");
        for (const auto& p : todo_b["plans"]) pf << p.dump() << "\n";
    }
    auto cfg2 = cfg;
    cfg2.pir_discharge_out = tmp / "pass2.jsonl";
    cfg2.pir_obligations = tmp / "plans_b.jsonl";
    (void)run_files(cfg2, {b});
    o.inputs = {tmp / "pass1.jsonl", tmp / "pass2.jsonl"};
    auto r2 = D::whole_build(o);
    auto d2 = decisions(r2);
    REQUIRE(d2.contains("ext_leaf"));
    CHECK_MESSAGE(d2["ext_leaf"]["level"] == "proved", d2["ext_leaf"].dump());
    CHECK(d2["ext_leaf"]["verdict"] == "PROVED");
    CHECK(d2["ext_leaf"]["sites"] == 2);
    // one unsatisfying caller (b_bad passes 8 bytes): stays an assumption
    REQUIRE(d2.contains("ext_bad"));
    CHECK(d2["ext_bad"]["level"] == "none");
    CHECK(d2["ext_bad"]["verdict"] == "PROVED-ASSUMING");
    CHECK_MESSAGE(d2["ext_bad"]["reason"].get<std::string>().find("b_bad") != std::string::npos, d2["ext_bad"].dump());
    // exported (address taken by __ADDRESSABLE): stays an assumption
    REQUIRE(d2.contains("ext_exported"));
    CHECK(d2["ext_exported"]["level"] == "none");
    CHECK(d2["ext_exported"]["reason"].get<std::string>().find("address is taken") != std::string::npos);
    // called through a function pointer in b.c: an unresolved indirect caller blocks it
    REQUIRE(d2.contains("ext_indirect"));
    CHECK(d2["ext_indirect"]["level"] == "none");
    CHECK_MESSAGE(d2["ext_indirect"]["reason"].get<std::string>().find("address is taken in") != std::string::npos,
                  d2["ext_indirect"].dump());
    // without a link rule nothing external is closed
    auto on = o;
    on.link_rule = "none";
    auto d3 = decisions(D::whole_build(on));
    CHECK(d3["ext_leaf"]["level"] == "none");
    CHECK(d3["ext_leaf"]["reason"].get<std::string>().find("external linkage") != std::string::npos);
    // nor with a database not declared complete (it may be filtered to C)
    auto oc = o;
    oc.complete_db = false;
    auto d3c = decisions(D::whole_build(oc));
    CHECK(d3c["ext_leaf"]["level"] == "none");
    CHECK(d3c["ext_leaf"]["reason"].get<std::string>().find("--complete-db") != std::string::npos);
    // a unit of the module without call-graph facts (unknown callers) blocks it
    write_db(tmp / "db_missing.json", true, false);
    auto om = o;
    om.compile_dbs = {tmp / "db_missing.json"};
    auto rm = D::whole_build(om);
    auto d4 = decisions(rm);
    CHECK(d4["ext_leaf"]["level"] == "none");
    CHECK(d4["ext_leaf"]["reason"].get<std::string>().find("no call-graph facts") != std::string::npos);
    bool facts_todo = false;
    for (const auto& t : rm.todo)
        if (fs::path(t.value("file", "")).filename() == "c_not_run.c" && t.value("facts", false)) facts_todo = true;
    CHECK(facts_todo);
    // an assembler unit in the module may call it
    write_db(tmp / "db_asm.json", false, true);
    auto oa = o;
    oa.compile_dbs = {tmp / "db_asm.json"};
    auto d5 = decisions(D::whole_build(oa));
    CHECK(d5["ext_leaf"]["level"] == "none");
    CHECK(d5["ext_leaf"]["reason"].get<std::string>().find("assembler") != std::string::npos);
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST_CASE("cli: --pir-discharge-closure, --pir-discharge-out, --pir-obligations") {
    auto r = parse_args({"fixture.c", "--pir-harness", "--pir-discharge", "--pir-discharge-closure", "--pir-discharge-out", "d.jsonl",
                         "--pir-obligations", "p.jsonl"});
    REQUIRE(r);
    CHECK(r->cfg.pir_discharge_closure);
    CHECK(r->cfg.pir_discharge_out.filename() == "d.jsonl");
    CHECK(r->cfg.pir_obligations.filename() == "p.jsonl");
    auto d = parse_args({"fixture.c", "--pir-harness", "--pir-discharge"});
    REQUIRE(d);
    CHECK(!d->cfg.pir_discharge_closure);
    CHECK(d->cfg.pir_discharge_out.empty());
    CHECK(d->cfg.pir_obligations.empty());
}
