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
