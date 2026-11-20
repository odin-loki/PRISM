// Doctests: automatic pointer harness (--pir-harness), the --jsonl records
// with levels and assumptions, and the Lean-fragment coverage field
// (docs/PIR.md "Automatic pointer harness", "Machine-readable output").
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../../src/prism/pir/jsonl.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace fs = std::filesystem;

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

std::map<std::string, prism::Finding> run(prism::Config cfg) {
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    std::map<std::string, prism::Finding> st;
    for (auto& f : pp::run_pir({dir / "harness_struct.c"}, cfg))
        if (f.function) st[*f.function] = f;
    return st;
}

prism::CliResult<prism::CliOptions> parse_args(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

}  // namespace

TEST_CASE("pir harness: off by default, every pointer function is NEEDS-HARNESS (Law 6)") {
    if (skip_without_frontend()) return;
    auto st = run(prism::default_config());
    REQUIRE(st.contains("dev_id"));
    for (auto& [name, f] : st) CHECK_MESSAGE(f.status == prism::laws::NEEDS_HARNESS, name);
}

TEST_CASE("pir harness: struct objects, nested pointers, named assumptions (--pir-harness)") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    auto st = run(cfg);
    namespace L = prism::laws;
    for (auto* ok : {"dev_id", "dev_reg_ok", "dev_flags", "dev_inner", "dev_via_helper", "dev_stat0", "list_second"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        // never PROVED: every harness choice is an assumption
        CHECK_MESSAGE(st[ok].status == L::PROVED_ASSUMING, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        const auto a = extra(st[ok], "assumptions");
        CHECK_MESSAGE(a.find("(auto harness)") != std::string::npos, ok);
        CHECK_MESSAGE(a.find("points to the start of a valid object") != std::string::npos, ok);
    }
    // the object has the struct's size from the IR layout (int + int[4] + 2 pointers + unsigned, padded)
    CHECK(extra(st["dev_id"], "assumptions").find("48 bytes") != std::string::npos);
    CHECK(extra(st["dev_id"], "assumptions").find("%struct.dev") != std::string::npos);
    // a violation that needs particular field values or an unchecked index is a missing precondition
    for (auto* bad : {"dev_reg_unchecked", "dev_div", "list_third"}) {
        REQUIRE_MESSAGE(st.contains(bad), bad);
        CHECK_MESSAGE(st[bad].status == L::NEEDS_HARNESS, (std::string(bad) + ": " + st[bad].status));
        CHECK_MESSAGE(st[bad].extra.contains("verdict_under_draft"), bad);
    }
    // the depth bound is an assumption and its effect is named
    CHECK(extra(st["list_third"], "assumptions").find("depth bound 2") != std::string::npos);
    CHECK(extra(st["list_third"], "assumptions").find("unconstrained arbitrary values") != std::string::npos);
}

TEST_CASE("pir harness: a deeper bound reaches the third link, recorded as an assumption") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_harness_depth = 3;
    auto st = run(cfg);
    REQUIRE(st.contains("list_third"));
    CHECK(st["list_third"].status == prism::laws::PROVED_ASSUMING);
    CHECK(extra(st["list_third"], "assumptions").find("depth bound 3") != std::string::npos);
}

TEST_CASE("pir jsonl: record fields, levels, assumptions, Lean coverage") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    const auto path = fs::temp_directory_path() / "prism_pir_harness_test" / "out.jsonl";
    fs::remove_all(path.parent_path());
    cfg.jsonl = path;
    auto st = run(cfg);
    REQUIRE(fs::exists(path));
    std::map<std::string, nlohmann::json> rec;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto j = nlohmann::json::parse(line);
        CHECK(j["schema"] == "prism-pir-jsonl/1");
        if (j["record"] == "function") rec[j["function"].get<std::string>()] = j;
    }
    REQUIRE(rec.size() == st.size());
    auto& r = rec["dev_id"];
    CHECK(r["unit"].get<std::string>().find("harness_struct.c") != std::string::npos);
    CHECK(r["engine"] == "prism-pir");
    CHECK(r["verdict"] == "PROVED-ASSUMING");
    CHECK(r["level"] == "L2");
    CHECK(r["conditional"] == true);
    CHECK(r["harness"]["auto"] == true);
    CHECK(r["harness"]["pointer_parameters_bound"][0] == "d");
    CHECK(r["start_line"].is_number());
    CHECK(r["end_line"].is_number());
    CHECK(r["key"].get<std::string>().find("harness_struct.c:") != std::string::npos);
    CHECK(r["vcs"].is_number());
    CHECK(r["vcs"].get<long long>() > 0);
    CHECK(r["time_s"].is_number());
    CHECK(r["peak_mb"].is_number());
    bool unknown_call_policy = false, harness_line = false;
    for (auto& a : r["assumptions"]) {
        const auto s = a.get<std::string>();
        unknown_call_policy = unknown_call_policy || s.starts_with("unknown-call policy");
        harness_line = harness_line || s.find("(auto harness)") != std::string::npos;
    }
    CHECK(unknown_call_policy);
    CHECK(harness_line);
    // R7: the auto harness is outside the Lean-proved fragment, with the reason
    CHECK(r["lean"]["supported"] == false);
    CHECK(r["lean"]["reason"].get<std::string>().find("automatic pointer harness") != std::string::npos);
    // NEEDS-HARNESS keeps its exact verdict name and the level L0 (compiled)
    CHECK(rec["dev_div"]["verdict"] == "NEEDS-HARNESS");
    CHECK(rec["dev_div"]["level"] == "L0");
    // the summary counts Lean reasons
    std::ifstream sin(path.string() + ".summary.json");
    auto s = nlohmann::json::parse(sin);
    CHECK(s["functions"].get<long long>() == static_cast<long long>(st.size()));
    CHECK(s["lean"]["outside"].get<long long>() > 0);
    CHECK(s["lean"]["outside_by_reason"].is_object());
    fs::remove_all(path.parent_path());
}

TEST_CASE("pir jsonl: a scalar function is inside the Lean fragment") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    const auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    const auto path = fs::temp_directory_path() / "prism_pir_harness_test2" / "out.jsonl";
    fs::remove_all(path.parent_path());
    cfg.jsonl = path;
    (void)pp::run_pir({dir / "overflow.c"}, cfg);
    std::ifstream in(path);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        auto j = nlohmann::json::parse(line);
        if (j["function"] == "add_ok") {
            found = true;
            CHECK(j["verdict"] == "PROVED");
            CHECK(j["level"] == "L2");
            CHECK(j["conditional"] == false);
            CHECK(j["lean"]["supported"] == true);
        }
    }
    CHECK(found);
    fs::remove_all(path.parent_path());
}

TEST_CASE("pir jsonl: proof level mapping and memory-limit outcome") {
    namespace L = prism::laws;
    auto mk = [](std::string_view status, std::string msg = "") {
        prism::Finding f;
        f.stage = "pir";
        f.status = std::string(status);
        f.file = "a.c";
        f.function = "f";
        f.message = std::move(msg);
        return f;
    };
    CHECK(pp::proof_level(mk(L::PROVED)) == "L2");
    CHECK(pp::proof_level(mk(L::PROVED_UNBOUNDED)) == "L2");
    CHECK(pp::proof_level(mk(L::PROVED_CERTIFIED)) == "L2");
    CHECK(pp::proof_level(mk(L::PROVED_ASSUMING)) == "L2");
    CHECK(pp::proof_level(mk(L::BOUNDED)) == "L1");
    CHECK(pp::proof_level(mk(L::FAILED)) == "L0");
    CHECK(pp::proof_level(mk(L::NEEDS_HARNESS)) == "L0");
    CHECK(pp::proof_level(mk(L::UNKNOWN)) == "L0");
    auto cut = mk(L::NEEDS_HARNESS);
    cut.extra["frontend_salvage"] = "cut";
    CHECK(pp::proof_level(cut) == "");
    prism::Finding unit;
    unit.status = std::string(L::ERROR);
    CHECK(pp::proof_level(unit) == "");
    // a check stopped by PRISM's memory budget is a memory-limit result, never a retry with more
    auto ml = mk(L::UNKNOWN, "memory budget reached while checking (process RSS 1700 MB, budget 2048 MB); no proof");
    CHECK(pp::hit_memory_limit(ml));
    CHECK(pp::jsonl_record(ml, prism::default_config())["outcome"] == "memory-limit");
    CHECK(!pp::hit_memory_limit(mk(L::UNKNOWN, "solver gave up")));
    CHECK(!pp::hit_memory_limit(mk(L::PROVED, "memory budget")));
    CHECK(pp::jsonl_record(mk(L::TIMEOUT), prism::default_config())["outcome"] == "timeout");
}

TEST_CASE("cli: --pir-harness, --pir-harness-depth/-elems/-bytes and --jsonl") {
    auto r = parse_args({"fixture.c", "--pir-harness", "--pir-harness-depth", "3", "--pir-harness-elems=8",
                               "--pir-harness-bytes", "128", "--jsonl", "out.jsonl"});
    REQUIRE(r);
    CHECK(r->cfg.pir_harness);
    CHECK(r->cfg.pir_harness_depth == 3);
    CHECK(r->cfg.pir_harness_elems == 8);
    CHECK(r->cfg.pir_harness_bytes == 128);
    CHECK(r->cfg.jsonl.filename() == "out.jsonl");
    auto d = parse_args({"fixture.c"});
    REQUIRE(d);
    CHECK(!d->cfg.pir_harness);
    CHECK(d->cfg.jsonl.empty());
}

namespace {

std::map<std::string, prism::Finding> run_file(prism::Config cfg, const char* file) {
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    std::map<std::string, prism::Finding> st;
    for (auto& f : pp::run_pir({dir / file}, cfg))
        if (f.function) st[*f.function] = f;
    return st;
}

bool carries_auto_harness(const prism::Finding& f) {
    return extra(f, "assumptions").find("(auto harness)") != std::string::npos;
}

}  // namespace

TEST_CASE("pir discharge: off by default, the callee keeps its automatic harness assumption") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    auto st = run_file(cfg, "harness_discharge.c");
    REQUIRE(st.contains("get_id"));
    CHECK(st["get_id"].status == prism::laws::PROVED_ASSUMING);
    CHECK(carries_auto_harness(st["get_id"]));
    CHECK(!st["get_id"].extra.contains("harness_discharge"));
}

TEST_CASE("pir discharge: callers that establish the harness remove the invented assumption (--pir-discharge)") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    auto st = run_file(cfg, "harness_discharge.c");
    namespace L = prism::laws;
    for (auto* ok : {"get_id", "get_in"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        // still PROVED-ASSUMING (the entry functions' assumptions remain), never PROVED
        CHECK_MESSAGE(st[ok].status == L::PROVED_ASSUMING, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        CHECK_MESSAGE(extra(st[ok], "harness_discharge").starts_with("discharged at"),
                      (std::string(ok) + ": " + extra(st[ok], "harness_discharge")));
        CHECK_MESSAGE(!carries_auto_harness(st[ok]), ok);
        CHECK_MESSAGE(extra(st[ok], "assumptions").find("the automatic harness is discharged") != std::string::npos, ok);
        CHECK_MESSAGE(extra(st[ok], "assumptions").find("the function is static, its address is not taken, it is not recursive") !=
                          std::string::npos,
                      ok);
    }
    // the callers have no pointer parameters: nothing is assumed for them
    CHECK(st["user_ok"].status == L::PROVED);
}

TEST_CASE("pir discharge: a caller that breaks the harness makes the discharge fail, and says why") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    auto st = run_file(cfg, "harness_discharge.c");
    struct Bad {
        const char* fn;
        const char* caller;
        const char* what;
    };
    for (const Bad& b : {Bad{"null_callee", "null_bad", "null"}, Bad{"small_callee", "small_bad", "smaller than"},
                         Bad{"uninit_callee", "uninit_bad", "uninitialised"}, Bad{"get_in_null", "nested_bad", "null"}}) {
        REQUIRE_MESSAGE(st.contains(b.fn), b.fn);
        const auto d = extra(st[b.fn], "harness_discharge");
        CHECK_MESSAGE(d.starts_with("not discharged"), (std::string(b.fn) + ": " + d));
        CHECK_MESSAGE(d.find(b.caller) != std::string::npos, (std::string(b.fn) + ": " + d));
        CHECK_MESSAGE(d.find(b.what) != std::string::npos, (std::string(b.fn) + ": " + d));
        CHECK_MESSAGE(carries_auto_harness(st[b.fn]), b.fn);  // the assumption stays
    }
    // one caller passes the same object twice: the harness assumes distinct objects
    const auto two = extra(st["two_callee"], "harness_discharge");
    CHECK_MESSAGE(two.starts_with("not discharged"), two);
    CHECK_MESSAGE(two.find("two_alias_bad") != std::string::npos, two);
    CHECK_MESSAGE(two.find("same object") != std::string::npos, two);
}

TEST_CASE("pir discharge: exported and address-taken functions keep the assumption") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    auto st = run_file(cfg, "harness_discharge.c");
    struct Keep {
        const char* fn;
        const char* why;
    };
    for (const Keep& k : {Keep{"exported_callee", "external linkage"}, Keep{"taken_callee", "address is taken"}}) {
        REQUIRE_MESSAGE(st.contains(k.fn), k.fn);
        const auto d = extra(st[k.fn], "harness_discharge");
        CHECK_MESSAGE(d.starts_with("not discharged"), (std::string(k.fn) + ": " + d));
        CHECK_MESSAGE(d.find(k.why) != std::string::npos, (std::string(k.fn) + ": " + d));
        CHECK_MESSAGE(carries_auto_harness(st[k.fn]), k.fn);
    }
}

TEST_CASE("pir harness: container_of gets its containing object (--pir-container-of)") {
    if (skip_without_frontend()) return;
    namespace L = prism::laws;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    // without the option a negative offset is not harnessable
    auto off = run_file(cfg, "harness_container.c");
    REQUIRE(off.contains("outer_tag"));
    CHECK(off["outer_tag"].status == L::NEEDS_HARNESS);
    CHECK(off["outer_tag"].message.find("container_of") != std::string::npos);
    cfg.pir_container_of = true;
    auto st = run_file(cfg, "harness_container.c");
    for (auto* ok : {"outer_tag", "outer_reg_ok"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        CHECK_MESSAGE(st[ok].status == L::PROVED_ASSUMING, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        const auto a = extra(st[ok], "assumptions");
        CHECK_MESSAGE(a.find("%struct.outer") != std::string::npos, ok);
        CHECK_MESSAGE(a.find("container_of") != std::string::npos, ok);
        CHECK_MESSAGE(a.find("(auto harness)") != std::string::npos, ok);
    }
    // an index the code does not check is a missing precondition, not a defect
    REQUIRE(st.contains("outer_reg_unchecked"));
    CHECK(st["outer_reg_unchecked"].status == L::NEEDS_HARNESS);
    CHECK(st["outer_reg_unchecked"].extra.contains("verdict_under_draft"));
}

TEST_CASE("pir harness: container_of through a helper function's returned pointer (--pir-container-of)") {
    if (skip_without_frontend()) return;
    namespace L = prism::laws;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    // without the option the negative offset is not harnessable: never proved
    auto off = run_file(cfg, "harness_container_helper.c");
    for (auto* k : {"helper_tag", "helper2_tag", "helper_reg_ok"}) {
        REQUIRE_MESSAGE(off.contains(k), k);
        CHECK_MESSAGE(off[k].status == L::NEEDS_HARNESS, (std::string(k) + ": " + off[k].status + " " + off[k].message));
    }
    cfg.pir_container_of = true;
    auto st = run_file(cfg, "harness_container_helper.c");
    for (auto* ok : {"helper_tag", "helper2_tag", "helper_reg_ok"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        CHECK_MESSAGE(st[ok].status == L::PROVED_ASSUMING, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        const auto a = extra(st[ok], "assumptions");
        CHECK_MESSAGE(a.find("%struct.outer") != std::string::npos, ok);
        CHECK_MESSAGE(a.find("container_of") != std::string::npos, ok);
    }
    // the unchecked index is still a missing precondition (NEEDS-HARNESS), never proved
    REQUIRE(st.contains("helper_reg_unchecked"));
    CHECK(st["helper_reg_unchecked"].status == L::NEEDS_HARNESS);
    // an accessor's returned pointer field: the nested object is typed from the caller's uses
    REQUIRE(off.contains("priv_tail"));
    CHECK(off["priv_tail"].status == L::NEEDS_HARNESS);
    REQUIRE(st.contains("priv_tail"));
    CHECK_MESSAGE(st["priv_tail"].status == L::PROVED_ASSUMING, (st["priv_tail"].status + " " + st["priv_tail"].message));
    CHECK(extra(st["priv_tail"], "assumptions").find("%struct.priv") != std::string::npos);
    // container_of then an accessor of the container's driver data: typed from the caller's uses
    REQUIRE(off.contains("drv_x"));
    CHECK(off["drv_x"].status == L::NEEDS_HARNESS);
    REQUIRE(st.contains("drv_x"));
    CHECK_MESSAGE(st["drv_x"].status == L::PROVED_ASSUMING, (st["drv_x"].status + " " + st["drv_x"].message));
    CHECK(extra(st["drv_x"], "assumptions").find("%struct.priv2") != std::string::npos);
    // two different returned offsets: no containing object is derived, never proved
    REQUIRE(st.contains("helper_two_offsets"));
    CHECK(st["helper_two_offsets"].status != L::PROVED_ASSUMING);
    CHECK(st["helper_two_offsets"].status != L::PROVED);
}

TEST_CASE("pir harness: --pir-field-invariants keeps only the rows a proof needs") {
    if (skip_without_frontend()) return;
    namespace L = prism::laws;
    const auto tsv = fs::temp_directory_path() / "prism_field_invariants_test.tsv";
    {
        std::ofstream o(tsv);
        o << "%struct.fidev\t8\t4\trange\t1\t10\tUNVALIDATED\ttest: needed\n";
        o << "%struct.fidev\t12\t4\trange\t0\t5\tUNVALIDATED\ttest: not needed\n";
        o << "%struct.fidev\t0\t8\tvalid\t16\t0\tUNVALIDATED\ttest: not needed\n";
    }
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_field_invariants = tsv;
    auto st = run_file(cfg, "field_invariants.c");
    REQUIRE(st.contains("div_by_field"));
    CHECK_MESSAGE(st["div_by_field"].status == L::PROVED_ASSUMING, (st["div_by_field"].status + " " + st["div_by_field"].message));
    const auto a = extra(st["div_by_field"], "assumptions");
    CHECK(a.find("(%struct.fidev at byte 8) is in [1, 10]") != std::string::npos);
    CHECK(a.find("(%struct.fidev at byte 12)") == std::string::npos);
    CHECK(a.find("(%struct.fidev at byte 0)") == std::string::npos);
    CHECK(extra(st["div_by_field"], "field_invariants_not_needed").find("%struct.fidev@12") != std::string::npos);
    // other - 1 may be 0 under the row [0, 5]: never proved
    REQUIRE(st.contains("div_by_other"));
    CHECK(st["div_by_other"].status != L::PROVED_ASSUMING);
    CHECK(st["div_by_other"].status != L::PROVED);
    std::error_code ec;
    fs::remove(tsv, ec);
}

TEST_CASE("pir harness: a scoped field-invariant row applies only inside its scope") {
    if (skip_without_frontend()) return;
    namespace L = prism::laws;
    auto run_with = [&](const std::string& scope) {
        const auto tsv = fs::temp_directory_path() / "prism_field_invariants_scope_test.tsv";
        {
            std::ofstream o(tsv);
            o << "%struct.fidev\t8\t4\trange\t1\t10\tUNVALIDATED\ttest\t" << scope << "\n";
        }
        auto cfg = prism::default_config();
        cfg.pir_harness = true;
        cfg.pir_field_invariants = tsv;
        auto st = run_file(cfg, "field_invariants.c");
        std::error_code ec;
        fs::remove(tsv, ec);
        REQUIRE(st.contains("div_by_field"));
        return st["div_by_field"];
    };
    // in scope: the function, its unit, a directory glob
    for (const char* sc : {"fn:div_by_*", "unit:*/field_invariants.c", "module:*/testdata/pir/*"}) {
        auto f = run_with(sc);
        CHECK_MESSAGE(f.status == L::PROVED_ASSUMING, (std::string(sc) + ": " + f.status + " " + f.message));
        CHECK_MESSAGE(extra(f, "assumptions").find(std::string("scope ") + sc) != std::string::npos, sc);
    }
    // out of scope: another function, another unit, another directory: the row does not apply, never proved
    for (const char* sc : {"fn:other_function", "unit:*/elsewhere.c", "module:*/drivers/net/*"}) {
        auto f = run_with(sc);
        CHECK_MESSAGE(f.status != L::PROVED_ASSUMING, sc);
        CHECK_MESSAGE(f.status != L::PROVED, sc);
        CHECK_MESSAGE(extra(f, "assumptions").find("field-invariant:") == std::string::npos, sc);
    }
    // the global row (no scope) says so
    auto g = run_with("");
    CHECK(extra(g, "assumptions").find("scope global") != std::string::npos);
}

TEST_CASE("pir discharge: container_of obligations, the member of a real outer, not a lone struct") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_container_of = true;
    cfg.pir_discharge = true;
    auto st = run_file(cfg, "harness_container.c");
    REQUIRE(st.contains("tag_of"));
    CHECK_MESSAGE(extra(st["tag_of"], "harness_discharge").starts_with("discharged at"),
                  extra(st["tag_of"], "harness_discharge"));
    CHECK(!carries_auto_harness(st["tag_of"]));
    REQUIRE(st.contains("tag_of_bad"));
    const auto d = extra(st["tag_of_bad"], "harness_discharge");
    CHECK_MESSAGE(d.starts_with("not discharged"), d);
    CHECK_MESSAGE(d.find("container_user_bad") != std::string::npos, d);
    CHECK(carries_auto_harness(st["tag_of_bad"]));
}

TEST_CASE("pir harness: a vacuous harness is never a proof (--pir-vacuity)") {
    if (skip_without_frontend()) return;
    namespace L = prism::laws;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    // the problem: without the check the contradictory precondition "proves" the function
    auto off = run_file(cfg, "harness_vacuity.c");
    REQUIRE(off.contains("vac_contradiction"));
    CHECK(off["vac_contradiction"].status == L::PROVED_ASSUMING);
    cfg.pir_vacuity = true;
    auto st = run_file(cfg, "harness_vacuity.c");
    for (auto* ok : {"vac_ok", "vac_plain"}) {
        REQUIRE_MESSAGE(st.contains(ok), ok);
        CHECK_MESSAGE(st[ok].status == L::PROVED_ASSUMING, (std::string(ok) + ": " + st[ok].status + " " + st[ok].message));
        CHECK_MESSAGE(extra(st[ok], "non_vacuity").starts_with("checked"),
                      (std::string(ok) + ": " + extra(st[ok], "non_vacuity")));
    }
    REQUIRE(st.contains("vac_contradiction"));
    CHECK(st["vac_contradiction"].status == L::NEEDS_HARNESS);
    CHECK(st["vac_contradiction"].message.find("vacuous") != std::string::npos);
    CHECK(extra(st["vac_contradiction"], "non_vacuity").starts_with("VACUOUS"));
    CHECK(st["vac_contradiction"].extra.contains("verdict_before_vacuity"));
}

TEST_CASE("cli: --pir-discharge, --pir-container-of, --pir-vacuity") {
    auto r = parse_args({"fixture.c", "--pir-harness", "--pir-discharge", "--pir-container-of", "--pir-vacuity"});
    REQUIRE(r);
    CHECK(r->cfg.pir_discharge);
    CHECK(r->cfg.pir_container_of);
    CHECK(r->cfg.pir_vacuity);
    auto d = parse_args({"fixture.c", "--pir-harness"});
    REQUIRE(d);
    CHECK(!d->cfg.pir_discharge);
    CHECK(!d->cfg.pir_container_of);
    CHECK(!d->cfg.pir_vacuity);
}

TEST_CASE("pir discharge: the obligation names in the Lean model are the properties the code emits") {
    // proofs/Prism/Discharge.lean (`obligationProps`, theorem `obligation_iff`) models the checks of
    // MemTr::obligation_object / obligation_distinct; the names must stay in step.
    const auto root = fs::path(PRISM_SOURCE_DIR);
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto lean = slurp(root / "proofs" / "Prism" / "Discharge.lean");
    const auto code = slurp(root / "src" / "prism" / "pir" / "translate_mem.cpp");
    REQUIRE(!lean.empty());
    REQUIRE(!code.empty());
    const auto at = lean.find("def obligationProps");
    REQUIRE(at != std::string::npos);
    const auto end = lean.find(']', at);
    std::vector<std::string> lean_names;
    for (auto p = lean.find('"', at); p != std::string::npos && p < end; p = lean.find('"', p + 1)) {
        const auto q = lean.find('"', p + 1);
        lean_names.push_back(lean.substr(p + 1, q - p - 1));
        p = q;
    }
    CHECK(lean_names.size() == 9);
    for (auto& n : lean_names) CHECK_MESSAGE(code.find("\"" + n + "\"") != std::string::npos, n);
    // and every ob-* property the code emits is in the model
    std::size_t n_code = 0;
    for (auto p = code.find("\"ob-"); p != std::string::npos; p = code.find("\"ob-", p + 1)) {
        const auto q = code.find('"', p + 1);
        const auto name = code.substr(p + 1, q - p - 1);
        CHECK_MESSAGE(std::find(lean_names.begin(), lean_names.end(), name) != lean_names.end(), name);
        ++n_code;
    }
    CHECK(n_code >= 9);
}
